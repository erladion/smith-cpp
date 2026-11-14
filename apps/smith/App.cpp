#include "App.h"

#include "core/Errors.h"
#include "core/Jobs.h"
#include "core/Tool.h"
#include "tui/Markdown.h"
#include "tui/Text.h"
#include "frontend/Commands.h"
#include "Connection.h"
#include "frontend/Duration.h"
#include "frontend/Greetings.h"
#include "frontend/Replay.h"
#include "SessionCommands.h"
#include "WorktreeCommands.h"
#include "frontend/Worktrees.h"
#include "View.h"

#include <QCoreApplication>
#include <QRegularExpression>

#include <atomic>

namespace Smith::Ui {

using Tui::DIM;
using Tui::fg;
using Tui::Key;
using Tui::KeyName;
using Tui::Line;
using Tui::prefixed;
using Tui::REASONING;
using Tui::Span;
using Tui::Style;
using Tui::truncate;
using Tui::wrap;

namespace {

// Columns and rows of the rain shown while the conversation is compacted (matrix theme).
constexpr int STRIP_COLS = 22;
constexpr int STRIP_ROWS = 3;
// Spinner frames per step of the status line's code stream.
constexpr int STREAM_EVERY = 4;

} // namespace

App::App(TuiOptions options, QObject* parent)
    : QObject(parent), m_options(std::move(options)),
      m_renderer(Tui::Screen{[this](const QString& text) { m_terminal.write(text); }, [this]() { return m_terminal.columns(); },
                             [this]() { return m_terminal.rows(); }}) {
  m_pServer = m_options.server;
  m_project = m_options.project;

  // The agent calls these from its own threads.
  m_hooks.onEvent = [this](const AgentEvent& event) { QMetaObject::invokeMethod(this, [this, event]() { onEvent(event); }, Qt::QueuedConnection); };
  m_hooks.approve = [this](const ApprovalRequest& request) { return approveFromWorker(request); };
  m_hooks.ask = [this](const Question& question) { return askFromWorker(question); };

  m_pAgent = m_options.createAgent(m_hooks, std::nullopt);
  m_pConnection = std::make_unique<ConnectionCommands>(*this);
  m_pSessions = std::make_unique<SessionCommands>(*this, *m_pConnection);
  m_pWorktrees = std::make_unique<WorktreeCommands>(*this);
  if (m_options.jobs) {
    connect(m_options.jobs.get(), &BackgroundJobs::changed, this, &App::requestRender, Qt::QueuedConnection);
  }

  m_renderTimer.setSingleShot(true);
  m_renderTimer.setInterval(16);
  connect(&m_renderTimer, &QTimer::timeout, this, &App::render);
  m_ticker.setInterval(80);
  connect(&m_ticker, &QTimer::timeout, this, [this]() {
    m_frame++;
    if (m_pStrip) {
      m_pStrip->step();
    }
    requestRender();
  });
  m_resizeTimer.setSingleShot(true);
  m_resizeTimer.setInterval(100);
  connect(&m_resizeTimer, &QTimer::timeout, this, &App::reprint);
  m_rainTimer.setInterval(33);
  connect(&m_rainTimer, &QTimer::timeout, this, [this]() {
    if (!m_pFullRain) {
      return;
    }
    m_pFullRain->step();
    m_terminal.write("\x1b[?2026h" + m_pFullRain->render() + "\x1b[?2026l");
    if (m_pFullRain->done()) {
      stopRain();
    }
  });

  connect(&m_terminal, &Tui::Terminal::input, this, &App::onInput);
  connect(&m_terminal, &Tui::Terminal::resized, this, &App::onResize);
  connect(&m_terminal, &Tui::Terminal::terminate, this, &App::exitApp);
}

App::~App() {
  exitApp();
}

void App::run() {
  m_terminal.enableRaw();
  m_terminal.setReading(true);
  auto start = [this]() {
    // Hide the real cursor (we draw our own), turn on bracketed paste, and ask
    // for the kitty keyboard protocol so Shift+Enter is distinguishable from
    // Enter. Terminals that don't know a request ignore it.
    m_terminal.write("\x1b[?25l\x1b[?2004h\x1b[>1u");
    Start how;
    how.resumed = !m_options.resumed.isEmpty();
    how.lastUsed = m_options.resumedLastUsed;
    printBanner(how);
    for (const QString& warning : m_options.warnings) {
      print(Entry::notice(NoticeLevel::Warn, warning));
    }
    for (const Entry& entry : historyToEntries(m_options.resumed, m_project.tools)) {
      print(entry);
    }
    refreshModels();
    refreshTasks();
    render();
  };
  if (matrix() && Tui::colorsEnabled()) {
    playRain(Tui::Rain::Options{"AGENT SMITH"}, start);
  } else {
    start();
  }
}

// ---- for the command modules ----

void App::refreshModels() {
  const ServerPtr target = m_pServer;
  auto* thread = QThread::create([this, target]() {
    try {
      const QList<ModelInfo> models = target->listModels();
      QStringList ids;
      for (const ModelInfo& model : models) {
        ids << model.id;
      }
      QMetaObject::invokeMethod(this, [this, ids]() { m_suggestions.models = ids; }, Qt::QueuedConnection);
    } catch (const std::exception&) {
      // Completion just won't offer models.
    }
  });
  m_workers << thread;
  connect(thread, &QThread::finished, this, [this, thread]() {
    m_workers.removeAll(thread);
    thread->deleteLater();
  });
  thread->start();
}

void App::print(const Entry& entry) {
  m_transcript.append(entry);
  m_printedWidth = width();
  m_renderer.print(entryLines(entry, width()));
  requestRender();
}

void App::startOver(const Start& start, const QList<Entry>& entries) {
  QList<Entry> all = entries;
  // Background jobs belong to the conversation that started them.
  const int stopped = m_options.jobs ? m_options.jobs->clear() : 0;
  if (stopped) {
    all.prepend(Entry::notice(NoticeLevel::Info, QString("Stopped %1 from the previous session.").arg(plural(stopped, "background job"))));
  }
  m_renderer.reset();
  m_transcript.clear();
  m_live.reset();
  m_tokens.reset();
  m_limit.reset();
  printBanner(start);
  for (const Entry& entry : all) {
    print(entry);
  }
  // The project may be another repository now.
  refreshTasks();
}

void App::refreshTasks() {
  const QString cwd = m_project.cwd;
  auto* thread = QThread::create([this, cwd]() {
    QStringList tasks;
    try {
      for (const Worktree& worktree : listWorktrees(cwd)) {
        if (!worktree.task.isEmpty()) {
          tasks << worktree.task;
        }
      }
    } catch (const std::exception&) {
      // Not a repository: nothing to complete.
    }
    QMetaObject::invokeMethod(this, [this, tasks]() { m_suggestions.tasks = tasks; }, Qt::QueuedConnection);
  });
  m_workers << thread;
  connect(thread, &QThread::finished, this, [this, thread]() {
    m_workers.removeAll(thread);
    thread->deleteLater();
  });
  thread->start();
}

void App::promptText(const QString& title, const QString& initial, std::function<void(const QString&)> onSubmit) {
  m_textPrompt = TextPrompt{title, std::move(onSubmit), m_editor.value()};
  m_editor.setValue(initial);
  m_suggestions.reset();
  requestRender();
}

void App::openPicker(std::unique_ptr<Tui::Picker> picker, std::function<void(const QString&)> onPick, std::function<void(const QString&)> onAction) {
  m_picker = OpenPicker{std::move(picker), std::move(onPick), std::move(onAction)};
  requestRender();
}

void App::withActivity(const QString& activity, std::function<std::function<void()>()> work) {
  m_busy = true;
  m_activity = activity;
  requestRender();
  auto next = std::make_shared<std::function<void()>>();
  auto error = std::make_shared<QString>();
  const QString baseUrl = m_pServer->baseUrl();
  auto* thread = QThread::create([work, next, error, baseUrl]() {
    try {
      *next = work();
    } catch (const std::exception& failure) {
      *error = describeError(failure, baseUrl);
    }
  });
  m_workers << thread;
  connect(thread, &QThread::finished, this, [this, thread, next, error]() {
    m_workers.removeAll(thread);
    thread->deleteLater();
    if (m_closed) {
      return;
    }
    m_activity.clear();
    m_busy = false;
    if (!error->isEmpty()) {
      print(Entry::notice(NoticeLevel::Error, *error));
    } else if (*next) {
      (*next)();
    }
    requestRender();
  });
  thread->start();
}

void App::printBanner(const Start& start) {
  GreetingContext context;
  context.resumed = start.resumed;
  context.lastUsed = start.lastUsed;
  if (!start.resumed) {
    // A folder no other session has worked in gets its own welcome.
    const QString current = m_options.sessions.currentId();
    context.newFolder = true;
    for (const SessionInfo& info : m_options.sessions.list(false)) {
      if (info.id != current) {
        context.newFolder = false;
      }
    }
  }
  const QString name = m_options.sessions.currentName();
  Entry banner;
  banner.kind = Entry::Kind::Banner;
  banner.model = m_pAgent->model();
  banner.server = m_pServer->label();
  banner.cwd = m_project.cwd;
  banner.session = name.isEmpty() ? m_options.sessions.currentId() : name + " · " + m_options.sessions.currentId();
  banner.resumed = start.resumed;
  banner.sandboxActive = m_options.sandbox->active();
  banner.sandboxDescription = m_options.sandbox->describe(m_project.cwd);
  banner.instructions = m_project.instructions;
  for (const Skill& skill : m_project.skills) {
    banner.skills << skill.name;
  }
  if (matrix()) {
    banner.greeting = greeting(context);
  }
  print(banner);
}

// ---- terminal plumbing ----

void App::onInput(const QString& data) {
  if (m_pFullRain) {
    // Any key ends the rain, and is used up doing so.
    stopRain();
    return;
  }
  for (const Key& key : m_keys.feed(data)) {
    if (m_closed) {
      return;
    }
    onKey(key);
  }
  requestRender();
}

void App::onResize() {
  if (m_pFullRain) {
    stopRain();
    return;
  }
  // Wait for dragging to settle, then reprint once.
  m_resizeTimer.start();
}

void App::reprint() {
  if (m_closed || m_pFullRain) {
    return;
  }
  if (width() != m_printedWidth) {
    m_renderer.reset();
    m_printedWidth = width();
    for (const Entry& entry : m_transcript) {
      m_renderer.print(entryLines(entry, width()));
    }
  }
  render();
}

void App::restoreTerminal() {
  m_terminal.write("\x1b[<u\x1b[?2004l\x1b[?25h");
  m_terminal.restore();
}

void App::exitApp() {
  if (m_closed) {
    return;
  }
  m_closed = true;
  m_closing = true;
  if (m_pCancel) {
    m_pCancel->cancel();
  }
  if (m_approval) {
    m_approval->answer->resolve(ApprovalDecision::Deny);
  }
  if (m_question) {
    m_question->answer->resolve(Answer{});
  }
  m_ticker.stop();
  m_renderTimer.stop();
  m_resizeTimer.stop();
  m_rainTimer.stop();
  if (m_pFullRain) {
    m_terminal.write("\x1b[0m\x1b[?1049l");
    m_pFullRain.reset();
  }
  m_renderer.finish();
  restoreTerminal();
  m_terminal.setReading(false);
  // The agent's threads stop at their next check of the cancel token.
  for (QThread* thread : m_workers) {
    thread->wait();
  }
  Q_EMIT finished();
}

bool App::matrix() const {
  return m_options.theme == Theme::Matrix;
}

bool App::streaming() const {
  return matrix() && m_options.codeStream && Tui::colorsEnabled() && !m_busy;
}

int App::width() const {
  return std::max(20, m_terminal.columns());
}

bool App::prompting() const {
  return m_approval.has_value() || m_question.has_value();
}

bool App::agentRunning() const {
  return m_busy && m_pCancel != nullptr;
}

// ---- drawing ----

void App::requestRender() {
  if (m_closed || m_renderTimer.isActive()) {
    return;
  }
  m_renderTimer.start();
}

void App::render() {
  if (m_closed || m_pFullRain) {
    return;
  }
  // The animations only need a timer while one is on screen.
  const bool animating = (!m_activity.isEmpty() && !prompting()) || streaming();
  if (animating && !m_ticker.isActive()) {
    m_ticker.start();
  } else if (!animating && m_ticker.isActive()) {
    m_ticker.stop();
  }
  m_renderer.draw(liveLines());
}

QList<Line> App::liveLines() {
  const int w = width();
  QList<Line> lines;
  if (m_live && !m_live->text.isEmpty()) {
    lines.append(Line{});
    if (m_live->kind == BlockKind::Text) {
      lines.append(Tui::markdownLines(m_live->text, w));
    } else {
      lines.append(wrap(Line{Span{m_live->text, REASONING}}, w));
    }
  }
  if (!m_activity.isEmpty() && !prompting()) {
    const QString elapsed = m_turn ? " · " + formatDuration(m_turn->started.elapsed()) : QString();
    if (m_compacting && matrix() && Tui::colorsEnabled()) {
      // A strip of rain, with the activity beside its middle row.
      if (!m_pStrip) {
        Tui::Rain::Options options;
        options.frames = std::numeric_limits<double>::infinity();
        m_pStrip = std::make_unique<Tui::Rain>(STRIP_COLS, STRIP_ROWS, options);
        // Already raining when it appears.
        for (int i = 0; i < 12; i++) {
          m_pStrip->step();
        }
      }
      QList<Line> rain = m_pStrip->lines();
      rain[1].append(Span{"  Purging old memories…" + elapsed, DIM});
      lines.append(Line{});
      for (const Line& line : rain) {
        lines.append(truncate(line, w));
      }
    } else {
      const Spinner& spinner = spinnerFor(m_options.theme);
      lines.append(Line{});
      lines.append(truncate(Line{Span{spinner.frames[m_frame % spinner.frames.size()], fg(spinner.color)}, Span{" " + m_activity + elapsed, DIM}}, w));
    }
    // The step the agent is on, from its checklist.
    const TodoList todos = m_pAgent->todos();
    for (const Todo& todo : todos) {
      if (todo.status == TodoStatus::InProgress && m_busy) {
        const int done = todos.size() - unfinished(todos).size();
        lines.append(truncate(Line{Span{QString("  ▶ %1/%2 ").arg(done + 1).arg(todos.size()), fg(Color::Yellow)}, Span{todo.content, DIM}}, w));
        break;
      }
    }
  }
  if (m_approval) {
    lines.append(Line{});
    lines.append(approvalBox(m_approval->request, w, m_terminal.rows()));
  } else if (m_question) {
    lines.append(Line{});
    lines.append(questionBox(m_question->question, m_question->selected, w));
    const QList<Line> answer = prefixed(Line{Span{"› ", fg(Color::Cyan)}}, Line{Span{"  ", {}}}, m_editor.render("Your answer"), w - 4);
    lines.append(box(answer, Color::Gray, w, true));
  } else if (m_picker) {
    lines.append(Line{});
    lines.append(box(m_picker->picker->lines(), Color::Cyan, w));
  } else if (m_textPrompt && !m_busy) {
    lines.append(Line{});
    lines.append(truncate(Line{Span{m_textPrompt->title, Style{.bold = true}}, Span{"  enter to confirm · esc cancels", DIM}}, w));
    const QList<Line> prompt = prefixed(Line{Span{"› ", fg(Color::Yellow)}}, Line{Span{"  ", {}}}, m_editor.render({}), w - 4);
    lines.append(box(prompt, Color::Yellow, w, true));
  } else if (!m_busy || agentRunning()) {
    const QStringList queued = m_pAgent->queuedMessages();
    if (!queued.isEmpty()) {
      lines.append(Line{});
      for (const QString& text : queued) {
        lines.append(prefixed(Line{Span{"› ", DIM}}, Line{Span{"  ", {}}}, Line{Span{text, DIM}}, w));
      }
      lines.append(Line{Span{"  queued: the agent gets this at its next step", Style{.dim = true, .italic = true}}});
    }
    const QString placeholder = m_busy ? "Type to add to what the agent is doing (enter queues it)" : "Ask for something, or /help";
    const QList<Line> prompt = prefixed(Line{Span{"› ", fg(Color::Cyan)}}, Line{Span{"  ", {}}}, m_editor.render(placeholder), w - 4);
    lines.append(Line{});
    lines.append(box(prompt, Color::Gray, w, true));
    lines.append(m_suggestions.lines(currentSuggestions(), w));
  }
  Status status;
  status.endpoint = m_pServer->name();
  status.model = m_pAgent->model();
  status.reasoning = m_pAgent->reasoning();
  status.tokens = m_tokens;
  status.limit = m_limit;
  status.sandboxActive = m_options.sandbox->active();
  status.sandboxType = m_options.sandbox->type();
  status.network = m_options.sandbox->network();
  status.busy = m_busy;
  status.queueing = agentRunning();
  status.jobs = m_options.jobs ? m_options.jobs->running() : 0;
  status.worktreeBranch = m_project.worktreeBranch;
  if (streaming()) {
    const int frame = m_frame;
    status.filler = [frame](int n) { return Tui::codeStream(frame / STREAM_EVERY, n); };
  }
  lines.append(statusLine(status, w));
  return lines;
}

// ---- agent events ----

void App::onEvent(const AgentEvent& event) {
  if (m_closed) {
    return;
  }
  switch (event.type) {
  case AgentEvent::Type::Delta:
    setCompacting(false);
    m_activity.clear();
    appendDelta(event.kind, event.text);
    break;
  case AgentEvent::Type::ResponseEnd:
    flushLive();
    break;
  case AgentEvent::Type::ToolPrepare:
    setCompacting(false);
    flushLive();
    m_activity = "Preparing " + event.name + "…";
    break;
  case AgentEvent::Type::UserMessage:
    flushLive();
    print(Entry::simple(Entry::Kind::User, event.text));
    break;
  case AgentEvent::Type::ToolStart:
    setCompacting(false);
    m_running.insert(event.id, event.name + " " + event.summary);
    m_runningOrder.removeAll(event.id);
    m_runningOrder << event.id;
    {
      QStringList labels;
      for (const QString& id : m_runningOrder) {
        labels << m_running.value(id);
      }
      m_activity = runningLabel(labels);
    }
    break;
  case AgentEvent::Type::ToolEnd: {
    setCompacting(false);
    if (m_turn) {
      m_turn->toolCalls++;
    }
    m_running.remove(event.id);
    m_runningOrder.removeAll(event.id);
    QStringList labels;
    for (const QString& id : m_runningOrder) {
      labels << m_running.value(id);
    }
    m_activity = runningLabel(labels);
    // The checklist itself was printed when it changed.
    if (event.name == TODO_TOOL && !event.isError) {
      break;
    }
    Entry entry;
    entry.kind = Entry::Kind::Tool;
    entry.name = event.name;
    entry.summary = event.summary;
    entry.output = event.output;
    entry.isError = event.isError;
    entry.diff = event.diff;
    print(entry);
    break;
  }
  case AgentEvent::Type::Context:
    m_tokens = event.tokens;
    m_limit = event.limit;
    break;
  case AgentEvent::Type::Activity:
    setCompacting(event.compacting);
    // Progress from inside a tool (a sub-agent); say so when several run at once.
    m_activity = m_running.size() > 1 ? QString("%1 in parallel · %2").arg(m_running.size()).arg(event.text) : event.text;
    break;
  case AgentEvent::Type::Notice:
    flushLive();
    print(Entry::notice(event.level, event.text));
    break;
  case AgentEvent::Type::Todos:
    if (m_busy) {
      flushLive();
      Entry entry;
      entry.kind = Entry::Kind::Todos;
      entry.todos = event.todos;
      print(entry);
    }
    break;
  case AgentEvent::Type::Done:
    if (m_turn) {
      m_turn->reason = doneReasonName(event.reason);
    }
    break;
  }
  requestRender();
}

void App::appendDelta(BlockKind kind, const QString& text) {
  // Streamed text stays in the live region until a paragraph is complete;
  // then it is printed for good, so the part being redrawn stays small.
  if (m_live && m_live->kind != kind) {
    flushLive();
  }
  QString next;
  if (m_live) {
    next = m_live->text + text;
  } else {
    static const QRegularExpression leading(R"(^\s+)");
    next = QString(text).remove(leading);
  }
  const int cut = Tui::safeSplit(next);
  if (cut > 0) {
    QString done = next.left(cut);
    while (!done.isEmpty() && done.back().isSpace()) {
      done.chop(1);
    }
    print(Entry::simple(kind == BlockKind::Text ? Entry::Kind::Text : Entry::Kind::Reasoning, done));
    next = next.mid(cut);
    while (next.startsWith('\n')) {
      next.remove(0, 1);
    }
  }
  m_live = Live{kind, next};
}

void App::flushLive() {
  if (m_live && !m_live->text.trimmed().isEmpty()) {
    QString text = m_live->text;
    while (!text.isEmpty() && text.back().isSpace()) {
      text.chop(1);
    }
    print(Entry::simple(m_live->kind == BlockKind::Text ? Entry::Kind::Text : Entry::Kind::Reasoning, text));
  }
  m_live.reset();
}

void App::setCompacting(bool on) {
  m_compacting = on;
  if (!on) {
    m_pStrip.reset();
  }
}

// ---- approvals and questions ----

ApprovalDecision App::approveFromWorker(const ApprovalRequest& request) {
  auto pending = std::make_shared<Pending<ApprovalDecision>>();
  QMetaObject::invokeMethod(
      this,
      [this, request, pending]() {
        if (m_closed) {
          pending->resolve(ApprovalDecision::Deny);
          return;
        }
        m_approval = PendingApproval{request, pending};
        requestRender();
      },
      Qt::QueuedConnection);
  // Once the app has closed, nobody will answer.
  return pending->wait([this]() { return m_closing.load(); }, ApprovalDecision::Deny);
}

Answer App::askFromWorker(const Question& question) {
  auto pending = std::make_shared<Pending<Answer>>();
  QMetaObject::invokeMethod(
      this,
      [this, question, pending]() {
        if (m_closed) {
          pending->resolve(Answer{});
          return;
        }
        // Whatever was typed meanwhile (to queue) stays for after; the answer starts empty.
        m_stashed = m_editor.value();
        m_editor.setValue({});
        m_question = PendingQuestion{question, 0, pending};
        requestRender();
      },
      Qt::QueuedConnection);
  return pending->wait([this]() { return m_closing.load(); }, Answer{});
}

void App::decide(ApprovalDecision decision) {
  if (!m_approval) {
    return;
  }
  const auto pending = m_approval->answer;
  m_approval.reset();
  pending->resolve(decision);
}

void App::answer(const Answer& value) {
  if (!m_question) {
    return;
  }
  const auto pending = m_question->answer;
  m_question.reset();
  m_editor.setValue(m_stashed);
  m_stashed.clear();
  pending->resolve(value);
}

void App::onQuestionKey(const Key& key) {
  PendingQuestion& q = *m_question;
  const bool typing = !m_editor.value().isEmpty();
  const int n = q.question.options.size();
  static const QRegularExpression digit("^[1-9]$");
  if (key.name == KeyName::Escape) {
    answer(Answer{});
    if (m_pCancel) {
      m_pCancel->cancel();
    }
  } else if (!typing && n > 0 && (key.name == KeyName::Up || key.name == KeyName::Down)) {
    q.selected = (q.selected + (key.name == KeyName::Up ? n - 1 : 1)) % n;
  } else if (!typing && key.name == KeyName::Text && digit.match(key.text).hasMatch() && key.text.toInt() <= n) {
    answer(Answer{Answer::Kind::Option, key.text.toInt() - 1, {}});
  } else if (!typing && key.name == KeyName::Return && n > 0) {
    answer(Answer{Answer::Kind::Option, q.selected, {}});
  } else if (m_editor.handle(key)) {
    const QString text = m_editor.commit().trimmed();
    if (!text.isEmpty()) {
      answer(Answer{Answer::Kind::Text, 0, text});
    }
  }
}

// ---- keys ----

QList<Suggestion> App::currentSuggestions() const {
  if (m_busy || prompting() || m_picker || m_textPrompt) {
    return {};
  }
  QStringList endpoints;
  for (const Endpoint& endpoint : m_options.endpoints) {
    endpoints << endpoint.name;
  }
  return m_suggestions.items(m_editor.value(), m_project.cwd, endpoints);
}

void App::onKey(const Key& key) {
  if (key.isCtrl('c')) {
    exitApp();
    return;
  }

  if (m_approval) {
    const QString letter = key.name == KeyName::Text ? key.text.toLower() : QString();
    if (letter == "y") {
      decide(ApprovalDecision::Allow);
    } else if (letter == "a") {
      decide(ApprovalDecision::AllowAlways);
    } else if (letter == "n") {
      decide(ApprovalDecision::Deny);
    } else if (key.name == KeyName::Escape) {
      decide(ApprovalDecision::Deny);
      if (m_pCancel) {
        m_pCancel->cancel();
      }
    }
    return;
  }

  if (m_question) {
    onQuestionKey(key);
    return;
  }

  if (m_picker) {
    const Tui::Picker::Outcome outcome = m_picker->picker->handle(key);
    if (outcome == Tui::Picker::Outcome::Cancel) {
      m_picker.reset();
    } else if (outcome == Tui::Picker::Outcome::Chose) {
      // Cleared first: choosing can open the next picker (the folder browser).
      const Tui::Picker::Chosen chosen = m_picker->picker->chosen();
      const auto onPick = m_picker->onPick;
      const auto onAction = m_picker->onAction;
      m_picker.reset();
      if (chosen.action) {
        if (onAction) {
          onAction(chosen.value);
        }
      } else if (onPick) {
        onPick(chosen.value);
      }
    }
    return;
  }

  if (m_textPrompt) {
    if (key.name == KeyName::Escape) {
      m_editor.setValue(m_textPrompt->stashed);
      m_textPrompt.reset();
      print(Entry::notice(NoticeLevel::Info, "Cancelled."));
    } else if (m_editor.handle(key)) {
      // Not committed: an answer like this doesn't belong in the ↑ history.
      const QString value = m_editor.value();
      const TextPrompt prompt = *m_textPrompt;
      m_textPrompt.reset();
      m_editor.setValue(prompt.stashed);
      prompt.onSubmit(value);
    }
    return;
  }

  if (m_busy) {
    if (key.name == KeyName::Escape) {
      if (m_pCancel) {
        m_pCancel->cancel();
      }
    } else if (agentRunning() && m_editor.handle(key)) {
      queueMessage(m_editor.commit());
    }
    return;
  }

  if (key.isCtrl('d') && m_editor.value().isEmpty()) {
    exitApp();
    return;
  }

  // While command suggestions are showing, arrows pick one, tab completes it and enter runs it.
  QString toRun;
  const SuggestionList::Outcome handled =
      m_suggestions.handle(key, currentSuggestions(), m_editor.value(), [this](const QString& value) { m_editor.setValue(value); }, toRun);
  if (handled == SuggestionList::Outcome::Handled) {
    return;
  }
  if (handled == SuggestionList::Outcome::Run) {
    m_editor.setValue(toRun);
    submit(m_editor.commit());
    return;
  }

  const QString before = m_editor.value();
  if (m_editor.handle(key)) {
    submit(m_editor.commit());
  }
  if (m_editor.value() != before) {
    m_suggestions.reset();
  }
}

// ---- turns and commands ----

void App::queueMessage(const QString& value) {
  const QString text = value.trimmed();
  if (text.isEmpty()) {
    return;
  }
  if (text.startsWith('/')) {
    m_editor.setValue(value);
    print(Entry::notice(NoticeLevel::Warn, "Commands work once the agent is done. Your command is still in the input box."));
    return;
  }
  m_pAgent->queue(text);
}

void App::submit(const QString& value) {
  const QString text = value.trimmed();
  if (text.isEmpty()) {
    return;
  }
  if (text.startsWith('/')) {
    runCommand(text);
    return;
  }
  print(Entry::simple(Entry::Kind::User, text));
  m_turn = TurnInfo{};
  m_turn->started.start();
  const std::shared_ptr<Agent> current = m_pAgent;
  runBusy("Thinking…", [current, text](const CancelPtr& cancel) { current->send(text, cancel); },
          [this]() {
            const std::optional<TurnInfo> turn = m_turn;
            m_turn.reset();
            if (turn && !m_closed) {
              print(Entry::simple(Entry::Kind::Meta, turnSummary(turn->reason, turn->started.elapsed(), turn->toolCalls)));
              if (turn->reason == "interrupted" && matrix()) {
                print(Entry::simple(Entry::Kind::Quote, INTERRUPTED));
              }
            }
          });
}

void App::runCommand(const QString& text) {
  static const QRegularExpression whitespace(R"(\s+)");
  QStringList parts = text.split(whitespace, Qt::SkipEmptyParts);
  const QString command = parts.takeFirst();
  const QString arg = parts.join(' ');
  if (command == "/exit" || command == "/quit") {
    exitApp();
  } else if (command == "/help") {
    print(Entry::notice(NoticeLevel::Info, commandHelp()));
  } else if (command == "/new" || command == "/clear") {
    m_pSessions->newSession(arg);
  } else if (command == "/sessions" || command == "/resume") {
    m_pSessions->openSessionPicker(arg == "all");
  } else if (command == "/name") {
    m_pSessions->nameSession(arg);
  } else if (command == "/open") {
    m_pSessions->openProjectPicker();
  } else if (command == "/worktree") {
    m_pWorktrees->create(arg);
  } else if (command == "/worktrees") {
    m_pWorktrees->openPicker();
  } else if (command == "/land") {
    m_pWorktrees->land(arg);
  } else if (command == "/drop") {
    m_pWorktrees->drop(arg);
  } else if (command == "/endpoint") {
    if (arg.isEmpty()) {
      m_pConnection->openEndpointPicker();
    } else {
      m_pConnection->switchEndpoint(arg);
    }
  } else if (command == "/model") {
    if (arg.isEmpty()) {
      m_pConnection->openModelPicker();
    } else {
      m_pConnection->switchModel(arg);
    }
  } else if (command == "/reasoning") {
    if (arg.isEmpty()) {
      m_pConnection->openReasoningPicker();
    } else {
      m_pConnection->setReasoning(arg);
    }
  } else if (command == "/network") {
    setNetwork(arg);
  } else if (command == "/undo") {
    undo();
  } else if (command == "/skills") {
    listSkills();
  } else if (command == "/jobs") {
    manageJobs(arg);
  } else if (command == "/compact") {
    setCompacting(true);
    const std::shared_ptr<Agent> current = m_pAgent;
    runBusy("Compacting…", [current](const CancelPtr& cancel) { current->compactNow(cancel); });
  } else if (command == "/redpill") {
    // Not listed in /help.
    if (!Tui::colorsEnabled()) {
      print(Entry::notice(NoticeLevel::Info, "The Matrix needs colors, and NO_COLOR is set."));
      return;
    }
    m_ticker.stop();
    Tui::Rain::Options options;
    options.frames = std::numeric_limits<double>::infinity();
    playRain(options, [this]() {
      // The window may have been resized meanwhile: reprint if so, then redraw.
      reprint();
    });
  } else {
    print(Entry::notice(NoticeLevel::Error, QString("Unknown command %1. Type /help for the list.").arg(command)));
  }
}

void App::runBusy(const QString& activity, std::function<void(const CancelPtr&)> work, std::function<void()> then) {
  const CancelPtr cancel = makeCancel();
  m_pCancel = cancel;
  m_busy = true;
  m_activity = activity;
  requestRender();
  auto error = std::make_shared<QString>();
  const QString baseUrl = m_pServer->baseUrl();
  auto* thread = QThread::create([work, cancel, error, baseUrl]() {
    try {
      work(cancel);
    } catch (const std::exception& failure) {
      *error = describeError(failure, baseUrl);
    } catch (...) {
      *error = "Unexpected error.";
    }
  });
  m_workers << thread;
  connect(thread, &QThread::finished, this, [this, thread, error, then]() {
    m_workers.removeAll(thread);
    thread->deleteLater();
    if (m_closed) {
      return;
    }
    flushLive();
    if (!error->isEmpty()) {
      print(Entry::notice(NoticeLevel::Error, *error));
    }
    m_pCancel.reset();
    m_activity.clear();
    m_running.clear();
    m_runningOrder.clear();
    setCompacting(false);
    m_busy = false;
    // Messages the agent never got (interrupted, error, step limit) go back into the input.
    const QStringList undelivered = m_pAgent->takeQueued();
    if (!undelivered.isEmpty()) {
      QStringList all = undelivered;
      const QString typing = m_editor.value().trimmed();
      if (!typing.isEmpty()) {
        all << typing;
      }
      m_editor.setValue(all.join("\n\n"));
      print(Entry::notice(NoticeLevel::Warn, "Your queued message wasn't delivered, so it's back in the input box."));
    }
    if (then) {
      then();
    }
    requestRender();
  });
  thread->start();
}

void App::playRain(const Tui::Rain::Options& options, std::function<void()> then) {
  const int cols = m_terminal.columns();
  const int rows = m_terminal.rows();
  if (cols < 30 || rows < 10) {
    then();
    return;
  }
  m_pFullRain = std::make_unique<Tui::Rain>(cols, rows, options);
  m_afterRain = std::move(then);
  m_terminal.write("\x1b[?1049h\x1b[?25l\x1b[2J");
  m_rainTimer.start();
}

void App::stopRain() {
  if (!m_pFullRain) {
    return;
  }
  m_rainTimer.stop();
  m_pFullRain.reset();
  // Leave the alternate screen: the normal screen and its scrollback come back as they were.
  m_terminal.write("\x1b[0m\x1b[?1049l");
  const auto then = std::move(m_afterRain);
  m_afterRain = {};
  if (then) {
    then();
  }
}

void App::undo() {
  const std::optional<UndoResult> result = m_pAgent->undo();
  if (!result) {
    print(Entry::notice(NoticeLevel::Info, "Nothing to undo."));
    return;
  }
  const QString cwd = m_project.cwd;
  auto rel = [&](const QStringList& files) {
    QStringList out;
    for (const QString& file : files) {
      out << (file.startsWith(cwd + "/") ? file.mid(cwd.size() + 1) : file);
    }
    return out.join(", ");
  };
  QStringList lines{"Undid the last turn and removed it from the conversation."};
  if (!result->restored.isEmpty()) {
    lines << "Restored: " + rel(result->restored);
  }
  if (!result->skipped.isEmpty()) {
    lines << "Left alone because they changed afterwards: " + rel(result->skipped);
  }
  if (!result->untracked.isEmpty()) {
    lines << QString("Not undone: anything changed by %1 commands.").arg(result->untracked.join(", "));
  }
  const bool warn = !result->skipped.isEmpty() || !result->untracked.isEmpty();
  print(Entry::notice(warn ? NoticeLevel::Warn : NoticeLevel::Info, lines.join('\n')));
  m_editor.setValue(result->prompt);
}

void App::listSkills() {
  if (m_project.skills.isEmpty()) {
    print(Entry::notice(NoticeLevel::Info, "No skills found. Add folders with a SKILL.md to .agents/skills/ in the project or ~/.config/smith/skills/."));
    return;
  }
  static const QRegularExpression whitespace(R"(\s+)");
  QStringList lines;
  for (const Skill& skill : m_project.skills) {
    lines << skill.name + ": " + QString(skill.description).replace(whitespace, " ");
  }
  print(Entry::notice(NoticeLevel::Info, lines.join('\n')));
}

void App::manageJobs(const QString& arg) {
  const auto jobs = m_options.jobs;
  if (!jobs) {
    print(Entry::notice(NoticeLevel::Info, "Background jobs aren't available here."));
    return;
  }
  const QStringList parts = arg.split(' ', Qt::SkipEmptyParts);
  if (parts.value(0) == "stop" && parts.size() > 1) {
    const QString id = parts[1];
    if (id == "all") {
      const int stopped = jobs->stopAll();
      print(Entry::notice(NoticeLevel::Info, stopped ? QString("Stopped %1.").arg(plural(stopped, "background job")) : QString("No background jobs are running.")));
      return;
    }
    withActivity("Stopping " + id + "…", [this, jobs, id]() -> std::function<void()> {
      const JobStatus status = jobs->stop(id);
      return [this, id, status]() { print(Entry::notice(NoticeLevel::Info, QString("Stopped %1 (%2).").arg(id, describeJob(status)))); };
    });
    return;
  }
  if (!arg.isEmpty()) {
    print(Entry::notice(NoticeLevel::Error, "Use /jobs to list background jobs, /jobs stop <id> or /jobs stop all to stop them."));
    return;
  }
  const QList<JobStatus> list = jobs->list();
  if (list.isEmpty()) {
    print(Entry::notice(NoticeLevel::Info, "No background jobs. The agent starts them with bash's run_in_background."));
    return;
  }
  QStringList lines;
  for (const JobStatus& job : list) {
    lines << QString("%1  %2 started %3 ago  %4")
                 .arg(job.id, describeJob(job).leftJustified(20), formatDuration(job.started.msecsTo(QDateTime::currentDateTime())), job.command);
  }
  print(Entry::notice(NoticeLevel::Info, lines.join('\n') + "\n/jobs stop <id> or /jobs stop all stops them."));
}

void App::setNetwork(const QString& arg) {
  const auto sandbox = m_options.sandbox;
  if (!sandbox->active()) {
    print(Entry::notice(NoticeLevel::Warn, QString("There is no sandbox, so commands already have full network access (%1).").arg(sandbox->reason())));
    return;
  }
  if (arg != "on" && arg != "off") {
    print(Entry::notice(NoticeLevel::Info,
                        QString("Network is %1 for sandboxed commands. Use /network on or /network off.").arg(sandbox->network() ? "on" : "off")));
    return;
  }
  if (sandbox->network() == (arg == "on")) {
    print(Entry::notice(NoticeLevel::Info, QString("Network is already %1 for sandboxed commands.").arg(arg)));
    return;
  }
  sandbox->setNetwork(arg == "on");
  const QString restart = sandbox->type() == SandboxType::Container
                              ? " The container was removed for that; the next command starts a new one, without what was installed in the old one."
                              : QString();
  print(Entry::notice(NoticeLevel::Info, QString("Sandboxed commands now have network %1.%2").arg(arg, restart)));
}

} // namespace Smith::Ui
