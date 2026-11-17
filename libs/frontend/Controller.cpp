#include "frontend/Controller.h"

#include "core/Dirs.h"
#include "core/Errors.h"
#include "core/Mentions.h"
#include "core/Preferences.h"
#include "core/Reasoning.h"
#include "core/Tool.h"
#include "frontend/Commands.h"
#include "frontend/Duration.h"
#include "frontend/Greetings.h"
#include "frontend/Replay.h"
#include "frontend/Runtime.h"

#include <QLocale>
#include <QRegularExpression>

namespace Smith {

namespace {

QString windowText(std::optional<int> contextSize) {
  return contextSize ? QLocale(QLocale::English).toString(*contextSize) + "-token context" : QString("context size unknown");
}

} // namespace

ControllerOptions controllerOptions(const std::shared_ptr<Runtime>& runtime) {
  ControllerOptions options;
  options.createAgent = [runtime](const AgentHooks& hooks, const std::optional<AgentSettings>& current) { return runtime->createAgent(hooks, current); };
  options.project = runtime->project();
  options.switchProject = [runtime](const QString& folder) { return runtime->switchProject(folder); };
  options.server = runtime->server();
  options.endpoints = runtime->config().endpoints;
  options.configFile = runtime->config().file;
  options.rememberedModel = &Runtime::rememberedModel;
  options.sandbox = runtime->sandbox();
  options.sessions = runtime->sessions();
  options.savePreferences = [runtime](const PreferenceUpdate& update) { runtime->savePreferences(update); };
  options.jobs = runtime->jobs();
  options.resumed = runtime->resumedHistory();
  options.resumedLastUsed = runtime->resumedLastUsed();
  options.warnings = runtime->warnings();
  return options;
}

Controller::Controller(QObject* pParent) : QObject(pParent) {
  // The agent calls these from its own threads.
  m_hooks.onEvent = [this](const AgentEvent& event) { QMetaObject::invokeMethod(this, [this, event]() { onEvent(event); }, Qt::QueuedConnection); };
  m_hooks.approve = [this](const ApprovalRequest& request) { return m_prompts.approve(request); };
  m_hooks.ask = [this](const Question& question) { return m_prompts.ask(question); };
  m_prompts.refuseWhen([this]() { return m_turns.interrupted(); });
  connect(&m_turns, &TurnRunner::changed, this, &Controller::changed);
}

Controller::~Controller() {
  close();
}

void Controller::attach(ControllerOptions options) {
  const ThemeSpec theme = m_options.theme;
  m_options = std::move(options);
  if (m_options.theme.name.isEmpty()) {
    m_options.theme = theme;
  }
  m_pServer = m_options.server;
  m_project = m_options.project;
  m_pAgent = m_options.createAgent(m_hooks, std::nullopt);
  if (m_options.jobs) {
    connect(m_options.jobs.get(), &BackgroundJobs::changed, this, &Controller::changed, Qt::QueuedConnection);
  }
  Q_EMIT changed();
}

void Controller::close() {
  if (m_closed) {
    return;
  }
  m_closed = true;
  m_turns.interrupt();
  m_prompts.close();
  m_turns.close();
}

// ---- the transcript ----

void Controller::start() {
  QList<Entry> entries{banner(!m_options.resumed.isEmpty(), m_options.resumedLastUsed)};
  for (const QString& warning : m_options.warnings) {
    entries.append(Entry::notice(NoticeLevel::Warn, warning));
  }
  entries.append(historyToEntries(m_options.resumed, m_project.tools));
  Q_EMIT startedOver(entries);
  Q_EMIT sessionsChanged();
  Q_EMIT projectChanged();
  refreshModels();
}

Entry Controller::banner(bool resumed, const std::optional<QDateTime>& lastUsed) const {
  const SessionControl& sessions = m_options.sessions;
  GreetingContext context;
  context.resumed = resumed;
  context.lastUsed = lastUsed;
  if (!resumed && sessions.list) {
    // A folder no other session has worked in gets its own welcome.
    const QString current = sessions.currentId();
    context.newFolder = true;
    for (const SessionInfo& info : sessions.list(false)) {
      if (info.id != current) {
        context.newFolder = false;
      }
    }
  }
  const QString id = sessions.currentId ? sessions.currentId() : QString();
  const QString name = sessions.currentName ? sessions.currentName() : QString();
  Entry entry;
  entry.kind = Entry::Kind::Banner;
  entry.model = m_pAgent->model();
  entry.server = m_pServer->label();
  entry.cwd = m_project.cwd;
  entry.session = name.isEmpty() ? id : name + " · " + id;
  entry.resumed = resumed;
  entry.sandboxActive = m_options.sandbox->active();
  entry.sandboxDescription = m_options.sandbox->describe(m_project.cwd);
  entry.instructions = m_project.instructions;
  for (const Skill& skill : m_project.skills) {
    entry.skills << skill.name;
  }
  entry.tools = m_project.customTools;
  if (matrix()) {
    entry.greeting = greeting(context);
  }
  return entry;
}

void Controller::startOver(bool resumed, const std::optional<QDateTime>& lastUsed, const QList<Entry>& entries) {
  QList<Entry> all{banner(resumed, lastUsed)};
  // Background jobs belong to the conversation that started them.
  const int stopped = m_options.jobs ? m_options.jobs->clear() : 0;
  if (stopped) {
    all.append(Entry::notice(NoticeLevel::Info, QString("Stopped %1 from the previous session.").arg(plural(stopped, "background job"))));
  }
  m_turns.forgetContext();
  Q_EMIT startedOver(all + entries);
  Q_EMIT sessionsChanged();
  Q_EMIT projectChanged();
  Q_EMIT changed();
}

void Controller::print(const Entry& entry) {
  Q_EMIT entryAdded(entry);
}

void Controller::notice(NoticeLevel level, const QString& text) {
  print(Entry::notice(level, text));
}

void Controller::onEvent(const AgentEvent& event) {
  if (m_closed) {
    return;
  }
  const std::optional<Entry> entry = m_turns.apply(event);
  Q_EMIT agentEvent(event, entry);
  Q_EMIT changed();
}

// ---- messages ----

void Controller::submit(const QString& value) {
  const QString text = value.trimmed();
  if (text.isEmpty() || !attached()) {
    return;
  }
  if (m_turns.busy()) {
    if (text.startsWith('/')) {
      notice(NoticeLevel::Warn, "Commands work once the agent is done. Your command is still in the input box.");
      Q_EMIT inputRestored(value);
    } else if (m_turns.agentRunning()) {
      // Delivered at the agent's next step.
      m_pAgent->queue(text);
    } else {
      Q_EMIT inputRestored(value);
    }
    Q_EMIT changed();
    return;
  }
  if (text.startsWith('/')) {
    runCommand(text);
    return;
  }
  sendMessage(text);
}

void Controller::sendMessage(const QString& text) {
  print(Entry::simple(Entry::Kind::User, text));
  // Files it mentions with @, and image files it names, go along with it.
  const Attachments refs = attachmentsIn(text, m_project.cwd);
  if (!refs.attached.isEmpty()) {
    notice(NoticeLevel::Info, "Attached " + refs.attached.join(", ") + ".");
  }
  for (const QString& problem : refs.problems) {
    notice(NoticeLevel::Warn, problem);
  }
  const std::shared_ptr<Agent> agent = m_pAgent;
  const QList<Image> images = refs.images;
  const QList<AttachedFile> files = refs.files;
  const bool planning = agent->planning();
  runTurn(
      planning ? "Planning…" : "Thinking…", [agent, text, images, files](const CancelPtr& cancel) { agent->send(text, cancel, images, files); },
      [this, planning]() { afterUserTurn(planning); }, true);
}

void Controller::afterUserTurn(bool planning) {
  const TurnStats& turn = m_turns.lastTurn();
  print(Entry::simple(Entry::Kind::Meta, turnSummary(turn.reason, turn.elapsedMs, turn.toolCalls)));
  if (turn.reason == "interrupted" && matrix()) {
    print(Entry::simple(Entry::Kind::Quote, INTERRUPTED));
  }
  Q_EMIT sessionsChanged();
  // The next queued task, if this one went well; else they wait in the input box.
  QStringList putBack;
  if (const std::optional<QString> next = m_turns.afterTurn(putBack)) {
    QMetaObject::invokeMethod(this, [this, task = *next]() { submit(task); }, Qt::QueuedConnection);
    return;
  }
  if (!putBack.isEmpty()) {
    notice(NoticeLevel::Warn, putBackText(putBack.size()));
    Q_EMIT inputAppended(putBack.join("\n\n"));
  }
  if (planning && turn.reason == "stop" && m_pAgent->planning()) {
    Q_EMIT pickerRequested("plan", {});
  }
}

void Controller::queueNext(const QString& value) {
  const QString text = value.trimmed();
  if (text.isEmpty() || !m_turns.agentRunning()) {
    return;
  }
  if (text.startsWith('/')) {
    notice(NoticeLevel::Warn, "Commands can't be queued. Yours is still in the input box, for when the agent is done.");
    Q_EMIT inputRestored(value);
    return;
  }
  m_turns.queueNext(text);
}

QString Controller::takeNext(int index) {
  return m_turns.takeNext(index);
}

void Controller::interrupt() {
  m_prompts.dismiss();
  m_turns.interrupt();
}

void Controller::carryOutPlan() {
  if (busy()) {
    return;
  }
  if (m_pAgent->planning()) {
    setPlanning(false);
  }
  submit(GO_AHEAD);
}

void Controller::runTurn(const QString& activity, std::function<void(const CancelPtr&)> work, std::function<void()> then, bool timed) {
  m_turns.runTurn(
      activity, m_pServer->baseUrl(), std::move(work),
      [this, then](const QString& error) {
        Q_EMIT turnEnded();
        if (!error.isEmpty()) {
          notice(NoticeLevel::Error, error);
        }
        // Messages the agent never got (interrupted, error, step limit) go back into the input.
        const QStringList undelivered = m_pAgent->takeQueued();
        if (!undelivered.isEmpty()) {
          notice(NoticeLevel::Warn, "Your queued message wasn't delivered, so it's back in the input box.");
          Q_EMIT inputAppended(undelivered.join("\n\n"));
        }
        if (then) {
          then();
        }
      },
      timed);
}

void Controller::runTask(const QString& activity, std::function<std::function<void()>()> work) {
  m_turns.runTask(activity, m_pServer ? m_pServer->baseUrl() : QString(), std::move(work),
                  [this](const QString& error) { notice(NoticeLevel::Error, error); });
}

void Controller::launch(std::function<void()> work, std::function<void()> finished) {
  m_turns.launch(std::move(work), std::move(finished));
}

// ---- commands ----

void Controller::runCommand(const QString& text) {
  static const QRegularExpression whitespace(R"(\s+)");
  QStringList parts = text.split(whitespace, Qt::SkipEmptyParts);
  const QString command = parts.takeFirst();
  const QString arg = parts.join(' ');
  if (m_appCommands && m_appCommands(command, arg)) {
    return;
  }
  if (command == "/exit" || command == "/quit") {
    Q_EMIT quitRequested();
  } else if (command == "/help") {
    notice(NoticeLevel::Info, m_help ? m_help() : commandHelp());
  } else if (command == "/new" || command == "/clear") {
    newSession(arg);
  } else if (command == "/sessions" || command == "/resume") {
    Q_EMIT pickerRequested("sessions", arg == "all" ? QString("all") : QString());
  } else if (command == "/name") {
    nameSession(arg);
  } else if (command == "/open") {
    Q_EMIT pickerRequested("open", {});
  } else if (command == "/endpoint") {
    if (arg.isEmpty()) {
      Q_EMIT pickerRequested("endpoint", {});
    } else {
      setEndpoint(arg);
    }
  } else if (command == "/model") {
    if (arg.isEmpty()) {
      Q_EMIT pickerRequested("model", {});
    } else {
      setModel(arg);
    }
  } else if (command == "/reasoning") {
    if (arg.isEmpty()) {
      Q_EMIT pickerRequested("reasoning", {});
    } else {
      setReasoning(arg);
    }
  } else if (command == "/theme") {
    if (arg.isEmpty()) {
      pickTheme();
    } else {
      setTheme(arg);
    }
  } else if (command == "/plan") {
    if (!arg.isEmpty() && arg != "on" && arg != "off") {
      notice(NoticeLevel::Error, "Use /plan to switch plan mode, or /plan on or /plan off.");
    } else {
      setPlanning(arg.isEmpty() ? !m_pAgent->planning() : arg == "on");
    }
  } else if (command == "/diff") {
    showChanges();
  } else if (command == "/network") {
    if (arg == "on" || arg == "off") {
      setNetwork(arg == "on");
    } else if (m_options.sandbox->active()) {
      notice(NoticeLevel::Info, QString("Network is %1 for sandboxed commands. Use /network on or /network off.").arg(m_options.sandbox->network() ? "on" : "off"));
    } else {
      setNetwork(true);
    }
  } else if (command == "/permissions") {
    notice(NoticeLevel::Info, managePermissions(*m_pAgent->allowed(), arg));
  } else if (command == "/undo") {
    undo();
  } else if (command == "/skills") {
    Q_EMIT pickerRequested("skills", {});
  } else if (command == "/jobs") {
    const QStringList words = arg.split(' ', Qt::SkipEmptyParts);
    if (words.value(0) == "stop" && words.size() > 1) {
      stopJob(words[1]);
    } else if (arg.isEmpty()) {
      Q_EMIT pickerRequested("jobs", {});
    } else {
      notice(NoticeLevel::Error, "Use /jobs to list background jobs, /jobs stop <id> or /jobs stop all to stop them.");
    }
  } else if (command == "/compact") {
    compact();
  } else if (command == "/redpill") {
    // Not listed in /help.
    Q_EMIT redPill();
  } else {
    notice(NoticeLevel::Error, QString("Unknown command %1. Type /help for the list.").arg(command));
  }
}

void Controller::setPlanning(bool on) {
  if (busy()) {
    return;
  }
  if (on == m_pAgent->planning()) {
    notice(NoticeLevel::Info, QString("Plan mode is already %1.").arg(on ? "on" : "off"));
    return;
  }
  m_pAgent->setPlanning(on);
  notice(NoticeLevel::Info, planModeText(on));
  Q_EMIT changed();
}

void Controller::setNetwork(bool on) {
  if (!attached()) {
    return;
  }
  const auto sandbox = m_options.sandbox;
  if (!sandbox->active()) {
    notice(NoticeLevel::Warn, QString("There is no sandbox, so commands already have full network access (%1).").arg(sandbox->reason()));
    return;
  }
  if (sandbox->network() == on) {
    notice(NoticeLevel::Info, QString("Network is already %1 for sandboxed commands.").arg(on ? "on" : "off"));
    return;
  }
  sandbox->setNetwork(on);
  const QString restart = sandbox->type() == SandboxType::Container
                              ? " The container was removed for that; the next command starts a new one, without what was installed in the old one."
                              : QString();
  notice(NoticeLevel::Info, QString("Sandboxed commands now have network %1.%2").arg(on ? "on" : "off", restart));
  Q_EMIT changed();
}

void Controller::setTheme(const QString& name) {
  QString problem;
  const std::optional<ThemeSpec> theme = findTheme(name, &problem);
  if (!theme) {
    notice(NoticeLevel::Error, problem);
    return;
  }
  m_options.theme = *theme;
  if (m_options.savePreferences) {
    m_options.savePreferences(PreferenceUpdate{{}, {}, {}, theme->name});
  } else {
    // Before there's a runtime (the desktop app starting).
    PreferenceChange change;
    change.theme = theme->name;
    savePreferences(change);
  }
  Q_EMIT themeChanged();
  notice(NoticeLevel::Info, QString("Theme: %1.%2").arg(theme->name, m_themeNote ? m_themeNote(*theme) : QString()));
}

void Controller::pickTheme() {
  for (const QString& problem : listThemes().problems) {
    notice(NoticeLevel::Warn, problem);
  }
  Q_EMIT pickerRequested("theme", {});
}

void Controller::showChanges() {
  if (!attached()) {
    return;
  }
  for (const Entry& entry : changesEntries(m_pAgent->sessionChanges(), m_project.cwd)) {
    print(entry);
  }
}

void Controller::undo() {
  if (busy()) {
    return;
  }
  const std::optional<UndoResult> result = m_pAgent->undo();
  if (!result) {
    notice(NoticeLevel::Info, "Nothing to undo.");
    return;
  }
  print(undoNotice(*result, m_project.cwd));
  Q_EMIT inputRestored(result->prompt);
  Q_EMIT changed();
}

void Controller::compact() {
  if (busy()) {
    return;
  }
  m_turns.setCompacting(true);
  const std::shared_ptr<Agent> agent = m_pAgent;
  runTurn("Compacting…", [agent](const CancelPtr& cancel) { agent->compactNow(cancel); });
}

void Controller::stopJob(const QString& id) {
  const auto jobs = m_options.jobs;
  if (!jobs) {
    notice(NoticeLevel::Info, "Background jobs aren't available here.");
    return;
  }
  if (id == "all") {
    const int stopped = jobs->stopAll();
    notice(NoticeLevel::Info, stopped ? QString("Stopped %1.").arg(plural(stopped, "background job")) : QString("No background jobs are running."));
    Q_EMIT changed();
    return;
  }
  runTask("Stopping " + id + "…", [this, jobs, id]() -> std::function<void()> {
    const JobStatus status = jobs->stop(id);
    return [this, id, status]() { notice(NoticeLevel::Info, QString("Stopped %1 (%2).").arg(id, describeJob(status))); };
  });
}

// ---- what the agent talks to ----

void Controller::setModel(const QString& id) {
  if (busy() || id.isEmpty() || id == m_pAgent->model()) {
    return;
  }
  switchModel(id);
}

void Controller::switchModel(const QString& id) {
  const ServerPtr server = m_pServer;
  runTask(QString("Checking %1…").arg(id), [this, server, id, closing = m_turns.closing()]() -> std::function<void()> {
    const std::optional<int> contextSize = server->contextSize(id, closing);
    return [this, server, id, contextSize]() {
      m_pAgent->setModel(id, contextSize);
      m_options.savePreferences(PreferenceUpdate{server, id, {}});
      notice(NoticeLevel::Info, QString("Switched to %1 (%2).").arg(id, windowText(contextSize)));
    };
  });
}

void Controller::setReasoning(const QString& level) {
  if (busy()) {
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
  m_options.savePreferences(PreferenceUpdate{m_pServer, {}, level});
  notice(NoticeLevel::Info, QString("Reasoning: %1 (%2).").arg(level, reasoningHint(level)));
  Q_EMIT changed();
}

void Controller::setEndpoint(const QString& name, const QString& preferredModel) {
  if (busy()) {
    return;
  }
  const Endpoint* found = nullptr;
  QStringList known;
  for (const Endpoint& endpoint : m_options.endpoints) {
    known << endpoint.name;
    if (endpoint.name == name) {
      found = &endpoint;
    }
  }
  if (!found) {
    notice(NoticeLevel::Error, QString("Unknown endpoint \"%1\". Configured: %2.").arg(name, known.isEmpty() ? QString("none") : known.join(", ")));
    return;
  }
  if (name == m_pServer->name() && preferredModel.isEmpty()) {
    return;
  }
  const Endpoint chosen = *found;
  const auto remembered = m_options.rememberedModel;
  runTask(QString("Connecting to %1…").arg(name), [this, chosen, preferredModel, remembered, closing = m_turns.closing()]() -> std::function<void()> {
    const ServerPtr next = Server::fromEndpoint(chosen);
    const QString model = next->chooseModel({preferredModel, remembered ? remembered(next->baseUrl()) : QString()}, closing);
    const std::optional<int> contextSize = next->contextSize(model, closing);
    return [this, next, model, contextSize]() {
      m_pAgent->setModel(model, contextSize, next);
      m_pServer = next;
      m_models.clear();
      Q_EMIT modelsChanged();
      refreshModels();
      m_options.savePreferences(PreferenceUpdate{next, model, {}});
      notice(NoticeLevel::Info, QString("Connected to %1, using %2 (%3).").arg(next->label(), model, windowText(contextSize)));
    };
  });
}

void Controller::refreshModels(bool fresh) {
  if (!m_pServer) {
    return;
  }
  const ServerPtr server = m_pServer;
  auto models = std::make_shared<QList<ModelInfo>>();
  launch(
      [server, models, fresh, closing = m_turns.closing()]() {
        try {
          *models = server->listModels(fresh, closing);
        } catch (const std::exception&) {
          // Completion and the picker then offer only the current model.
        }
      },
      [this, server, models]() {
        if (server == m_pServer) {
          m_models = *models;
          Q_EMIT modelsChanged();
        }
      });
}

// ---- sessions and projects ----

std::optional<QList<Entry>> Controller::enterProject(const QString& folder) {
  ProjectView next;
  try {
    next = m_options.switchProject(folder);
  } catch (const std::exception& error) {
    notice(NoticeLevel::Error, errorMessage(error));
    return std::nullopt;
  }
  const AgentSettings current{m_pServer, m_pAgent->model(), m_pAgent->reasoning(), m_pAgent->contextLimit(), m_pAgent->planning()};
  m_project = next;
  m_pAgent = m_options.createAgent(m_hooks, current);
  QList<Entry> notes;
  for (const QString& warning : next.warnings) {
    notes.append(Entry::notice(NoticeLevel::Warn, warning));
  }
  if (next.cwd == homeDir() || next.cwd == "/") {
    notes.append(Entry::notice(NoticeLevel::Warn, QString("The project is now %1: the agent can change anything under it.").arg(next.cwd)));
  }
  return notes;
}

void Controller::newSession(const QString& folder, const QList<Entry>& extra) {
  if (busy()) {
    return;
  }
  const QString previous = m_options.sessions.currentId();
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
  m_options.sessions.startNew(m_pAgent->model(), m_pServer->name());
  const QString where = folder.isEmpty() ? QString() : " in " + tildify(m_project.cwd);
  notes.prepend(Entry::notice(NoticeLevel::Info,
                              QString("Started a new session%1. The previous one is %2; /sessions lists this folder's sessions.").arg(where, previous)));
  startOver(false, std::nullopt, notes + extra);
}

void Controller::resumeIn(const QString& folder, const QList<Entry>& extra) {
  for (const SessionInfo& session : m_options.sessions.list(true)) {
    if (session.cwd == folder) {
      if (session.id != m_options.sessions.currentId()) {
        openSession(session.id, folder, extra);
      }
      return;
    }
  }
  newSession(folder, extra);
}

void Controller::openSession(const QString& id, const QString& folder, const QList<Entry>& extra) {
  if (busy() || id == m_options.sessions.currentId()) {
    return;
  }
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
    opened = m_options.sessions.open(id);
  } catch (const std::exception& error) {
    notice(NoticeLevel::Error, QString("Could not open session %1: %2").arg(id, errorMessage(error)));
    return;
  }
  m_pAgent->loadConversation(opened.messages);
  startOver(true, opened.lastUsed, notes + extra + historyToEntries(opened.messages, m_project.tools));

  // Continue on the endpoint and model the session last used, when they're available.
  bool configured = false;
  for (const Endpoint& endpoint : m_options.endpoints) {
    configured = configured || endpoint.name == opened.endpoint;
  }
  if (!opened.endpoint.isEmpty() && opened.endpoint != m_pServer->name() && configured) {
    setEndpoint(opened.endpoint, opened.model);
  } else if (!opened.model.isEmpty() && opened.model != m_pAgent->model()) {
    const ServerPtr server = m_pServer;
    const QString model = opened.model;
    runTask(QString("Checking %1…").arg(model), [this, server, model, closing = m_turns.closing()]() -> std::function<void()> {
      bool served = false;
      try {
        for (const ModelInfo& info : server->listModels(false, closing)) {
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

void Controller::deleteSession(const QString& id, const QString& name) {
  try {
    m_options.sessions.remove(id);
  } catch (const std::exception& error) {
    notice(NoticeLevel::Error, "Could not delete the session: " + errorMessage(error));
    return;
  }
  notice(NoticeLevel::Info, QString("Deleted the session \"%1\" (%2) and its tool log.").arg(name.isEmpty() ? id : name, id));
  Q_EMIT sessionsChanged();
}

void Controller::nameSession(const QString& value) {
  const QString name = value.trimmed();
  if (name.isEmpty()) {
    const QString current = m_options.sessions.currentName();
    notice(NoticeLevel::Info, current.isEmpty() ? QString("This session has no name yet. /name <name> gives it one; until then, listings use its first message.")
                                                : QString("This session is named \"%1\". /name <new name> renames it.").arg(current));
    return;
  }
  m_options.sessions.rename(name);
  static const QRegularExpression whitespace(R"(\s)");
  const QString resume = whitespace.match(name).hasMatch() ? QString("smith --resume \"%1\"").arg(name) : "smith --resume " + name;
  notice(NoticeLevel::Info, QString("Named this session \"%1\". Resume it from anywhere with: %2").arg(name, resume));
  Q_EMIT sessionsChanged();
}

} // namespace Smith
