#include "core/Agent.h"
#include "core/Config.h"
#include "core/Dirs.h"
#include "core/Errors.h"
#include "core/Http.h"
#include "core/Instructions.h"
#include "core/Jobs.h"
#include "core/Preferences.h"
#include "core/Prompt.h"
#include "core/Reasoning.h"
#include "core/Sandbox.h"
#include "core/Server.h"
#include "core/Session.h"
#include "core/Skills.h"
#include "core/ToolLog.h"
#include "core/Toolchain.h"
#include "core/tools/Tools.h"
#include "tui/Terminal.h"
#include "ui/App.h"
#include "ui/Duration.h"
#include "ui/Folders.h"
#include "ui/Greetings.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocale>
#include <QMap>
#include <QMutex>
#include <QRegularExpression>

#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>

using namespace Smith;

namespace {

const QString DEFAULT_BASE_URL = "http://localhost:8080/v1";

QString helpText() {
  return QString(R"(Usage: smith [options] [folder]

Works in [folder], or the current directory. That folder is the project: the only place the
agent can change files.

Server:
  -e, --endpoint <name>  Use an endpoint from the config file (default: last used, else the config's default)
  --endpoints            List configured endpoints
  --base-url <url>       Use this OpenAI-compatible URL instead of a configured endpoint (env SMITH_BASE_URL)
  --api-key <key>        API key (for --base-url also env SMITH_API_KEY or OPENAI_API_KEY)
  --model <name>         Model (env SMITH_MODEL; default: last used on this server, else the
                         endpoint's configured model, else the first listed)
  --reasoning <level>    %1 (default: last used, else "default")
  --context-size <n>     Context window in tokens, if the server doesn't report it
  --temperature <n>      Sampling temperature (default: the server's)
  --max-steps <n>        Model calls allowed per message (default: maxSteps in the config file, else 50)

Sandbox (where shell commands and the file tools run):
  --sandbox <type>       bwrap | container | off (default: sandbox.type in the config file, else
                         bwrap; Smith won't start if it can't run. "on" means bwrap. off runs
                         commands unsandboxed and must be asked for on every run)
  --network              Allow sandboxed commands to use the network (default: blocked)

Sessions:
  -c, --continue         Resume the most recent session in this directory
  -r, --resume <id|name> Resume a session by id or name (in the folder it was in)
  --sessions             List saved sessions from every folder (smith <folder> --sessions: only that one)
  --delete <id|name>     Delete a session and its tool log (asks first; --yes doesn't)

One-shot:
  -p, --print <prompt>   Run one prompt without the TUI and print the answer
  -y, --yes              With --print, allow every tool call without asking; with --delete, don't ask

  -v, --version          Show the version
  -h, --help             Show this help

Endpoints are configured in ~/.config/smith/config.json; run --endpoints for an example.)")
      .arg(REASONING_LEVELS.join(" | "));
}

void out(const QString& text) {
  std::fputs((text + "\n").toUtf8().constData(), stdout);
  std::fflush(stdout);
}

void err(const QString& text) {
  std::fputs(text.toUtf8().constData(), stderr);
  std::fflush(stderr);
}

// Everything that depends on the folder being worked in.
struct Project {
  QString cwd;
  QList<InstructionFile> instructions;
  QList<Skill> skills;
  ToolList tools;
  QStringList warnings;
};

Project loadProject(const QString& cwd, const std::shared_ptr<BackgroundJobs>& jobs) {
  Project project;
  project.cwd = cwd;
  project.instructions = loadInstructions(cwd);
  const QString repo = findRepoRoot(cwd);
  const LoadedSkills loaded = loadSkills(skillRoots(repo.isEmpty() ? cwd : repo));
  project.skills = loaded.skills;
  project.warnings = loaded.warnings;
  project.tools = defaultTools(jobs);
  if (!project.skills.isEmpty()) {
    project.tools.append(skillTool(project.skills));
  }
  return project;
}

// Folders outside the project that sandboxed commands may read: skill
// folders (their scripts run from there), and the repository the project is
// in, for a project that's a submodule or a subfolder.
QStringList visibleFolders(const Project& project) {
  QStringList folders;
  for (const Skill& skill : project.skills) {
    folders << skill.dir;
  }
  const QString repo = findRepoRoot(project.cwd);
  if (!repo.isEmpty() && repo != project.cwd) {
    folders << repo;
  }
  return folders;
}

// A short path for display: relative to the project when nearby, else with ~ for the home folder.
QString displayPath(const QString& file, const QString& cwd) {
  const QString rel = QDir(cwd).relativeFilePath(file);
  if (!rel.startsWith("../..")) {
    return rel;
  }
  return tildify(file);
}

Ui::ProjectView projectView(const Project& project, const QString& cwd) {
  Ui::ProjectView view;
  view.cwd = project.cwd;
  for (const InstructionFile& file : project.instructions) {
    view.instructions << displayPath(file.path, cwd);
  }
  view.skills = project.skills;
  view.tools = project.tools;
  view.warnings = project.warnings;
  return view;
}

// An existing folder, given relative to `base`, absolute, or with ~/.
QString resolveFolder(const QString& folder, const QString& base) {
  const QString dir = QDir::cleanPath(QDir(base).absoluteFilePath(expandHome(folder)));
  if (!QFileInfo(dir).isDir()) {
    throw SmithError("Not a folder: " + folder);
  }
  return dir;
}

std::optional<double> parseNumber(const QString& raw, const QString& flag) {
  if (raw.isEmpty()) {
    return std::nullopt;
  }
  bool ok = false;
  const double n = raw.toDouble(&ok);
  if (!ok) {
    throw SmithError(flag + " must be a number");
  }
  return n;
}

QString resolveReasoning(const QString& flag, const QString& saved) {
  if (flag.isEmpty()) {
    return saved.isEmpty() ? QString("default") : saved;
  }
  if (!isReasoningLevel(flag)) {
    throw SmithError("--reasoning must be one of: " + REASONING_LEVELS.join(", "));
  }
  return flag;
}

QString firstEnv(const QStringList& names) {
  for (const QString& name : names) {
    const QString value = qEnvironmentVariable(name.toUtf8().constData());
    if (!value.isEmpty()) {
      return value;
    }
  }
  return {};
}

// Which server to start on: --endpoint, then an explicit URL (--base-url or
// SMITH_BASE_URL), then the resumed session's endpoint, the last one used,
// the config's default, the first configured, and finally localhost:8080.
ServerPtr chooseServer(const QString& endpointFlag, const QString& baseUrlFlag, const QString& keyFlag, const Config& config, const Preferences& prefs,
                       const QString& sessionEndpoint) {
  if (!endpointFlag.isEmpty()) {
    const Endpoint* endpoint = config.findEndpoint(endpointFlag);
    if (!endpoint) {
      QStringList known;
      for (const Endpoint& e : config.endpoints) {
        known << e.name;
      }
      throw SmithError(QString("Unknown endpoint \"%1\". Configured in %2: %3.").arg(endpointFlag, config.file, known.isEmpty() ? "none" : known.join(", ")));
    }
    return Server::fromEndpoint(*endpoint, keyFlag);
  }
  const QString url = !baseUrlFlag.isEmpty() ? baseUrlFlag : firstEnv({"SMITH_BASE_URL", "AGENT_BASE_URL"});
  QString envKey = !keyFlag.isEmpty() ? keyFlag : firstEnv({"SMITH_API_KEY", "AGENT_API_KEY", "OPENAI_API_KEY"});
  if (envKey.isEmpty()) {
    envKey = "no-key";
  }
  if (!url.isEmpty()) {
    return std::make_shared<Server>(url, envKey);
  }
  const Endpoint* endpoint = config.findEndpoint(sessionEndpoint);
  if (!endpoint) {
    endpoint = config.findEndpoint(prefs.endpoint);
  }
  if (!endpoint) {
    endpoint = config.findEndpoint(config.defaultEndpoint);
  }
  if (!endpoint && !config.endpoints.isEmpty()) {
    endpoint = &config.endpoints.first();
  }
  return endpoint ? Server::fromEndpoint(*endpoint, keyFlag) : std::make_shared<Server>(DEFAULT_BASE_URL, envKey);
}

void printEndpoints(const Config& config) {
  if (config.endpoints.isEmpty()) {
    out(QString("No endpoints configured. Create %1, for example:\n\n%2").arg(config.file, EXAMPLE_CONFIG));
    out("\nUse \"apiKeyEnv\" to read a key from an environment variable instead of storing it in the file.");
    return;
  }
  out(QString("From %1:\n").arg(config.file));
  for (const Endpoint& e : config.endpoints) {
    QStringList marks;
    if (e.name == config.defaultEndpoint) {
      marks << "default";
    }
    if (!e.apiKey.isEmpty()) {
      marks << "key";
    }
    out(QString("%1 %2 %3 %4").arg(e.name.leftJustified(16), e.baseUrl.leftJustified(36), e.model.leftJustified(24), marks.join(", ")));
  }
  for (const QString& warning : config.warnings) {
    out("\nwarning: " + warning);
  }
}

// Every saved session (or one folder's): name, where it is, when, how long, which model.
void printSessions(const QString& cwd) {
  const QList<SessionInfo> sessions = listSessions(cwd);
  if (sessions.isEmpty()) {
    out(cwd.isEmpty() ? "No saved sessions yet." : "No saved sessions for this folder.");
    return;
  }
  const QStringList header = {"NAME", "FOLDER", "LAST USED", "MSGS", "MODEL", "ID"};
  QList<QStringList> rows;
  for (const SessionInfo& s : sessions) {
    rows.append({s.name, Ui::shortPath(s.cwd, 40), QLocale(QLocale::English).toString(s.updated, "M/d/yy, h:mm AP"), QString::number(s.turns),
                 s.endpoint.isEmpty() ? s.model : s.endpoint + "/" + s.model, s.id});
  }
  QList<int> widths;
  for (int i = 0; i < header.size(); i++) {
    int w = header[i].size();
    for (const QStringList& row : rows) {
      w = std::max(w, int(row[i].size()));
    }
    widths << std::min(i < 2 ? 48 : 40, w);
  }
  auto fit = [&](const QStringList& cells) {
    QStringList shown;
    for (int i = 0; i < cells.size(); i++) {
      shown << (cells[i].size() > widths[i] ? cells[i].left(widths[i] - 1) + "…" : cells[i].leftJustified(widths[i]));
    }
    QString line = shown.join("  ");
    while (line.endsWith(' ')) {
      line.chop(1);
    }
    return line;
  };
  out(fit(header));
  for (const QStringList& row : rows) {
    out(fit(row));
  }
  out(QString("\n%1. Resume one with: smith --resume <id or name>").arg(plural(sessions.size(), "session")));
}

// smith --delete: shows the session, asks (unless --yes), and deletes it with its tool log.
void deleteFromCommandLine(const QString& idOrName, bool yes) {
  const QString id = findSession(idOrName);
  QString what = id;
  for (const SessionInfo& info : listSessions()) {
    if (info.id == id) {
      what = QString("\"%1\" (%2) in %3: %4, last used %5")
                 .arg(info.name, id, Ui::shortPath(info.cwd, 60), plural(info.turns, "message"), QLocale(QLocale::English).toString(info.updated));
    }
  }
  if (!yes) {
    if (!Tui::Terminal::isInteractive()) {
      throw SmithError(QString("Not deleting %1 without asking; pass --yes to delete it anyway.").arg(what));
    }
    std::fputs(QString("Delete %1, and its tool log? [y/N] ").arg(what).toUtf8().constData(), stdout);
    std::fflush(stdout);
    std::string answer;
    std::getline(std::cin, answer);
    static const QRegularExpression accept("^y(es)?$", QRegularExpression::CaseInsensitiveOption);
    if (!accept.match(QString::fromStdString(answer).trimmed()).hasMatch()) {
      out("Nothing deleted.");
      return;
    }
  }
  deleteSession(id);
  out(QString("Deleted %1.").arg(what));
}

// Cancelled by SIGINT in headless mode.
CancelPtr g_headlessCancel;

void onHeadlessInterrupt(int) {
  if (g_headlessCancel) {
    g_headlessCancel->cancel();
  }
}

int runHeadless(const std::function<std::shared_ptr<Agent>(const Ui::AgentHooks&)>& createAgent, const QString& prompt, bool allowAll,
                const QString& baseUrl) {
  g_headlessCancel = makeCancel();
  std::signal(SIGINT, onHeadlessInterrupt);

  auto lock = std::make_shared<QMutex>();
  int toolCalls = 0;
  QString reason;
  Ui::AgentHooks hooks;
  hooks.onEvent = [&, lock](const AgentEvent& event) {
    QMutexLocker guard(lock.get());
    switch (event.type) {
    case AgentEvent::Type::Delta:
      if (event.kind == BlockKind::Text) {
        std::fputs(event.text.toUtf8().constData(), stdout);
        std::fflush(stdout);
      }
      break;
    case AgentEvent::Type::ResponseEnd:
      std::fputs("\n", stdout);
      std::fflush(stdout);
      break;
    case AgentEvent::Type::ToolEnd:
      toolCalls++;
      err(QString("[%1 %2] %3\n").arg(event.isError ? "✗" : "✓", event.name, event.summary));
      break;
    case AgentEvent::Type::Notice: {
      const QString level = event.level == NoticeLevel::Info ? "info" : event.level == NoticeLevel::Warn ? "warn" : "error";
      err(QString("[%1] %2\n").arg(level, event.text));
      break;
    }
    case AgentEvent::Type::Done:
      reason = doneReasonName(event.reason);
      break;
    default:
      break;
    }
  };
  hooks.approve = [allowAll](const ApprovalRequest& request) {
    if (allowAll) {
      return ApprovalDecision::Allow;
    }
    err(QString("[denied %1] %2 (pass --yes to allow tools)\n").arg(request.tool, request.summary));
    return ApprovalDecision::Deny;
  };

  const std::shared_ptr<Agent> agent = createAgent(hooks);
  QElapsedTimer clock;
  clock.start();
  int code = 0;
  try {
    agent->send(prompt, g_headlessCancel);
  } catch (const std::exception& error) {
    err(describeError(error, baseUrl) + "\n");
    code = 1;
  }
  err(QString("[%1]\n").arg(Ui::turnSummary(reason, clock.elapsed(), toolCalls)));
  return code;
}

int run(QCoreApplication& app) {
  Http::useSystemNetworkSettings();
  for (const QString& note : moveOldFolders()) {
    err(note + "\n");
  }
  secureSessions();

  QCommandLineParser parser;
  const QCommandLineOption endpoint(QStringList{"e", "endpoint"}, "", "name");
  const QCommandLineOption endpoints("endpoints");
  const QCommandLineOption baseUrl("base-url", "", "url");
  const QCommandLineOption apiKey("api-key", "", "key");
  const QCommandLineOption model("model", "", "name");
  const QCommandLineOption reasoning("reasoning", "", "level");
  const QCommandLineOption contextSize("context-size", "", "n");
  const QCommandLineOption temperature("temperature", "", "n");
  const QCommandLineOption maxSteps("max-steps", "", "n");
  const QCommandLineOption sandboxFlag("sandbox", "", "type");
  const QCommandLineOption network("network");
  const QCommandLineOption continueFlag(QStringList{"c", "continue"});
  const QCommandLineOption resume(QStringList{"r", "resume"}, "", "id");
  const QCommandLineOption sessionsFlag("sessions");
  const QCommandLineOption deleteFlag("delete", "", "id");
  const QCommandLineOption print(QStringList{"p", "print"}, "", "prompt");
  const QCommandLineOption yes(QStringList{"y", "yes"});
  const QCommandLineOption help(QStringList{"h", "help"});
  const QCommandLineOption version(QStringList{"v", "version"});
  parser.addOptions({endpoint, endpoints, baseUrl, apiKey, model, reasoning, contextSize, temperature, maxSteps, sandboxFlag, network, continueFlag, resume,
                     sessionsFlag, deleteFlag, print, yes, help, version});
  if (!parser.parse(app.arguments())) {
    throw SmithError(parser.errorText());
  }
  if (parser.isSet(help)) {
    out(helpText());
    return 0;
  }
  if (parser.isSet(version)) {
    out(QString("smith %1").arg(SMITH_VERSION));
    return 0;
  }

  const QStringList positionals = parser.positionalArguments();
  if (positionals.size() > 1) {
    throw SmithError("Give at most one folder to work in.");
  }
  if (!positionals.isEmpty()) {
    QDir::setCurrent(resolveFolder(positionals.first(), QDir::currentPath()));
  }
  if (parser.isSet(sessionsFlag)) {
    printSessions(positionals.isEmpty() ? QString() : QDir::currentPath());
    return 0;
  }
  if (parser.isSet(deleteFlag)) {
    deleteFromCommandLine(parser.value(deleteFlag), parser.isSet(yes));
    return 0;
  }

  // Resume first: a resumed session suggests which model to use, and where to work.
  std::optional<Session::Opened> resumed;
  if (parser.isSet(resume)) {
    resumed = Session::open(findSession(parser.value(resume)));
    // Work where the session happened, unless a folder was given.
    if (positionals.isEmpty() && pathExists(resumed->session->cwd())) {
      QDir::setCurrent(resumed->session->cwd());
    }
  } else if (parser.isSet(continueFlag)) {
    const QList<SessionInfo> latest = listSessions(QDir::currentPath(), 1);
    if (latest.isEmpty()) {
      throw SmithError("No saved session for this directory.");
    }
    resumed = Session::open(latest.first().id);
  }

  const QString cwd = QDir::currentPath();
  const Config config = loadConfig();
  if (parser.isSet(endpoints)) {
    printEndpoints(config);
    return 0;
  }

  const Preferences prefs = loadPreferences();
  const ServerPtr server = chooseServer(parser.value(endpoint), parser.value(baseUrl), parser.value(apiKey), config, prefs,
                                        resumed ? resumed->session->endpoint() : QString());

  const std::optional<double> temperatureValue = parseNumber(parser.value(temperature), "--temperature");
  const std::optional<double> maxStepsFlag = parseNumber(parser.value(maxSteps), "--max-steps");
  const double maxStepsValue = maxStepsFlag.value_or(config.maxSteps.value_or(50));
  if (maxStepsValue < 1 || maxStepsValue != std::floor(maxStepsValue)) {
    throw SmithError("--max-steps must be a whole number of at least 1");
  }
  const QString reasoningLevel = resolveReasoning(parser.value(reasoning), prefs.reasoning);
  QString modelId = parser.value(model);
  if (modelId.isEmpty()) {
    // The AGENT_ name is from before the rename.
    modelId = firstEnv({"SMITH_MODEL", "AGENT_MODEL"});
  }
  if (modelId.isEmpty()) {
    modelId = server->chooseModel({resumed ? resumed->session->model() : QString(), prefs.models.value(server->baseUrl())});
  }
  std::optional<int> contextSizeValue;
  if (const auto flag = parseNumber(parser.value(contextSize), "--context-size")) {
    contextSizeValue = int(*flag);
  } else {
    contextSizeValue = server->contextSize(modelId);
  }

  QString sandboxName = parser.isSet(sandboxFlag) ? parser.value(sandboxFlag) : sandboxTypeName(config.sandbox.type);
  if (sandboxName == "on") {
    sandboxName = "bwrap";
  }
  if (sandboxName != "bwrap" && sandboxName != "container" && sandboxName != "off") {
    throw SmithError("--sandbox must be bwrap, container or off");
  }
  SandboxOptions sandboxOptions;
  sandboxOptions.type = sandboxName == "container" ? SandboxType::Container : sandboxName == "off" ? SandboxType::Off : SandboxType::Bwrap;
  sandboxOptions.network = parser.isSet(network) || config.sandbox.network;
  sandboxOptions.env = config.sandbox.env;
  sandboxOptions.limits = config.sandbox.limits;
  sandboxOptions.home = config.sandbox.home;
  sandboxOptions.container = config.sandbox.container;
  const std::shared_ptr<Sandbox> sandbox = Sandbox::create(sandboxOptions);

  // Commands the agent runs in the background; they're stopped when Smith exits.
  const auto jobs = std::make_shared<BackgroundJobs>();

  // What can change while the TUI runs: the project (/new <folder>) and the session (/sessions, /new).
  struct State {
    Project project;
    std::shared_ptr<Session> session;
  };
  auto state = std::make_shared<State>();
  state->project = loadProject(cwd, jobs);
  sandbox->setReadOnlyPaths(visibleFolders(state->project));
  const QStringList warnings = config.warnings + sandbox->warnings() + state->project.warnings;

  state->session = resumed ? resumed->session : Session::create(cwd, modelId, server->name());
  state->session->recordModel(modelId, server->name());
  const Messages history = resumed ? resumed->messages : Messages{};
  std::shared_ptr<ToolLog> toolLog;
  if (config.toolLog) {
    toolLog = std::make_shared<ToolLog>([state]() { return state->session->id(); }, [state]() { return state->project.cwd; });
  }

  // An agent for the current project. The first one continues the resumed
  // conversation with the startup settings; later ones (a new project) start
  // empty with the settings in use at that moment.
  auto createAgent = [=](const Ui::AgentHooks& hooks, const std::optional<Ui::AgentSettings>& current) {
    const Project& project = state->project;
    PromptExtras extras;
    extras.instructions = project.instructions;
    extras.skills = project.skills;
    extras.fastTools = fastTools();
    extras.canAsk = bool(hooks.ask);
    extras.sandbox = sandbox->promptNotes(project.cwd);
    PromptExtras subagentExtras;
    subagentExtras.instructions = project.instructions;
    subagentExtras.skills = project.skills;

    AgentOptions options;
    options.server = current ? current->server : server;
    options.model = current ? current->model : modelId;
    options.reasoning = current ? current->reasoning : reasoningLevel;
    options.contextSize = current ? current->contextSize : contextSizeValue;
    options.history = current ? Messages{} : history;
    options.cwd = project.cwd;
    options.temperature = temperatureValue;
    options.maxSteps = int(maxStepsValue);
    options.sandbox = sandbox;
    options.tools = project.tools;
    options.systemPrompt = buildSystemPrompt(project.cwd, extras);
    if (config.subagents) {
      options.subagentPrompt = buildSubagentPrompt(project.cwd, subagentExtras);
    }
    options.onHistory = [state](const HistoryChange& change) { state->session->record(change); };
    options.protectRunLater = config.protectRunLater;
    if (toolLog) {
      options.onToolRun = [toolLog](const ToolRun& run) { toolLog->write(run); };
    }
    options.onEvent = hooks.onEvent;
    options.approve = hooks.approve;
    options.ask = hooks.ask;
    return std::make_shared<Agent>(options);
  };

  int code = 0;
  if (parser.isSet(print)) {
    for (const QString& warning : warnings) {
      err("[warn] " + warning + "\n");
    }
    err(QString("[%1 · %2 · %3]\n").arg(server->label(), modelId, sandbox->describe(state->project.cwd)));
    code = runHeadless([&](const Ui::AgentHooks& hooks) { return createAgent(hooks, std::nullopt); }, parser.value(print), parser.isSet(yes),
                       server->baseUrl());
    // Their open output pipes would keep the process alive.
    jobs->stopAll();
    err(QString("[session %1]\n").arg(state->session->id()));
    sandbox->dispose();
    return code;
  }

  if (!Tui::Terminal::isInteractive()) {
    throw SmithError("The interactive UI needs a terminal. Use -p <prompt> for non-interactive runs.");
  }

  Ui::TuiOptions options;
  options.createAgent = createAgent;
  options.server = server;
  options.endpoints = config.endpoints;
  options.project = projectView(state->project, cwd);
  options.switchProject = [state, sandbox, jobs](const QString& folder) {
    const QString dir = resolveFolder(folder, state->project.cwd);
    state->project = loadProject(dir, jobs);
    sandbox->setReadOnlyPaths(visibleFolders(state->project));
    return projectView(state->project, dir);
  };
  options.configFile = config.file;
  options.warnings = warnings;
  options.rememberedModel = [](const QString& url) { return loadPreferences().models.value(url); };
  options.sandbox = sandbox;
  options.sessions.currentId = [state]() { return state->session->id(); };
  options.sessions.currentName = [state]() { return state->session->name(); };
  options.sessions.list = [state](bool everywhere) { return listSessions(everywhere ? QString() : state->project.cwd, 50); };
  options.sessions.open = [state](const QString& id) {
    const Session::Opened opened = Session::open(id);
    state->session = opened.session;
    return Ui::OpenedSession{opened.messages, opened.session->model(), opened.session->endpoint(), opened.session->cwd(), opened.session->lastUsed()};
  };
  options.sessions.rename = [state](const QString& name) { state->session->rename(name); };
  options.sessions.remove = [state](const QString& id) {
    if (id == state->session->id()) {
      throw SmithError("That's the current session.");
    }
    deleteSession(id);
  };
  options.sessions.startNew = [state](const QString& model, const QString& endpointName) {
    state->session = Session::create(state->project.cwd, model, endpointName);
  };
  options.sessions.recentProjects = []() {
    QList<Ui::RecentProject> projects;
    QMap<QString, int> index;
    for (const SessionInfo& info : listSessions({}, 200)) {
      if (index.contains(info.cwd)) {
        projects[index.value(info.cwd)].sessions++;
      } else {
        index.insert(info.cwd, projects.size());
        projects.append(Ui::RecentProject{info.cwd, 1, info.updated});
      }
    }
    return projects;
  };
  options.resumed = history;
  options.resumedLastUsed = resumed ? resumed->session->lastUsed() : std::nullopt;
  options.theme = config.theme;
  options.codeStream = config.codeStream;
  options.jobs = jobs;
  options.savePreferences = [state](const Ui::PreferenceUpdate& update) {
    if (!update.model.isEmpty()) {
      state->session->recordModel(update.model, update.server->name());
    }
    PreferenceChange change;
    change.reasoning = update.reasoning;
    if (!update.model.isEmpty()) {
      change.endpoint = update.server->name();
      change.modelBaseUrl = update.server->baseUrl();
      change.modelId = update.model;
    }
    savePreferences(change);
  };

  QElapsedTimer started;
  started.start();
  {
    Ui::App tui(options);
    QObject::connect(&tui, &Ui::App::finished, &app, &QCoreApplication::quit, Qt::QueuedConnection);
    tui.run();
    app.exec();
  }
  jobs->stopAll();
  sandbox->dispose();
  const QString name = state->session->name();
  static const QRegularExpression whitespace(R"(\s)");
  const QString resumeWith = !name.isEmpty() && !whitespace.match(name).hasMatch() ? name : state->session->id();
  const QString goodbye = config.theme == Theme::Matrix ? Ui::farewell(started.elapsed()) + "\n" : QString();
  out(QString("%1Session %2%3. Resume with: smith --resume %4").arg(goodbye, name.isEmpty() ? QString() : "\"" + name + "\" ", state->session->id(), resumeWith));
  return code;
}

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName("smith");
  QCoreApplication::setApplicationVersion(SMITH_VERSION);
  try {
    return run(app);
  } catch (const std::exception& error) {
    err(errorMessage(error) + "\n");
    return 1;
  }
}
