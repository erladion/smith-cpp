#include "App.h"
#include "TerminalTheme.h"

#include "core/Errors.h"
#include "core/Images.h"
#include "core/Jobs.h"
#include "core/Tool.h"
#include "frontend/Commands.h"
#include "Connection.h"
#include "frontend/Duration.h"
#include "frontend/Greetings.h"
#include "frontend/Replay.h"
#include "SessionCommands.h"
#include "WorktreeCommands.h"
#include "frontend/Worktrees.h"

namespace Smith::Ui {

using Tui::Key;

App::App(TuiOptions options, QObject* parent)
    : QObject(parent), m_options(std::move(options)),
      m_renderer(Tui::Screen{[this](const QString& text) { m_terminal.write(text); }, [this]() { return m_terminal.columns(); },
                             [this]() { return m_terminal.rows(); }}) {
  m_pServer = m_options.server;
  m_project = m_options.project;
  Tui::setPalette(terminalPalette(m_options.theme));

  // The agent calls these from its own threads.
  m_hooks.onEvent = [this](const AgentEvent& event) { QMetaObject::invokeMethod(this, [this, event]() { onEvent(event); }, Qt::QueuedConnection); };
  m_hooks.approve = [this](const ApprovalRequest& request) { return m_prompts.approve(request); };
  m_hooks.ask = [this](const Question& question) { return m_prompts.ask(question); };
  connect(&m_prompts, &Prompts::questionAsked, this, [this]() {
    // Whatever was typed meanwhile (to queue) stays for after; the answer starts empty.
    m_stashed = m_editor.value();
    m_editor.setValue({});
    m_questionSelected = 0;
  });
  m_prompts.refuseWhen([this]() { return m_turns.interrupted(); });
  connect(&m_prompts, &Prompts::changed, this, &App::requestRender);
  connect(&m_turns, &TurnRunner::changed, this, &App::requestRender);

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
    Tui::Rain::Options rain;
    rain.title = "AGENT SMITH";
    rain.color = rainColor();
    playRain(rain, start);
  } else {
    start();
  }
}

// ---- for the command modules ----

void App::refreshModels() {
  const ServerPtr target = m_pServer;
  const CancelPtr closing = m_turns.closing();
  m_turns.launch([this, target, closing]() {
    try {
      const QList<ModelInfo> models = target->listModels(false, closing);
      QStringList ids;
      for (const ModelInfo& model : models) {
        ids << model.id;
      }
      QMetaObject::invokeMethod(this, [this, ids]() { m_suggestions.models = ids; }, Qt::QueuedConnection);
    } catch (const std::exception&) {
      // Completion just won't offer models.
    }
  });
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
  m_turns.forgetContext();
  printBanner(start);
  for (const Entry& entry : all) {
    print(entry);
  }
  // The project may be another repository now.
  refreshTasks();
}

void App::refreshTasks() {
  const QString cwd = m_project.cwd;
  m_turns.launch([this, cwd]() {
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
  m_turns.runTask(activity, m_pServer->baseUrl(), std::move(work), [this](const QString& error) { print(Entry::notice(NoticeLevel::Error, error)); });
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
  banner.tools = m_project.customTools;
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
  m_turns.interrupt();
  m_prompts.close();
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
  m_turns.close();
  Q_EMIT finished();
}

bool App::matrix() const {
  return m_options.theme.base == Theme::Matrix;
}

Tui::Color App::rainColor() const {
  return matrix() ? Tui::Color::Green : Tui::Color::Blue;
}

bool App::streaming() const {
  return matrix() && m_options.codeStream && Tui::colorsEnabled() && !m_turns.busy();
}

int App::width() const {
  return std::max(20, m_terminal.columns());
}

bool App::prompting() const {
  return m_prompts.waiting();
}

bool App::agentRunning() const {
  return m_turns.agentRunning();
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
  // Image files the message names go along with it.
  const ImageRefs refs = imagesIn(text, m_project.cwd);
  if (!refs.attached.isEmpty()) {
    print(Entry::notice(NoticeLevel::Info, "Attached " + refs.attached.join(", ") + "."));
  }
  for (const QString& problem : refs.problems) {
    print(Entry::notice(NoticeLevel::Warn, problem));
  }
  const std::shared_ptr<Agent> current = m_pAgent;
  const QList<Image> images = refs.images;
  const bool planning = current->planning();
  runBusy(
      planning ? "Planning…" : "Thinking…", [current, text, images](const CancelPtr& cancel) { current->send(text, cancel, images); },
      [this, planning]() {
        const TurnStats& turn = m_turns.lastTurn();
        print(Entry::simple(Entry::Kind::Meta, turnSummary(turn.reason, turn.elapsedMs, turn.toolCalls)));
        if (turn.reason == "interrupted" && matrix()) {
          print(Entry::simple(Entry::Kind::Quote, INTERRUPTED));
        }
        if (planning && turn.reason == "stop" && m_pAgent->planning()) {
          offerPlan();
        }
      },
      true);
}

void App::runBusy(const QString& activity, std::function<void(const CancelPtr&)> work, std::function<void()> then, bool timed) {
  m_turns.runTurn(
      activity, m_pServer->baseUrl(), std::move(work),
      [this, then](const QString& error) {
        flushLive();
        if (!error.isEmpty()) {
          print(Entry::notice(NoticeLevel::Error, error));
        }
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
      },
      timed);
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

} // namespace Smith::Ui
