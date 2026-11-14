#include "Backend.h"

#include "core/Dirs.h"
#include "core/Errors.h"
#include "core/Jobs.h"
#include "core/Reasoning.h"
#include "core/Tool.h"
#include "tui/Rain.h"
#include "frontend/Commands.h"
#include "frontend/Duration.h"
#include "frontend/Folders.h"
#include "frontend/Greetings.h"
#include "frontend/Replay.h"
#include "frontend/Activity.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QJSEngine>
#include <QLocale>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QUrl>

namespace Smith::Desktop {

namespace {

QString windowText(std::optional<int> contextSize) {
  return contextSize ? QLocale(QLocale::English).toString(*contextSize) + "-token context" : QString("context size unknown");
}

QString helpText() {
  QStringList lines;
  int width = 0;
  for (const CommandSpec& command : commands()) {
    width = std::max(width, int(QString("%1 %2").arg(command.name, command.args).size()));
  }
  for (const CommandSpec& command : commands()) {
    lines << "  " + QString("%1 %2").arg(command.name, command.args).leftJustified(width + 2) + command.description;
  }
  return "Commands (the same as in the terminal; most also have a button or menu):\n" + lines.join('\n') +
         "\nKeys: enter sends · shift+enter adds a new line · esc interrupts the agent · ↑/↓ in an empty box recall earlier input\n"
         "      y / a / n answer an approval · ctrl+n new session · ctrl+o open a folder · ctrl+k pick a model";
}

} // namespace

Backend::Backend(const LaunchOptions& launch, QObject* parent) : QObject(parent), m_launch(launch) {
  try {
    const Config config = loadConfig();
    m_theme = config.theme;
    m_codeStream = config.codeStream;
  } catch (const std::exception&) {
    // start() reports a broken config file; until then, the default look.
  }
  m_palette = Palette::forTheme(m_theme);
  m_transcript.setPalette(m_palette);

  // The agent calls these from its own threads.
  m_hooks.onEvent = [this](const AgentEvent& event) { QMetaObject::invokeMethod(this, [this, event]() { onEvent(event); }, Qt::QueuedConnection); };
  m_hooks.approve = [this](const ApprovalRequest& request) { return approveFromWorker(request); };
  m_hooks.ask = [this](const Question& question) { return askFromWorker(question); };
}

Backend::~Backend() {
  quit();
}

Backend* Backend::create(QQmlEngine*, QJSEngine*) {
  QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
  return s_instance;
}

void Backend::setInstance(Backend* backend) {
  s_instance = backend;
}

QStringList Backend::spinnerFrames() const {
  return Smith::spinnerFrames(m_theme);
}

QStringList Backend::reasoningLevels() const {
  return REASONING_LEVELS;
}

QString Backend::version() const {
  return SMITH_VERSION;
}

// ---- startup ----

QThread* Backend::launch(std::function<void()> work, std::function<void()> finished) {
  QThread* thread = QThread::create(std::move(work));
  m_workers << thread;
  connect(thread, &QThread::finished, this, [this, thread, finished]() {
    m_workers.removeAll(thread);
    thread->deleteLater();
    if (!m_closed && finished) {
      finished();
    }
  });
  thread->start();
  return thread;
}

void Backend::start() {
  if (m_state == "ready") {
    return;
  }
  m_state = "starting";
  m_startupError.clear();
  Q_EMIT changed();
  auto runtime = std::make_shared<std::shared_ptr<Runtime>>();
  auto error = std::make_shared<QString>();
  const LaunchOptions options = m_launch;
  launch(
      [runtime, error, options]() {
        try {
          *runtime = Runtime::start(options);
        } catch (const std::exception& failure) {
          *error = errorMessage(failure);
        }
      },
      [this, runtime, error]() {
        if (!error->isEmpty()) {
          m_state = "failed";
          m_startupError = *error;
          Q_EMIT changed();
          return;
        }
        m_pRuntime = *runtime;
        ready();
      });
}

void Backend::ready() {
  m_pServer = m_pRuntime->server();
  m_project = m_pRuntime->project();
  m_sessionControl = m_pRuntime->sessions();
  m_pAgent = m_pRuntime->createAgent(m_hooks);
  connect(m_pRuntime->jobs().get(), &BackgroundJobs::changed, this, &Backend::changed, Qt::QueuedConnection);
  m_state = "ready";
  const Messages resumed = m_pRuntime->resumedHistory();
  printBanner(!resumed.isEmpty(), m_pRuntime->resumedLastUsed());
  for (const QString& warning : m_pRuntime->warnings()) {
    notice(NoticeLevel::Warn, warning);
  }
  for (const Entry& entry : historyToEntries(resumed, m_project.tools)) {
    m_transcript.append(entry);
  }
  refreshModels();
  refreshSessions();
  refreshWorktrees();
  Q_EMIT changed();
}

void Backend::printBanner(bool resumed, const std::optional<QDateTime>& lastUsed) {
  GreetingContext context;
  context.resumed = resumed;
  context.lastUsed = lastUsed;
  if (!resumed) {
    // A folder no other session has worked in gets its own welcome.
    const QString current = m_sessionControl.currentId();
    context.newFolder = true;
    for (const SessionInfo& info : m_sessionControl.list(false)) {
      if (info.id != current) {
        context.newFolder = false;
      }
    }
  }
  Entry banner;
  banner.kind = Entry::Kind::Banner;
  banner.model = m_pAgent->model();
  banner.server = m_pServer->label();
  banner.cwd = m_project.cwd;
  const QString name = m_sessionControl.currentName();
  banner.session = name.isEmpty() ? m_sessionControl.currentId() : name + " · " + m_sessionControl.currentId();
  banner.resumed = resumed;
  banner.sandboxActive = m_pRuntime->sandbox()->active();
  banner.sandboxDescription = m_pRuntime->sandbox()->describe(m_project.cwd);
  banner.instructions = m_project.instructions;
  for (const Skill& skill : m_project.skills) {
    banner.skills << skill.name;
  }
  if (matrix()) {
    banner.greeting = greeting(context);
  }
  m_transcript.append(banner);
}

void Backend::notice(NoticeLevel level, const QString& text) {
  m_transcript.append(Entry::notice(level, text));
}

// ---- properties ----

QVariantList Backend::todos() const {
  QVariantList out;
  if (!m_pAgent) {
    return out;
  }
  for (const Todo& todo : m_pAgent->todos()) {
    const QString status = todo.status == TodoStatus::Completed ? "completed" : todo.status == TodoStatus::InProgress ? "in_progress" : "pending";
    out.append(QVariantMap{{"content", todo.content}, {"status", status}});
  }
  return out;
}

QStringList Backend::queued() const {
  return m_pAgent ? m_pAgent->queuedMessages() : QStringList();
}

QVariantMap Backend::approval() const {
  if (!m_approval) {
    return {};
  }
  const ApprovalRequest& request = m_approval->request;
  QVariantMap map{
      {"tool", request.tool},
      {"summary", request.summary},
      {"note", request.note},
      {"warning", request.warning},
      {"hasSandbox", request.hasSandbox},
      {"sandboxActive", request.sandboxActive},
      {"sandboxDescription", request.sandboxDescription},
  };
  if (request.preview) {
    QVariantList rows;
    for (const DiffLine& line : *request.preview) {
      const QString kind = line.kind == DiffLine::Kind::Add      ? "add"
                           : line.kind == DiffLine::Kind::Remove ? "remove"
                           : line.kind == DiffLine::Kind::Gap    ? "gap"
                                                                 : "context";
      const int number = line.kind == DiffLine::Kind::Remove ? line.oldLine : line.newLine;
      rows.append(QVariantMap{{"kind", kind}, {"text", line.text}, {"number", number}, {"count", line.count}});
    }
    map["diff"] = rows;
  }
  return map;
}

QVariantMap Backend::question() const {
  if (!m_question) {
    return {};
  }
  return QVariantMap{{"question", m_question->question.question}, {"options", m_question->question.options}};
}

QString Backend::endpoint() const {
  return m_pServer ? m_pServer->name() : QString();
}

QString Backend::serverLabel() const {
  return m_pServer ? m_pServer->label() : QString();
}

QStringList Backend::endpoints() const {
  QStringList names;
  if (m_pRuntime) {
    for (const Endpoint& endpoint : m_pRuntime->config().endpoints) {
      names << endpoint.name;
    }
  }
  return names;
}

QString Backend::model() const {
  return m_pAgent ? m_pAgent->model() : QString();
}

QString Backend::reasoning() const {
  return m_pAgent ? m_pAgent->reasoning() : QString();
}

bool Backend::sandboxActive() const {
  return m_pRuntime && m_pRuntime->sandbox()->active();
}

QString Backend::sandboxKind() const {
  return m_pRuntime ? sandboxTypeName(m_pRuntime->sandbox()->type()) : QString();
}

QString Backend::sandboxDescription() const {
  return m_pRuntime ? m_pRuntime->sandbox()->describe(m_project.cwd) : QString();
}

bool Backend::network() const {
  return m_pRuntime && m_pRuntime->sandbox()->network();
}

int Backend::jobsRunning() const {
  return m_pRuntime ? m_pRuntime->jobs()->running() : 0;
}

QVariantList Backend::jobs() const {
  QVariantList out;
  if (!m_pRuntime) {
    return out;
  }
  for (const JobStatus& job : m_pRuntime->jobs()->list()) {
    out.append(QVariantMap{
        {"id", job.id},
        {"command", job.command},
        {"running", job.running},
        {"status", describeJob(job)},
        {"started", formatDuration(job.started.msecsTo(QDateTime::currentDateTime()))},
    });
  }
  return out;
}

QString Backend::projectName() const {
  const QString name = QFileInfo(m_project.cwd).fileName();
  return name.isEmpty() ? m_project.cwd : name;
}

QVariantList Backend::skills() const {
  QVariantList out;
  for (const Skill& skill : m_project.skills) {
    out.append(QVariantMap{{"name", skill.name}, {"description", QString(skill.description).simplified()}});
  }
  return out;
}

QString Backend::sessionId() const {
  return m_sessionControl.currentId ? m_sessionControl.currentId() : QString();
}

QString Backend::sessionName() const {
  return m_sessionControl.currentName ? m_sessionControl.currentName() : QString();
}

void Backend::setAllSessions(bool all) {
  if (all != m_allSessions) {
    m_allSessions = all;
    refreshSessions();
  }
}

QVariantList Backend::recentProjects() const {
  QVariantList out;
  if (!m_sessionControl.recentProjects) {
    return out;
  }
  for (const RecentProject& project : m_sessionControl.recentProjects()) {
    if (project.cwd == m_project.cwd || !pathExists(project.cwd)) {
      continue;
    }
    out.append(QVariantMap{
        {"path", project.cwd},
        {"name", QFileInfo(project.cwd).fileName().isEmpty() ? project.cwd : QFileInfo(project.cwd).fileName()},
        {"hint", QString("%1 · %2").arg(plural(project.sessions, "session"), ago(project.updated))},
        {"where", tildify(QFileInfo(project.cwd).path())},
    });
  }
  return out;
}

bool Backend::canUndo() const {
  return m_pAgent && !m_busy && m_pAgent->canUndo();
}

QVariantList Backend::suggest(const QString& input) const {
  QStringList models;
  for (const QVariant& model : m_models) {
    models << model.toMap().value("id").toString();
  }
  QVariantList out;
  QStringList tasks;
  for (const Worktree& worktree : m_worktreeList) {
    if (!worktree.task.isEmpty()) {
      tasks << worktree.task;
    }
  }
  for (const Suggestion& suggestion : Smith::suggest(input, CompletionContext{m_project.cwd, endpoints(), models, tasks})) {
    out.append(QVariantMap{
        {"text", suggestion.text}, {"label", suggestion.label}, {"hint", suggestion.hint}, {"takesArgument", suggestion.takesArgument}});
  }
  return out;
}

QString Backend::codeStreamText(int offset, int width) const {
  return Tui::codeStream(offset, width);
}

// ---- agent events ----

void Backend::onEvent(const AgentEvent& event) {
  if (m_closed) {
    return;
  }
  auto label = [this]() {
    QStringList labels;
    for (const QString& id : m_runningOrder) {
      labels << m_running.value(id);
    }
    return runningLabel(labels);
  };
  switch (event.type) {
  case AgentEvent::Type::Delta:
    setCompacting(false);
    m_activity = event.kind == BlockKind::Text ? "Writing…" : "Thinking…";
    m_transcript.appendLive(event.kind, event.text);
    break;
  case AgentEvent::Type::ResponseEnd:
    m_transcript.finishLive();
    break;
  case AgentEvent::Type::ToolPrepare:
    setCompacting(false);
    m_transcript.finishLive();
    m_activity = "Preparing " + event.name + "…";
    break;
  case AgentEvent::Type::UserMessage:
    m_transcript.append(Entry::simple(Entry::Kind::User, event.text));
    break;
  case AgentEvent::Type::ToolStart:
    setCompacting(false);
    m_running.insert(event.id, event.name + " " + event.summary);
    m_runningOrder.removeAll(event.id);
    m_runningOrder << event.id;
    m_activity = label();
    break;
  case AgentEvent::Type::ToolEnd: {
    setCompacting(false);
    m_turnToolCalls++;
    m_running.remove(event.id);
    m_runningOrder.removeAll(event.id);
    m_activity = label();
    // The checklist itself was shown when it changed.
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
    m_transcript.append(entry);
    break;
  }
  case AgentEvent::Type::Context:
    m_contextTokens = event.tokens;
    m_contextLimit = event.limit.value_or(0);
    break;
  case AgentEvent::Type::Activity:
    setCompacting(event.compacting);
    // Progress from inside a tool (a sub-agent); say so when several run at once.
    m_activity = m_running.size() > 1 ? QString("%1 in parallel · %2").arg(m_running.size()).arg(event.text) : event.text;
    break;
  case AgentEvent::Type::Notice:
    notice(event.level, event.text);
    break;
  case AgentEvent::Type::Todos:
    if (m_busy) {
      Entry entry;
      entry.kind = Entry::Kind::Todos;
      entry.todos = event.todos;
      m_transcript.append(entry);
    }
    break;
  case AgentEvent::Type::Done:
    m_turnReason = doneReasonName(event.reason);
    break;
  }
  Q_EMIT changed();
}

void Backend::setCompacting(bool on) {
  m_compacting = on;
}

ApprovalDecision Backend::approveFromWorker(const ApprovalRequest& request) {
  auto pending = std::make_shared<Pending<ApprovalDecision>>();
  QMetaObject::invokeMethod(
      this,
      [this, request, pending]() {
        if (m_closed) {
          pending->resolve(ApprovalDecision::Deny);
          return;
        }
        m_approval = PendingApproval{request, pending};
        Q_EMIT promptChanged();
      },
      Qt::QueuedConnection);
  // Once the app has closed, nobody will answer.
  return pending->wait([this]() { return m_closing.load(); }, ApprovalDecision::Deny);
}

Answer Backend::askFromWorker(const Question& question) {
  auto pending = std::make_shared<Pending<Answer>>();
  QMetaObject::invokeMethod(
      this,
      [this, question, pending]() {
        if (m_closed) {
          pending->resolve(Answer{});
          return;
        }
        m_question = PendingQuestion{question, pending};
        Q_EMIT promptChanged();
      },
      Qt::QueuedConnection);
  return pending->wait([this]() { return m_closing.load(); }, Answer{});
}

void Backend::decide(const QString& decision) {
  if (!m_approval) {
    return;
  }
  const auto pending = m_approval->answer;
  m_approval.reset();
  Q_EMIT promptChanged();
  if (decision == "allow") {
    pending->resolve(ApprovalDecision::Allow);
  } else if (decision == "always") {
    pending->resolve(ApprovalDecision::AllowAlways);
  } else {
    pending->resolve(ApprovalDecision::Deny);
    if (decision == "interrupt" && m_pCancel) {
      m_pCancel->cancel();
    }
  }
}

void Backend::answerOption(int index) {
  if (!m_question) {
    return;
  }
  const auto pending = m_question->answer;
  m_question.reset();
  Q_EMIT promptChanged();
  pending->resolve(Answer{Answer::Kind::Option, index, {}});
}

void Backend::answerText(const QString& text) {
  if (!m_question || text.trimmed().isEmpty()) {
    return;
  }
  const auto pending = m_question->answer;
  m_question.reset();
  Q_EMIT promptChanged();
  pending->resolve(Answer{Answer::Kind::Text, 0, text.trimmed()});
}

void Backend::dismissQuestion() {
  if (!m_question) {
    return;
  }
  const auto pending = m_question->answer;
  m_question.reset();
  Q_EMIT promptChanged();
  pending->resolve(Answer{});
}

void Backend::interrupt() {
  if (m_approval) {
    decide("deny");
  }
  if (m_question) {
    dismissQuestion();
  }
  if (m_pCancel) {
    m_pCancel->cancel();
  }
}

// ---- turns ----

void Backend::submit(const QString& value) {
  const QString text = value.trimmed();
  if (text.isEmpty() || m_state != "ready") {
    return;
  }
  if (m_busy) {
    if (text.startsWith('/')) {
      notice(NoticeLevel::Warn, "Commands work once the agent is done. Your command is still in the input box.");
      Q_EMIT restoreInput(value);
    } else if (agentRunning()) {
      // Delivered at the agent's next step.
      m_pAgent->queue(text);
    } else {
      Q_EMIT restoreInput(value);
    }
    Q_EMIT changed();
    return;
  }
  if (text.startsWith('/')) {
    runCommand(text);
    return;
  }
  m_transcript.append(Entry::simple(Entry::Kind::User, text));
  m_turnClock.start();
  m_turnStartedAt = QDateTime::currentMSecsSinceEpoch();
  m_turnToolCalls = 0;
  m_turnReason.clear();
  const std::shared_ptr<Agent> agent = m_pAgent;
  runTurn("Thinking…", [agent, text](const CancelPtr& cancel) { agent->send(text, cancel); },
          [this]() {
            m_transcript.append(Entry::simple(Entry::Kind::Meta, turnSummary(m_turnReason, m_turnClock.elapsed(), m_turnToolCalls)));
            if (m_turnReason == "interrupted" && matrix()) {
              m_transcript.append(Entry::simple(Entry::Kind::Quote, INTERRUPTED));
            }
            m_turnStartedAt = 0;
            refreshSessions();
          });
}

void Backend::runTurn(const QString& activity, std::function<void(const CancelPtr&)> work, std::function<void()> then) {
  const CancelPtr cancel = makeCancel();
  m_pCancel = cancel;
  m_busy = true;
  m_activity = activity;
  Q_EMIT changed();
  auto error = std::make_shared<QString>();
  const QString baseUrl = m_pServer->baseUrl();
  launch(
      [work, cancel, error, baseUrl]() {
        try {
          work(cancel);
        } catch (const std::exception& failure) {
          *error = describeError(failure, baseUrl);
        } catch (...) {
          *error = "Unexpected error.";
        }
      },
      [this, error, then]() {
        m_transcript.finishLive();
        if (!error->isEmpty()) {
          notice(NoticeLevel::Error, *error);
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
          notice(NoticeLevel::Warn, "Your queued message wasn't delivered, so it's back in the input box.");
          Q_EMIT restoreInput(undelivered.join("\n\n"));
        }
        if (then) {
          then();
        }
        Q_EMIT changed();
      });
}

void Backend::background(const QString& activity, std::function<std::function<void()>()> work) {
  m_busy = true;
  m_activity = activity;
  Q_EMIT changed();
  auto next = std::make_shared<std::function<void()>>();
  auto error = std::make_shared<QString>();
  const QString baseUrl = m_pServer ? m_pServer->baseUrl() : QString();
  launch(
      [work, next, error, baseUrl]() {
        try {
          *next = work();
        } catch (const std::exception& failure) {
          *error = describeError(failure, baseUrl);
        }
      },
      [this, next, error]() {
        m_activity.clear();
        m_busy = false;
        if (!error->isEmpty()) {
          notice(NoticeLevel::Error, *error);
        } else if (*next) {
          (*next)();
        }
        Q_EMIT changed();
      });
}

// ---- commands ----

void Backend::runCommand(const QString& text) {
  static const QRegularExpression whitespace(R"(\s+)");
  QStringList parts = text.split(whitespace, Qt::SkipEmptyParts);
  const QString command = parts.takeFirst();
  const QString arg = parts.join(' ');
  if (command == "/exit" || command == "/quit") {
    quit();
  } else if (command == "/help") {
    notice(NoticeLevel::Info, helpText());
  } else if (command == "/new" || command == "/clear") {
    newSession(arg);
  } else if (command == "/sessions" || command == "/resume") {
    setAllSessions(arg == "all");
    Q_EMIT openPicker("sessions");
  } else if (command == "/name") {
    if (arg.isEmpty()) {
      const QString current = sessionName();
      notice(NoticeLevel::Info, current.isEmpty() ? QString("This session has no name yet. /name <name> gives it one; until then, listings use its first message.")
                                                  : QString("This session is named \"%1\". /name <new name> renames it.").arg(current));
    } else {
      renameSession(arg);
    }
  } else if (command == "/open") {
    Q_EMIT openPicker("folder");
  } else if (command == "/worktree") {
    createWorktree(arg);
  } else if (command == "/worktrees") {
    Q_EMIT openPicker("worktrees");
  } else if (command == "/land") {
    if (const std::optional<Worktree> worktree = findTask(arg)) {
      reviewLanding(worktree->path);
    }
  } else if (command == "/drop") {
    if (const std::optional<Worktree> worktree = findTask(arg)) {
      Q_EMIT confirmDrop(worktree->path);
    }
  } else if (command == "/endpoint") {
    if (arg.isEmpty()) {
      Q_EMIT openPicker("endpoint");
    } else {
      setEndpoint(arg);
    }
  } else if (command == "/model") {
    if (arg.isEmpty()) {
      Q_EMIT openPicker("model");
    } else {
      setModel(arg);
    }
  } else if (command == "/reasoning") {
    if (arg.isEmpty()) {
      Q_EMIT openPicker("reasoning");
    } else {
      setReasoning(arg);
    }
  } else if (command == "/network") {
    if (arg == "on" || arg == "off") {
      setNetwork(arg == "on");
    } else {
      notice(NoticeLevel::Info, QString("Network is %1 for sandboxed commands. Use /network on or /network off.").arg(network() ? "on" : "off"));
    }
  } else if (command == "/undo") {
    undo();
  } else if (command == "/skills") {
    Q_EMIT openPicker("skills");
  } else if (command == "/jobs") {
    const QStringList words = arg.split(' ', Qt::SkipEmptyParts);
    if (words.value(0) == "stop" && words.size() > 1) {
      stopJob(words[1]);
    } else {
      Q_EMIT openPicker("jobs");
    }
  } else if (command == "/compact") {
    compact();
  } else if (command == "/redpill") {
    Q_EMIT redPill();
  } else {
    notice(NoticeLevel::Error, QString("Unknown command %1. Type /help for the list.").arg(command));
  }
}

void Backend::setReasoning(const QString& level) {
  if (!m_pAgent || m_busy) {
    return;
  }
  if (!isReasoningLevel(level)) {
    notice(NoticeLevel::Error, QString("Unknown reasoning level \"%1\". Use one of: %2.").arg(level, REASONING_LEVELS.join(", ")));
    return;
  }
  if (level == m_pAgent->reasoning()) {
    return;
  }
  m_pAgent->setReasoning(level);
  m_pRuntime->savePreferences(PreferenceUpdate{m_pServer, {}, level});
  notice(NoticeLevel::Info, QString("Reasoning: %1 (%2).").arg(level, reasoningHint(level)));
  Q_EMIT changed();
}

void Backend::setModel(const QString& id) {
  if (!m_pAgent || m_busy || id.isEmpty() || id == m_pAgent->model()) {
    return;
  }
  switchModel(id);
}

void Backend::switchModel(const QString& id) {
  const ServerPtr server = m_pServer;
  background(QString("Checking %1…").arg(id), [this, server, id]() -> std::function<void()> {
    const std::optional<int> contextSize = server->contextSize(id);
    return [this, server, id, contextSize]() {
      m_pAgent->setModel(id, contextSize);
      m_pRuntime->savePreferences(PreferenceUpdate{server, id, {}});
      notice(NoticeLevel::Info, QString("Switched to %1 (%2).").arg(id, windowText(contextSize)));
    };
  });
}

void Backend::setEndpoint(const QString& name) {
  if (!m_pRuntime || m_busy || name == endpoint()) {
    return;
  }
  switchEndpoint(name);
}

void Backend::switchEndpoint(const QString& name, const QString& preferredModel) {
  const Endpoint* found = m_pRuntime->config().findEndpoint(name);
  if (!found) {
    const QStringList known = endpoints();
    notice(NoticeLevel::Error, QString("Unknown endpoint \"%1\". Configured: %2.").arg(name, known.isEmpty() ? QString("none") : known.join(", ")));
    return;
  }
  const Endpoint chosen = *found;
  background(QString("Connecting to %1…").arg(name), [this, chosen, preferredModel]() -> std::function<void()> {
    const ServerPtr next = Server::fromEndpoint(chosen);
    const QString model = next->chooseModel({preferredModel, Runtime::rememberedModel(next->baseUrl())});
    const std::optional<int> contextSize = next->contextSize(model);
    return [this, next, model, contextSize]() {
      m_pAgent->setModel(model, contextSize, next);
      m_pServer = next;
      m_models.clear();
      refreshModels();
      m_pRuntime->savePreferences(PreferenceUpdate{next, model, {}});
      notice(NoticeLevel::Info, QString("Connected to %1, using %2 (%3).").arg(next->label(), model, windowText(contextSize)));
    };
  });
}

void Backend::refreshModels() {
  if (!m_pServer) {
    return;
  }
  const ServerPtr server = m_pServer;
  auto models = std::make_shared<QVariantList>();
  launch(
      [server, models]() {
        try {
          for (const ModelInfo& info : server->listModels(true)) {
            models->append(QVariantMap{{"id", info.id}, {"context", info.contextSize.value_or(0)}});
          }
        } catch (const std::exception&) {
          // The picker then offers only the current model.
        }
      },
      [this, server, models]() {
        if (server == m_pServer) {
          m_models = *models;
          Q_EMIT changed();
        }
      });
}

void Backend::setNetwork(bool on) {
  if (!m_pRuntime) {
    return;
  }
  const auto sandbox = m_pRuntime->sandbox();
  if (!sandbox->active()) {
    notice(NoticeLevel::Warn, QString("There is no sandbox, so commands already have full network access (%1).").arg(sandbox->reason()));
    return;
  }
  if (sandbox->network() == on) {
    return;
  }
  sandbox->setNetwork(on);
  const QString restart = sandbox->type() == SandboxType::Container
                              ? " The container was removed for that; the next command starts a new one, without what was installed in the old one."
                              : QString();
  notice(NoticeLevel::Info, QString("Sandboxed commands now have network %1.%2").arg(on ? "on" : "off", restart));
  Q_EMIT changed();
}

void Backend::undo() {
  if (!m_pAgent || m_busy) {
    return;
  }
  const std::optional<UndoResult> result = m_pAgent->undo();
  if (!result) {
    notice(NoticeLevel::Info, "Nothing to undo.");
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
  notice(result->skipped.isEmpty() && result->untracked.isEmpty() ? NoticeLevel::Info : NoticeLevel::Warn, lines.join('\n'));
  Q_EMIT restoreInput(result->prompt);
  Q_EMIT changed();
}

void Backend::compact() {
  if (!m_pAgent || m_busy) {
    return;
  }
  setCompacting(true);
  const std::shared_ptr<Agent> agent = m_pAgent;
  runTurn("Compacting…", [agent](const CancelPtr& cancel) { agent->compactNow(cancel); });
}

void Backend::stopJob(const QString& id) {
  if (!m_pRuntime) {
    return;
  }
  const auto jobs = m_pRuntime->jobs();
  if (id == "all") {
    const int stopped = jobs->stopAll();
    notice(NoticeLevel::Info, stopped ? QString("Stopped %1.").arg(plural(stopped, "background job")) : QString("No background jobs are running."));
    Q_EMIT changed();
    return;
  }
  background("Stopping " + id + "…", [this, jobs, id]() -> std::function<void()> {
    const JobStatus status = jobs->stop(id);
    return [this, id, status]() { notice(NoticeLevel::Info, QString("Stopped %1 (%2).").arg(id, describeJob(status))); };
  });
}

// ---- sessions and projects ----

void Backend::refreshSessions() {
  m_sessions.clear();
  if (m_sessionControl.list) {
    const QString current = m_sessionControl.currentId();
    for (const SessionInfo& info : m_sessionControl.list(m_allSessions)) {
      QStringList hint{ago(info.updated), plural(info.turns, "message")};
      m_sessions.append(QVariantMap{
          {"id", info.id},
          {"name", info.name},
          {"hint", hint.join(" · ")},
          {"model", (info.endpoint.isEmpty() ? QString() : info.endpoint + "/") + info.model},
          {"folder", shortPath(info.cwd, 40)},
          {"cwd", info.cwd},
          {"current", info.id == current},
      });
    }
  }
  Q_EMIT sessionsChanged();
}

void Backend::startOver(bool resumed, const std::optional<QDateTime>& lastUsed, const QList<Entry>& entries) {
  QList<Entry> all = entries;
  // Background jobs belong to the conversation that started them.
  const int stopped = m_pRuntime->jobs()->clear();
  if (stopped) {
    all.prepend(Entry::notice(NoticeLevel::Info, QString("Stopped %1 from the previous session.").arg(plural(stopped, "background job"))));
  }
  m_transcript.clear();
  m_contextTokens = 0;
  m_contextLimit = 0;
  printBanner(resumed, lastUsed);
  for (const Entry& entry : all) {
    m_transcript.append(entry);
  }
  refreshSessions();
  // The project may be another repository, or another worktree of this one.
  refreshWorktrees();
  Q_EMIT changed();
}

std::optional<QList<Entry>> Backend::enterProject(const QString& folder) {
  ProjectView next;
  try {
    next = m_pRuntime->switchProject(folder);
  } catch (const std::exception& error) {
    notice(NoticeLevel::Error, errorMessage(error));
    return std::nullopt;
  }
  const AgentSettings current{m_pServer, m_pAgent->model(), m_pAgent->reasoning(), m_pAgent->contextLimit()};
  m_project = next;
  m_pAgent = m_pRuntime->createAgent(m_hooks, current);
  QList<Entry> notes;
  for (const QString& warning : next.warnings) {
    notes.append(Entry::notice(NoticeLevel::Warn, warning));
  }
  if (next.cwd == homeDir() || next.cwd == "/") {
    notes.append(Entry::notice(NoticeLevel::Warn, QString("The project is now %1: the agent can change anything under it.").arg(next.cwd)));
  }
  return notes;
}

void Backend::newSession(const QString& folder) {
  if (!m_pAgent || m_busy) {
    return;
  }
  startNewSession(folder, {});
}

void Backend::startNewSession(const QString& folder, const QList<Entry>& extra) {
  const QString previous = sessionId();
  QList<Entry> notes;
  if (!folder.isEmpty()) {
    const std::optional<QList<Entry>> entered = enterProject(folder);
    if (!entered) {
      return;
    }
    notes = *entered;
  } else {
    m_pAgent->loadConversation({});
  }
  m_sessionControl.startNew(m_pAgent->model(), m_pServer->name());
  const QString where = folder.isEmpty() ? QString() : " in " + tildify(m_project.cwd);
  notes.prepend(Entry::notice(NoticeLevel::Info, QString("Started a new session%1. The previous one is %2.").arg(where, previous)));
  startOver(false, std::nullopt, notes + extra);
}

void Backend::resumeIn(const QString& folder, const QList<Entry>& extra) {
  for (const SessionInfo& session : m_sessionControl.list(true)) {
    if (session.cwd == folder) {
      if (session.id != sessionId()) {
        openSessionIn(session.id, folder, extra);
      }
      return;
    }
  }
  startNewSession(folder, extra);
}

void Backend::openProject(const QString& folder) {
  const QUrl url(folder);
  newSession(url.isLocalFile() ? url.toLocalFile() : folder);
}

void Backend::openSession(const QString& id) {
  if (!m_pAgent || m_busy || id == sessionId()) {
    return;
  }
  QString folder;
  for (const QVariant& session : m_sessions) {
    if (session.toMap().value("id").toString() == id) {
      folder = session.toMap().value("cwd").toString();
    }
  }
  openSessionIn(id, folder, {});
}

void Backend::openSessionIn(const QString& id, const QString& folder, const QList<Entry>& extra) {
  QList<Entry> notes;
  if (!folder.isEmpty() && folder != m_project.cwd) {
    if (!pathExists(folder)) {
      notice(NoticeLevel::Error, QString("That session's folder, %1, doesn't exist anymore.").arg(folder));
      return;
    }
    const std::optional<QList<Entry>> entered = enterProject(folder);
    if (!entered) {
      return;
    }
    notes.append(Entry::notice(NoticeLevel::Info, QString("Now working in %1.").arg(tildify(folder))));
    notes.append(*entered);
  }
  OpenedSession opened;
  try {
    opened = m_sessionControl.open(id);
  } catch (const std::exception& error) {
    notice(NoticeLevel::Error, QString("Could not open session %1: %2").arg(id, errorMessage(error)));
    return;
  }
  m_pAgent->loadConversation(opened.messages);
  startOver(true, opened.lastUsed, notes + extra + historyToEntries(opened.messages, m_project.tools));

  // Continue on the endpoint and model the session last used, when they're available.
  if (!opened.endpoint.isEmpty() && opened.endpoint != endpoint() && m_pRuntime->config().findEndpoint(opened.endpoint)) {
    switchEndpoint(opened.endpoint, opened.model);
  } else if (!opened.model.isEmpty() && opened.model != m_pAgent->model()) {
    const ServerPtr server = m_pServer;
    const QString model = opened.model;
    background(QString("Checking %1…").arg(model), [this, server, model]() -> std::function<void()> {
      bool served = false;
      try {
        for (const ModelInfo& info : server->listModels()) {
          served = served || info.id == model;
        }
      } catch (const std::exception&) {
        served = false;
      }
      return [this, served, model, server]() {
        if (served) {
          switchModel(model);
        } else {
          notice(NoticeLevel::Warn,
                 QString("This session used %1, which %2 doesn't serve; continuing with %3.").arg(model, server->label(), m_pAgent->model()));
        }
      };
    });
  }
}

void Backend::deleteSession(const QString& id) {
  if (!m_sessionControl.remove) {
    return;
  }
  QString name = id;
  for (const QVariant& session : m_sessions) {
    if (session.toMap().value("id").toString() == id) {
      name = session.toMap().value("name").toString();
    }
  }
  try {
    m_sessionControl.remove(id);
  } catch (const std::exception& error) {
    notice(NoticeLevel::Error, "Could not delete the session: " + errorMessage(error));
    return;
  }
  notice(NoticeLevel::Info, QString("Deleted the session \"%1\" (%2) and its tool log.").arg(name, id));
  refreshSessions();
}

void Backend::renameSession(const QString& name) {
  if (!m_sessionControl.rename || name.trimmed().isEmpty()) {
    return;
  }
  m_sessionControl.rename(name.trimmed());
  static const QRegularExpression whitespace(R"(\s)");
  const QString resume = whitespace.match(name.trimmed()).hasMatch() ? QString("smith --resume \"%1\"").arg(name.trimmed()) : "smith --resume " + name.trimmed();
  notice(NoticeLevel::Info, QString("Named this session \"%1\". Resume it from anywhere with: %2").arg(name.trimmed(), resume));
  refreshSessions();
}

// ---- shutdown ----

void Backend::quit() {
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
  // The agent's threads stop at their next check of the cancel token.
  for (QThread* thread : m_workers) {
    thread->wait();
  }
  if (m_pRuntime) {
    m_pRuntime->shutdown();
  }
  QCoreApplication::quit();
}


// ---- task worktrees ----

void Backend::refreshWorktrees() {
  if (!m_pRuntime) {
    return;
  }
  const int generation = ++m_worktreesGeneration;
  const QString cwd = m_project.cwd;
  auto listed = std::make_shared<std::optional<QList<Worktree>>>();
  auto summaries = std::make_shared<QMap<QString, QString>>();
  launch(
      [cwd, listed, summaries]() {
        try {
          *listed = listWorktrees(cwd);
        } catch (const std::exception&) {
          // Not a repository.
          return;
        }
        for (const Worktree& worktree : **listed) {
          if (worktree.main) {
            continue;
          }
          try {
            summaries->insert(worktree.path, changeSummary(worktreeChanges(worktree)));
          } catch (const std::exception& error) {
            summaries->insert(worktree.path, errorMessage(error));
          }
        }
      },
      [this, generation, listed, summaries]() {
        if (generation != m_worktreesGeneration) {
          return;
        }
        m_inRepository = listed->has_value();
        m_worktreeList = listed->value_or(QList<Worktree>{});
        m_worktrees.clear();
        const qint64 self = QCoreApplication::applicationPid();
        for (const Worktree& worktree : m_worktreeList) {
          const QString summary = summaries->value(worktree.path);
          const bool elsewhere = worktree.openBy != 0 && worktree.openBy != self;
          QStringList hint{worktree.branch.isEmpty() ? "detached at " + worktree.head : worktree.branch};
          if (!worktree.main) {
            hint << summary;
          }
          if (elsewhere) {
            hint << "open in another Smith";
          }
          const QString label = worktree.main ? QString("your checkout") : worktree.task.isEmpty() ? QFileInfo(worktree.path).fileName() : worktree.task;
          m_worktrees.append(QVariantMap{
              {"path", worktree.path},
              {"label", label},
              {"hint", hint.join(" · ")},
              {"branch", worktree.branch},
              {"task", worktree.task},
              {"main", worktree.main},
              {"current", isCurrent(worktree)},
              {"openElsewhere", elsewhere},
              {"summary", summary},
          });
        }
        Q_EMIT worktreesChanged();
      });
}

bool Backend::isCurrent(const Worktree& worktree) const {
  const QString cwd = realPath(m_project.cwd);
  return cwd == worktree.path || cwd.startsWith(worktree.path + "/");
}

std::optional<Worktree> Backend::worktreeAt(const QString& path) const {
  for (const Worktree& worktree : m_worktreeList) {
    if (worktree.path == path) {
      return worktree;
    }
  }
  if (m_landingState && m_landingState->worktree.path == path) {
    return m_landingState->worktree;
  }
  return std::nullopt;
}

std::optional<Worktree> Backend::findTask(const QString& task) {
  if (!m_inRepository) {
    notice(NoticeLevel::Error, QString("%1 isn't in a git repository.").arg(tildify(m_project.cwd)));
    return std::nullopt;
  }
  for (const Worktree& worktree : m_worktreeList) {
    if (!worktree.main && (task.isEmpty() ? isCurrent(worktree) : worktree.task == task)) {
      return worktree;
    }
  }
  notice(NoticeLevel::Error, task.isEmpty() ? QString("You're not in a task worktree: name the task (e.g. /land fix-login), or pick one in the sidebar.")
                                            : QString("There's no task \"%1\" here; the sidebar lists them.").arg(task));
  return std::nullopt;
}

void Backend::setLanding(const QVariantMap& landing) {
  m_landing = landing;
  Q_EMIT landingChanged();
}

void Backend::createWorktree(const QString& task) {
  if (!m_pAgent || m_busy) {
    return;
  }
  const QString name = task.trimmed();
  if (name.isEmpty()) {
    notice(NoticeLevel::Error, "Name the task, e.g. /worktree fix-login.");
    return;
  }
  if (!sandboxActive()) {
    notice(NoticeLevel::Error, "Task worktrees need the sandbox: it's what keeps git read-only for the agent. Start Smith without "
                               "--sandbox off (or \"sandbox\": \"off\" in the config) to use them.");
    return;
  }
  const QString cwd = m_project.cwd;
  background("Making the worktree…", [this, cwd, name]() -> std::function<void()> {
    const CreatedWorktree created = createTaskWorktree(cwd, name);
    return [this, created]() {
      const Worktree& worktree = created.worktree;
      if (isCurrent(worktree)) {
        notice(NoticeLevel::Info, QString("You're in %1's worktree already.").arg(worktree.branch));
        return;
      }
      if (created.reused) {
        resumeIn(worktree.path, {Entry::notice(NoticeLevel::Info, QString("Back in the task's worktree, on %1.").arg(worktree.branch))});
        return;
      }
      startNewSession(worktree.path, {Entry::notice(NoticeLevel::Info, QString("Made a worktree for the task: %1, on %2 (from %3). The agent's "
                                                                               "changes stay here; git is read-only for it, so it can't commit. "
                                                                               "Land them on your branch when it's done.")
                                                                           .arg(tildify(worktree.path), worktree.branch, created.from))});
    };
  });
}

void Backend::switchToWorktree(const QString& path) {
  if (!m_pAgent || m_busy) {
    return;
  }
  const std::optional<Worktree> worktree = worktreeAt(path);
  if (worktree && !isCurrent(*worktree)) {
    resumeIn(worktree->path);
  }
}

void Backend::reviewLanding(const QString& path) {
  if (!m_pAgent || m_busy) {
    return;
  }
  const std::optional<Worktree> found = worktreeAt(path);
  if (!found || found->main) {
    return;
  }
  if (found->openBy != 0 && found->openBy != QCoreApplication::applicationPid()) {
    notice(NoticeLevel::Error, QString("%1 is open in another Smith (pid %2), where the agent may still be working. Land it from there, or quit "
                                       "that one first.")
                                   .arg(found->branch)
                                   .arg(found->openBy));
    return;
  }
  const Worktree worktree = *found;
  // What was asked for helps the model name the commit; only known in the worktree's own session.
  const QString request = isCurrent(worktree) ? firstRequest(m_pAgent->messages()) : QString();
  const std::shared_ptr<Agent> agent = m_pAgent;
  setLanding(QVariantMap{{"stage", "loading"}, {"branch", worktree.branch}});
  background("Reading the changes…", [this, worktree, request, agent]() -> std::function<void()> {
    Landing landing;
    QString message;
    try {
      landing.worktree = worktree;
      landing.changes = worktreeChanges(worktree);
      landing.mainPath = mainCheckout(worktree.path);
      landing.target = currentBranch(landing.mainPath);
      if (!landing.changes.files.isEmpty()) {
        message = suggestCommitMessage(agent, worktree, landing.changes, request, makeCancel());
      }
    } catch (const std::exception& error) {
      const QString text = errorMessage(error);
      return [this, text]() {
        setLanding({});
        notice(NoticeLevel::Error, text);
      };
    }
    return [this, landing, message]() {
      if (landing.changes.isEmpty()) {
        setLanding({});
        notice(NoticeLevel::Info, QString("Nothing to land: %1 has no changes, and %2 has all its commits.")
                                      .arg(landing.worktree.branch, landing.target.isEmpty() ? QString("your checkout") : landing.target));
        return;
      }
      m_landingState = landing;
      QVariantList files;
      for (const Entry& entry : changeEntries(landing.changes)) {
        files.append(QVariantMap{{"name", entry.name}, {"summary", entry.summary}, {"diff", entry.diff ? diffRows(*entry.diff) : QVariantList()}});
      }
      setLanding(QVariantMap{
          {"stage", "review"},
          {"path", landing.worktree.path},
          {"branch", landing.worktree.branch},
          {"task", landing.worktree.task},
          {"target", landing.target},
          {"mainPath", tildify(landing.mainPath)},
          {"summary", changeSummary(landing.changes)},
          {"files", files},
          {"pending", landing.changes.pendingCommits},
          {"message", message},
          {"uncommitted", !landing.changes.files.isEmpty()},
      });
    };
  });
}

void Backend::land(const QString& mode, const QString& message) {
  if (!m_landingState || m_busy) {
    return;
  }
  const Landing landing = *m_landingState;
  const LandMode landMode = mode == "merge" ? LandMode::Merge : mode == "branch" ? LandMode::BranchOnly : LandMode::CherryPick;
  QVariantMap next = m_landing;
  if (!landing.changes.files.isEmpty() && message.trimmed().isEmpty()) {
    next["error"] = "The commit needs a message.";
    setLanding(next);
    return;
  }
  next.remove("error");
  next["stage"] = "landing";
  setLanding(next);
  const QString text = message.trimmed();
  background("Landing…", [this, landing, landMode, text]() -> std::function<void()> {
    LandResult result;
    try {
      result = landWorktree(landing.worktree, text, landMode);
    } catch (const std::exception& error) {
      const QString failure = errorMessage(error);
      return [this, failure]() {
        m_landingState.reset();
        setLanding({});
        notice(NoticeLevel::Error, failure);
      };
    }
    return [this, landing, landMode, result]() {
      const QString report = landingReport(landing.worktree, landMode, result, landing.mainPath);
      const bool failed = !result.problem.isEmpty();
      notice(failed ? NoticeLevel::Warn : NoticeLevel::Info, report);
      refreshWorktrees();
      if (failed || landMode == LandMode::BranchOnly) {
        m_landingState.reset();
        setLanding({});
        return;
      }
      setLanding(QVariantMap{
          {"stage", "landed"},
          {"path", landing.worktree.path},
          {"branch", landing.worktree.branch},
          {"task", landing.worktree.task},
          {"target", result.target},
          {"report", report},
      });
    };
  });
}

void Backend::closeLanding() {
  if (m_landing.value("stage").toString() == "landing") {
    return;
  }
  m_landingState.reset();
  setLanding({});
}

void Backend::removeWorktree(const QString& path, bool discard) {
  if (!m_pAgent || m_busy) {
    return;
  }
  const std::optional<Worktree> found = worktreeAt(path);
  if (!found || found->main) {
    return;
  }
  if (found->openBy != 0 && found->openBy != QCoreApplication::applicationPid()) {
    notice(NoticeLevel::Error, QString("%1 is open in another Smith (pid %2). Remove it from there, or quit that one first.")
                                   .arg(found->branch)
                                   .arg(found->openBy));
    return;
  }
  const Worktree worktree = *found;
  m_landingState.reset();
  setLanding({});
  QString mainPath;
  try {
    mainPath = mainCheckout(worktree.path);
  } catch (const std::exception& error) {
    notice(NoticeLevel::Error, errorMessage(error));
    return;
  }
  if (isCurrent(worktree)) {
    // Out of it first: its session, agent and background jobs go with the switch.
    resumeIn(mainPath, {Entry::notice(NoticeLevel::Info, QString("Back in your checkout, %1.").arg(tildify(mainPath)))});
  }
  background("Removing the worktree…", [this, worktree, discard]() -> std::function<void()> {
    Smith::removeWorktree(worktree, true, discard);
    return [this, worktree]() {
      notice(NoticeLevel::Info, QString("Removed %1 and %2.").arg(tildify(worktree.path), worktree.branch));
      refreshWorktrees();
    };
  });
}

} // namespace Smith::Desktop
