#include "frontend/Runtime.h"

#include "core/CustomTools.h"
#include "core/Dirs.h"
#include "core/Errors.h"
#include "core/Preferences.h"
#include "core/Prompt.h"
#include "core/Reasoning.h"
#include "core/Toolchain.h"
#include "core/tools/Tools.h"
#include "frontend/Worktrees.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QMutexLocker>

#include <cmath>

namespace Smith {

namespace {

const QString DEFAULT_BASE_URL = "http://localhost:8080/v1";

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
ServerPtr chooseServer(const LaunchOptions& options, const Config& config, const Preferences& prefs, const QString& sessionEndpoint) {
  if (!options.endpoint.isEmpty()) {
    const Endpoint* endpoint = config.findEndpoint(options.endpoint);
    if (!endpoint) {
      QStringList known;
      for (const Endpoint& e : config.endpoints) {
        known << e.name;
      }
      throw SmithError(QString("Unknown endpoint \"%1\". Configured in %2: %3.").arg(options.endpoint, config.file, known.isEmpty() ? "none" : known.join(", ")));
    }
    return Server::fromEndpoint(*endpoint, options.apiKey);
  }
  const QString url = !options.baseUrl.isEmpty() ? options.baseUrl : firstEnv({"SMITH_BASE_URL", "AGENT_BASE_URL"});
  QString envKey = !options.apiKey.isEmpty() ? options.apiKey : firstEnv({"SMITH_API_KEY", "AGENT_API_KEY", "OPENAI_API_KEY"});
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
  return endpoint ? Server::fromEndpoint(*endpoint, options.apiKey) : std::make_shared<Server>(DEFAULT_BASE_URL, envKey);
}

// A short path for display: relative to where Smith started when nearby, else with ~ for the home folder.
QString displayPath(const QString& file, const QString& cwd) {
  const QString rel = QDir(cwd).relativeFilePath(file);
  if (!rel.startsWith("../..")) {
    return rel;
  }
  return tildify(file);
}

} // namespace

void addLaunchOptions(QCommandLineParser& parser) {
  parser.addOptions({
      QCommandLineOption(QStringList{"e", "endpoint"}, "", "name"),
      QCommandLineOption("base-url", "", "url"),
      QCommandLineOption("api-key", "", "key"),
      QCommandLineOption("model", "", "name"),
      QCommandLineOption("reasoning", "", "level"),
      QCommandLineOption("context-size", "", "n"),
      QCommandLineOption("temperature", "", "n"),
      QCommandLineOption("max-steps", "", "n"),
      QCommandLineOption("sandbox", "", "type"),
      QCommandLineOption("network"),
      QCommandLineOption(QStringList{"c", "continue"}),
      QCommandLineOption(QStringList{"r", "resume"}, "", "id"),
  });
}

LaunchOptions readLaunchOptions(const QCommandLineParser& parser) {
  const QStringList positionals = parser.positionalArguments();
  if (positionals.size() > 1) {
    throw SmithError("Give at most one folder to work in.");
  }
  LaunchOptions options;
  options.folder = positionals.value(0);
  options.endpoint = parser.value("endpoint");
  options.baseUrl = parser.value("base-url");
  options.apiKey = parser.value("api-key");
  options.model = parser.value("model");
  options.reasoning = parser.value("reasoning");
  options.contextSize = parser.value("context-size");
  options.temperature = parser.value("temperature");
  options.maxSteps = parser.value("max-steps");
  options.sandbox = parser.value("sandbox");
  options.network = parser.isSet("network");
  options.resume = parser.value("resume");
  options.continueLast = parser.isSet("continue");
  return options;
}

QString resolveFolder(const QString& folder, const QString& base) {
  const QString dir = QDir::cleanPath(QDir(base).absoluteFilePath(expandHome(folder)));
  if (!QFileInfo(dir).isDir()) {
    throw SmithError("Not a folder: " + folder);
  }
  return dir;
}

std::shared_ptr<Runtime> Runtime::start(const LaunchOptions& options, const CancelPtr& cancel) {
  std::shared_ptr<Runtime> runtime(new Runtime);
  if (!options.folder.isEmpty()) {
    QDir::setCurrent(resolveFolder(options.folder, QDir::currentPath()));
  }

  // Resume first: a resumed session suggests which model to use, and where to work.
  std::optional<Session::Opened> resumed;
  if (!options.resume.isEmpty()) {
    resumed = Session::open(findSession(options.resume));
    // Work where the session happened, unless a folder was given.
    if (options.folder.isEmpty() && pathExists(resumed->session->cwd())) {
      QDir::setCurrent(resumed->session->cwd());
    }
  } else if (options.continueLast) {
    const QList<SessionInfo> latest = listSessions(QDir::currentPath(), 1);
    if (latest.isEmpty()) {
      throw SmithError("No saved session for this directory.");
    }
    resumed = Session::open(latest.first().id);
  }

  const QString cwd = QDir::currentPath();
  runtime->m_config = loadConfig();
  const Config& config = runtime->m_config;
  const Preferences prefs = loadPreferences();
  runtime->m_pServer = chooseServer(options, config, prefs, resumed ? resumed->session->endpoint() : QString());

  runtime->m_temperature = parseNumber(options.temperature, "--temperature");
  const double maxSteps = parseNumber(options.maxSteps, "--max-steps").value_or(config.maxSteps.value_or(50));
  if (maxSteps < 1 || maxSteps != std::floor(maxSteps)) {
    throw SmithError("--max-steps must be a whole number of at least 1");
  }
  runtime->m_maxSteps = int(maxSteps);
  runtime->m_reasoning = resolveReasoning(options.reasoning, prefs.reasoning);
  QString model = options.model;
  if (model.isEmpty()) {
    // The AGENT_ name is from before the rename.
    model = firstEnv({"SMITH_MODEL", "AGENT_MODEL"});
  }
  if (model.isEmpty()) {
    model = runtime->m_pServer->chooseModel({resumed ? resumed->session->model() : QString(), prefs.models.value(runtime->m_pServer->baseUrl())}, cancel);
  }
  runtime->m_model = model;
  if (const auto flag = parseNumber(options.contextSize, "--context-size")) {
    runtime->m_contextSize = int(*flag);
  } else {
    runtime->m_contextSize = runtime->m_pServer->contextSize(model, cancel);
    if (cancelled(cancel)) {
      throw AbortError();
    }
  }

  QString sandboxName = options.sandbox.isEmpty() ? sandboxTypeName(config.sandbox.type) : options.sandbox;
  if (sandboxName == "on") {
    sandboxName = "bwrap";
  }
  if (sandboxName != "bwrap" && sandboxName != "container" && sandboxName != "off") {
    throw SmithError("--sandbox must be bwrap, container or off");
  }
  SandboxOptions sandboxOptions;
  sandboxOptions.type = sandboxName == "container" ? SandboxType::Container : sandboxName == "off" ? SandboxType::Off : SandboxType::Bwrap;
  sandboxOptions.network = options.network || config.sandbox.network;
  sandboxOptions.env = config.sandbox.env;
  sandboxOptions.limits = config.sandbox.limits;
  sandboxOptions.home = config.sandbox.home;
  sandboxOptions.container = config.sandbox.container;
  runtime->m_pSandbox = Sandbox::create(sandboxOptions);

  // Commands the agent runs in the background; they're stopped when Smith exits.
  runtime->m_pJobs = std::make_shared<BackgroundJobs>();
  // A frontend may start this on a worker thread; the jobs' signals belong to the application's.
  if (QCoreApplication::instance()) {
    runtime->m_pJobs->moveToThread(QCoreApplication::instance()->thread());
  }

  runtime->m_project = runtime->loadProject(cwd);
  runtime->m_pSandbox->setReadOnlyPaths(visibleFolders(runtime->m_project));
  markWorktreeOpen(runtime->m_project.worktreeRoot);
  runtime->m_warnings = config.warnings + runtime->m_pSandbox->warnings() + runtime->m_project.warnings;

  runtime->m_pSession = resumed ? resumed->session : Session::create(cwd, model, runtime->m_pServer->name());
  runtime->m_pSession->recordModel(model, runtime->m_pServer->name());
  if (resumed) {
    runtime->m_resumed = resumed->messages;
    runtime->m_resumedLastUsed = resumed->session->lastUsed();
  }
  if (config.toolLog) {
    const std::weak_ptr<Runtime> weak = runtime;
    runtime->m_pToolLog = std::make_shared<ToolLog>(
        [weak]() {
          const auto self = weak.lock();
          return self ? self->session()->id() : QString();
        },
        [weak]() {
          const auto self = weak.lock();
          return self ? self->projectCwd() : QString();
        });
  }
  return runtime;
}

Runtime::~Runtime() {
  shutdown();
}

void Runtime::shutdown() {
  if (m_shutDown) {
    return;
  }
  m_shutDown = true;
  {
    QMutexLocker lock(&m_mutex);
    clearWorktreeOpen(m_project.worktreeRoot);
  }
  if (m_pJobs) {
    m_pJobs->stopAll();
  }
  if (m_pSandbox) {
    m_pSandbox->dispose();
  }
}

std::shared_ptr<Session> Runtime::session() const {
  QMutexLocker lock(&m_mutex);
  return m_pSession;
}

QString Runtime::projectCwd() const {
  QMutexLocker lock(&m_mutex);
  return m_project.cwd;
}

ProjectView Runtime::project() const {
  QMutexLocker lock(&m_mutex);
  ProjectView view;
  view.cwd = m_project.cwd;
  for (const InstructionFile& file : m_project.instructions) {
    view.instructions << displayPath(file.path, m_project.cwd);
  }
  view.skills = m_project.skills;
  view.tools = m_project.tools;
  view.customTools = m_project.customTools;
  view.warnings = m_project.warnings;
  view.worktreeBranch = m_project.worktreeBranch;
  return view;
}

Runtime::Project Runtime::loadProject(const QString& cwd) const {
  Project project;
  project.cwd = cwd;
  project.instructions = loadInstructions(cwd);
  const QString repo = findRepoRoot(cwd);
  const LoadedSkills loaded = loadSkills(skillRoots(repo.isEmpty() ? cwd : repo));
  project.skills = loaded.skills;
  project.warnings = loaded.warnings;
  project.tools = defaultTools(m_pJobs);
  if (!project.skills.isEmpty()) {
    project.tools.append(skillTool(project.skills));
  }
  // Tools of your own run only in the sandbox, so without one they aren't offered.
  const QList<ToolRoot> roots = toolRoots(repo.isEmpty() ? cwd : repo);
  if (m_pSandbox && m_pSandbox->active()) {
    QStringList reserved{"task", "todo_write", ASK_TOOL};
    for (const ToolDef& tool : project.tools) {
      reserved << tool.name;
    }
    const LoadedTools custom = loadCustomTools(roots, reserved);
    for (const ToolDef& tool : custom.tools) {
      project.customTools << tool.name;
    }
    project.tools << custom.tools;
    project.toolDirs = custom.dirs;
    project.warnings << custom.warnings;
  } else if (const int skipped = countCustomTools(roots)) {
    project.warnings << QString("%1 of your own %2 offered: they only run in the sandbox, which is off.")
                            .arg(plural(skipped, "tool"), skipped == 1 ? "isn't" : "aren't");
  }
  if (const std::optional<GitDirs> dirs = repo.isEmpty() ? std::nullopt : gitDirs(repo); dirs && dirs->linked) {
    project.worktreeRoot = repo;
    project.worktreeBranch = currentBranch(repo);
    if (project.worktreeBranch.isEmpty()) {
      project.worktreeBranch = "a detached HEAD";
    }
    project.gitCommonDir = dirs->commonDir;
  }
  return project;
}

QStringList Runtime::visibleFolders(const Project& project) {
  QStringList folders;
  for (const Skill& skill : project.skills) {
    folders << skill.dir;
  }
  folders << project.toolDirs;
  // A worktree's git data is in the main repository's .git: shown read-only,
  // so git works in the sandbox but can't commit.
  if (!project.gitCommonDir.isEmpty()) {
    folders << project.gitCommonDir;
  }
  const QString repo = findRepoRoot(project.cwd);
  if (!repo.isEmpty() && repo != project.cwd) {
    folders << repo;
  }
  return folders;
}

std::shared_ptr<Agent> Runtime::createAgent(const AgentHooks& hooks, const std::optional<AgentSettings>& current) {
  Project project;
  {
    QMutexLocker lock(&m_mutex);
    project = m_project;
  }
  PromptExtras extras;
  extras.instructions = project.instructions;
  extras.skills = project.skills;
  extras.fastTools = fastTools();
  extras.canAsk = bool(hooks.ask);
  extras.sandbox = m_pSandbox->promptNotes(project.cwd);
  extras.worktreeBranch = project.worktreeBranch;
  extras.workers = m_config.subagents;
  PromptExtras subagentExtras;
  subagentExtras.instructions = project.instructions;
  subagentExtras.skills = project.skills;

  AgentOptions options;
  options.server = current ? current->server : m_pServer;
  options.model = current ? current->model : m_model;
  options.reasoning = current ? current->reasoning : m_reasoning;
  options.contextSize = current ? current->contextSize : m_contextSize;
  options.planning = current && current->planning;
  options.history = current ? Messages{} : m_resumed;
  options.cwd = project.cwd;
  options.temperature = m_temperature;
  options.settingsFor = [config = std::make_shared<const Config>(m_config)](const QString& endpoint, const QString& model) {
    return config->settingsFor(endpoint, model);
  };
  options.maxSteps = m_maxSteps;
  options.sandbox = m_pSandbox;
  options.tools = project.tools;
  options.systemPrompt = buildSystemPrompt(project.cwd, extras);
  if (m_config.subagents) {
    options.subagentPrompt = buildSubagentPrompt(project.cwd, subagentExtras);
    PromptExtras workerExtras = subagentExtras;
    workerExtras.fastTools = extras.fastTools;
    workerExtras.sandbox = extras.sandbox;
    workerExtras.worktreeBranch = extras.worktreeBranch;
    options.workerPrompt = buildWorkerPrompt(project.cwd, workerExtras);
  }
  const std::weak_ptr<Runtime> weak = weak_from_this();
  options.onHistory = [weak](const HistoryChange& change) {
    if (const auto self = weak.lock()) {
      self->session()->record(change);
    }
  };
  options.protectRunLater = m_config.protectRunLater;
  if (m_pToolLog) {
    options.onToolRun = [log = m_pToolLog](const ToolRun& run) { log->write(run); };
  }
  // What runs without asking here: the config file's rules, and what the user allowed in this folder before.
  options.alwaysAllowed = std::make_shared<AllowedTools>();
  options.alwaysAllowed->setConfigured(m_config.allow, m_config.deny);
  const QString folder = project.cwd;
  options.alwaysAllowed->setRemembered(loadPreferences().allowed.value(folder), [folder](const QStringList& rules) {
    PreferenceChange change;
    change.allowedIn = folder;
    change.allowed = rules;
    Smith::savePreferences(change);
  });
  options.onEvent = hooks.onEvent;
  options.approve = hooks.approve;
  options.ask = hooks.ask;
  return std::make_shared<Agent>(options);
}

ProjectView Runtime::switchProject(const QString& folder) {
  const QString dir = resolveFolder(folder, projectCwd());
  Project next = loadProject(dir);
  m_pSandbox->setReadOnlyPaths(visibleFolders(next));
  QString left;
  {
    QMutexLocker lock(&m_mutex);
    left = m_project.worktreeRoot;
    m_project = next;
  }
  if (left != next.worktreeRoot) {
    clearWorktreeOpen(left);
  }
  markWorktreeOpen(next.worktreeRoot);
  return project();
}

SessionControl Runtime::sessions() {
  const std::shared_ptr<Runtime> self = shared_from_this();
  SessionControl control;
  control.currentId = [self]() { return self->session()->id(); };
  control.currentName = [self]() { return self->session()->name(); };
  control.list = [self](bool everywhere) { return listSessions(everywhere ? QString() : self->projectCwd(), 50); };
  control.open = [self](const QString& id) {
    const Session::Opened opened = Session::open(id);
    {
      QMutexLocker lock(&self->m_mutex);
      self->m_pSession = opened.session;
    }
    return OpenedSession{opened.messages, opened.session->model(), opened.session->endpoint(), opened.session->cwd(), opened.session->lastUsed()};
  };
  control.rename = [self](const QString& name) { self->session()->rename(name); };
  control.remove = [self](const QString& id) {
    if (id == self->session()->id()) {
      throw SmithError("That's the current session.");
    }
    deleteSession(id);
  };
  control.startNew = [self](const QString& model, const QString& endpoint) {
    auto next = Session::create(self->projectCwd(), model, endpoint);
    QMutexLocker lock(&self->m_mutex);
    self->m_pSession = next;
  };
  control.recentProjects = []() {
    QList<RecentProject> projects;
    QMap<QString, int> index;
    for (const SessionInfo& info : listSessions({}, 200)) {
      if (index.contains(info.cwd)) {
        projects[index.value(info.cwd)].sessions++;
      } else {
        index.insert(info.cwd, projects.size());
        projects.append(RecentProject{info.cwd, 1, info.updated});
      }
    }
    return projects;
  };
  return control;
}

void Runtime::savePreferences(const PreferenceUpdate& update) {
  if (!update.model.isEmpty()) {
    session()->recordModel(update.model, update.server->name());
  }
  PreferenceChange change;
  change.reasoning = update.reasoning;
  change.theme = update.theme;
  if (!update.model.isEmpty()) {
    change.endpoint = update.server->name();
    change.modelBaseUrl = update.server->baseUrl();
    change.modelId = update.model;
  }
  Smith::savePreferences(change);
}

QString Runtime::rememberedModel(const QString& baseUrl) {
  return loadPreferences().models.value(baseUrl);
}

} // namespace Smith
