#include "core/Sandbox.h"
#include "compat/Utf8.h"

#include "core/ContainerSandbox.h"
#include "core/Dirs.h"
#include "core/Errors.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSaveFile>

#include <cmath>
#include <cstdlib>

namespace Smith {

QString sandboxTypeName(SandboxType type) {
  switch (type) {
  case SandboxType::Bwrap:
    return "bwrap";
  case SandboxType::Container:
    return "container";
  case SandboxType::Off:
    return "off";
  }
  return "off";
}

namespace {

// Environment variables the agent's commands get, in the bwrap sandbox and
// without one: the basics, and what
// compilers and build tools read (including what RHEL's gcc-toolset, scl and
// environment modules set). Left out: anything that commonly holds
// credentials, such as NPM_CONFIG_* or PIP_INDEX_URL; sandbox.env adds more.
const QRegularExpression ENV_ALLOW(
    "^("
    "PATH|HOME|USER|LOGNAME|SHELL|TERM|TZ|LANG|LANGUAGE|LC_[A-Z_]+|COLORTERM|NO_COLOR|FORCE_COLOR|CI|EDITOR"
    "|CC|CXX|CPP|FC|F77|LD|AR|AS|NM|RANLIB|STRIP|CFLAGS|CXXFLAGS|CPPFLAGS|FFLAGS|LDFLAGS|LDLIBS|MAKEFLAGS|CMAKE_[A-Z_]+"
    "|LD_LIBRARY_PATH|LIBRARY_PATH|CPATH|C_INCLUDE_PATH|CPLUS_INCLUDE_PATH|PKG_CONFIG_PATH|PKG_CONFIG_LIBDIR|MANPATH|INFOPATH"
    "|X_SCLS|PCP_DIR|MODULEPATH|MODULESHOME|LOADEDMODULES|_LMFILES_|MODULES_[A-Z_]+|LMOD_[A-Z_]+"
    "|JAVA_HOME|MAVEN_HOME|M2_HOME|MAVEN_OPTS|GRADLE_HOME|GRADLE_USER_HOME|GRADLE_OPTS|JAVA_TOOL_OPTIONS"
    "|GOPATH|GOROOT|GOFLAGS|GOCACHE|GOMODCACHE|GOPROXY|GOPRIVATE|GONOSUMDB|GOTOOLCHAIN|CGO_ENABLED"
    "|CARGO_HOME|RUSTUP_HOME|RUSTFLAGS|CARGO_TARGET_DIR"
    "|VIRTUAL_ENV|CONDA_PREFIX|CONDA_DEFAULT_ENV|PYTHONPATH|PYTHONHOME|NODE_PATH|NODE_OPTIONS|GEM_HOME|GEM_PATH|PERL5LIB"
    "|HTTP_PROXY|HTTPS_PROXY|NO_PROXY|ALL_PROXY|http_proxy|https_proxy|no_proxy|all_proxy"
    "|SSL_CERT_FILE|SSL_CERT_DIR|REQUESTS_CA_BUNDLE|CURL_CA_BUNDLE|NODE_EXTRA_CA_CERTS"
    ")$");

// Variables holding this agent's own credentials, removed even when unsandboxed.
const QStringList AGENT_SECRETS = {"SMITH_API_KEY", "AGENT_API_KEY", "OPENAI_API_KEY"};

// Replaced by empty directories. /tmp and /var/tmp become private scratch space.
const QStringList HIDDEN_SYSTEM_DIRS = {"/tmp", "/var/tmp", "/run", "/mnt", "/media"};

// The temp directories, and the folder under the private temp folder that stands in for each.
const QList<QPair<QString, QString>> TEMP_DIRS = {{"/tmp", "tmp"}, {"/var/tmp", "var-tmp"}};

// Relative to the home directory.
const QStringList HIDDEN_HOME_PATHS = {
    ".ssh", ".gnupg", ".aws", ".azure", ".config/gcloud", ".kube", ".docker", ".config/containers", ".netrc",
    ".git-credentials", ".password-store", ".local/share/keyrings", ".config/gh", ".npmrc", ".pypirc",
    ".cargo/credentials", ".cargo/credentials.toml", ".mozilla", ".config/google-chrome", ".config/chromium",
    ".bash_history", ".zsh_history", ".python_history", ".local/share/fish/fish_history", ".claude", ".claude.json",
    // Smith's own sessions and settings (and where they were before the rename).
    ".config/smith", ".local/share/smith", ".config/agent", ".local/share/agent",
};

// /proc holds every process's environment, including the agent's API keys.
const QStringList HIDDEN_FROM_FILE_TOOLS = {"/proc", "/sys", "/dev"};

const QRegularExpression READ_ONLY("Read-only file system|EROFS");
const QRegularExpression NO_NETWORK(
    "Could not resolve host|Temporary failure in name resolution|Name or service not known|getaddrinfo (ENOTFOUND|EAI_AGAIN)|"
    "Network is unreachable|ENETUNREACH|Failed to establish a new connection|Could not resolve|no such host|Cannot download|"
    "Connection refused.*:443|Failed to connect to",
    QRegularExpression::CaseInsensitiveOption);

std::optional<QString> networkNote(const QString& output, bool network) {
  if (network || !NO_NETWORK.match(output).hasMatch()) {
    return std::nullopt;
  }
  return QString("[Sandbox: the network is off, so this couldn't download or connect. Don't work around it; tell the user they can "
                 "turn it on with /network on, or carry on without it.]");
}

QStringList splitPath() {
  return qEnvironmentVariable("PATH").split(':', Qt::SkipEmptyParts);
}

// Folders on PATH inside the home folder, so your own tools keep working
// while the rest of home is hidden. For a "bin" folder its parent is shown,
// except for ~/bin and ~/.local/bin, whose parents hold much more.
QStringList pathFoldersInHome() {
  const QString home = homeDir();
  QStringList folders;
  for (const QString& entry : splitPath()) {
    if (!isWithin(home, entry) || entry == home || !pathExists(entry)) {
      continue;
    }
    const QString parent = QFileInfo(entry).path();
    const bool tooBroad = parent == home || parent == home + "/.local";
    const QString folder = QFileInfo(entry).fileName() == "bin" && !tooBroad ? parent : entry;
    if (!folders.contains(folder)) {
      folders << folder;
    }
  }
  return folders;
}

bool systemdScopes() {
  static const bool works = []() {
    SpawnSpec spec{"systemd-run", {"--user", "--scope", "--quiet", "-p", "TasksMax=100", "true"}, hostEnv(), {}, {}};
    RunOptions options;
    options.timeoutMs = 5000;
    const ProcessResult result = runProcess(spec, options);
    return result.started && result.exitCode == 0 && result.stoppedBy == ProcessResult::Stop::None;
  }();
  return works;
}

// The command that runs each shell command under the limits: systemd-run in a
// scope of its own when your user's systemd can do that, else prlimit (processes only).
QStringList limiterFor(const std::optional<Limits>& limits, QStringList& warnings) {
  if (!limits || !limits->any()) {
    return {};
  }
  if (systemdScopes()) {
    QStringList props;
    // Swap counts too, or a command could get past the limit by swapping.
    if (limits->memory) {
      props << "-p" << QString("MemoryMax=%1").arg(*limits->memory) << "-p" << "MemorySwapMax=0";
    }
    if (limits->processes) {
      props << "-p" << QString("TasksMax=%1").arg(*limits->processes);
    }
    if (limits->cpus) {
      props << "-p" << QString("CPUQuota=%1%").arg(std::lround(*limits->cpus * 100));
    }
    // systemd-run needs the session's bus address; env -u keeps it out of the sandbox.
    return QStringList{"systemd-run", "--user", "--scope", "--quiet"} + props +
           QStringList{"--", "env", "-u", "DBUS_SESSION_BUS_ADDRESS", "-u", "XDG_RUNTIME_DIR"};
  }
  QStringList missing;
  if (limits->memory) {
    missing << "memory";
  }
  if (limits->cpus) {
    missing << "cpus";
  }
  if (!missing.isEmpty()) {
    warnings << QString("The sandbox's %1 limit needs systemd-run --user --scope, which doesn't work here; only the process limit applies.")
                    .arg(missing.join(" and "));
  }
  if (limits->processes) {
    return {"prlimit", QString("--nproc=%1").arg(*limits->processes), "--"};
  }
  return {};
}

// Hides a path: an empty folder over a directory, /dev/null over a file.
// Symlinks are followed, and the real location is hidden.
void hide(QStringList& args, const QString& target) {
  const QString real = realPath(target);
  if (real.isEmpty()) {
    return;
  }
  if (isDirectory(real)) {
    args << "--tmpfs" << real;
  } else {
    args << "--ro-bind" << "/dev/null" << real;
  }
}

// Returns why bwrap can't be used here, or an empty string if it works.
QString probeBwrap() {
  SpawnSpec spec{"bwrap", {"--unshare-all", "--die-with-parent", "--ro-bind", "/", "/", "--", "true"}, hostEnv(), {}, {}};
  RunOptions options;
  options.timeoutMs = 5000;
  const ProcessResult result = runProcess(spec, options);
  if (!result.started) {
    return result.missing ? QString("bubblewrap (bwrap) is not installed") : result.startError;
  }
  if (result.exitCode != 0) {
    QString detail = utf8(result.err).trimmed();
    if (detail.isEmpty()) {
      detail = QString("exit code %1").arg(result.exitCode);
    }
    return QString("bwrap failed (%1); unprivileged user namespaces may be disabled (sysctl user.max_user_namespaces)").arg(detail);
  }
  return {};
}

class NoSandbox : public Sandbox {
public:
  NoSandbox(const QString& reason, const QStringList& extraEnv) : m_reason(reason), m_extraEnv(extraEnv) {}

  SandboxType type() const override { return SandboxType::Off; }
  bool active() const override { return false; }
  QString reason() const override { return m_reason; }
  bool network() const override { return true; }
  void setNetwork(bool) override {}

  QString describe(const QString&) const override { return QString("not sandboxed (%1)").arg(m_reason); }

  // Nothing is isolated, but tokens and keys in your environment still stay out of the agent's commands.
  using Sandbox::spawnSpec;
  SpawnSpec spawnSpec(const QString& command, const QString& cwd, const CommandAccess& access) override {
    QProcessEnvironment env = sandboxEnv(ENV_ALLOW, m_extraEnv);
    addVariables(env, access.env);
    return SpawnSpec{"bash", {"-c", command}, env, cwd, {}};
  }

  SpawnSpec programSpec(const QString& file, const QStringList& args, const QString& project, const QString& chdir) override {
    return SpawnSpec{file, args, sandboxEnv(ENV_ALLOW, m_extraEnv), chdir.isEmpty() ? project : chdir, {}};
  }

  QByteArray readBytes(const QString& file, const QString&) override {
    QFileInfo info(file);
    if (!info.exists()) {
      throw SmithError(QString("ENOENT: no such file or directory, open '%1'").arg(file), "ENOENT");
    }
    if (info.isDir()) {
      throw SmithError(QString("EISDIR: illegal operation on a directory, read"), "EISDIR");
    }
    QFile handle(file);
    if (!handle.open(QIODevice::ReadOnly)) {
      throw SmithError(QString("EACCES: permission denied, open '%1'").arg(file), "EACCES");
    }
    return handle.readAll();
  }

  void writeBytes(const QString& file, const QByteArray& content, const QString&) override {
    QDir().mkpath(QFileInfo(file).path());
    // Written beside the file and renamed over it, so a crash can't leave half of it.
    QSaveFile handle(file);
    // Where the folder isn't writable but the file is, write in place.
    handle.setDirectWriteFallback(true);
    if (!handle.open(QIODevice::WriteOnly) || handle.write(content) < 0 || !handle.commit()) {
      throw SmithError(QString("Could not write %1: %2").arg(file, handle.errorString()), "EACCES");
    }
  }

  std::optional<SandboxPath> sandboxPath(const QString&, const QString&) override { return std::nullopt; }

  QStringList promptNotes(const QString&) const override {
    return {"Shell commands run directly on the user's machine, not in a sandbox. Be careful with anything that changes things outside the "
            "working directory.",
            "Commands get a short list of environment variables (PATH, HOME, compiler and language settings, proxies), not the user's whole "
            "environment: tokens and keys aren't there. If a command needs one, ask the user; they can pass it with sandbox.env in the config."};
  }

  std::optional<QString> explainFailure(const QString&, const QString&) const override { return std::nullopt; }

  void dispose() override {}

private:
  QString m_reason;
  QStringList m_extraEnv;
};

class BwrapSandbox : public Sandbox {
public:
  BwrapSandbox(bool network, const std::optional<Limits>& limits, const HomeView& home, const QStringList& extraEnv)
      : m_network(network), m_extraEnv(extraEnv) {
    m_home.show = pathFoldersInHome() + home.show;
    m_home.writable = home.writable;
    for (const QString& dir : home.writable) {
      if (!QDir().mkpath(dir)) {
        m_warnings << QString("Can't create %1 for sandbox.home.writable.").arg(dir);
      }
    }
    for (const QString& dir : home.show) {
      if (!pathExists(dir)) {
        m_warnings << QString("%1 (sandbox.home.show) doesn't exist; it's skipped.").arg(dir);
      }
    }
    m_limiter = limiterFor(limits, m_warnings);
  }

  ~BwrapSandbox() override { dispose(); }

  SandboxType type() const override { return SandboxType::Bwrap; }
  bool active() const override { return true; }

  bool network() const override {
    QMutexLocker lock(&m_mutex);
    return m_network;
  }

  void setNetwork(bool on) override {
    QMutexLocker lock(&m_mutex);
    m_network = on;
  }

  QString describe(const QString&) const override {
    return QString("sandboxed: writes limited to the project, home folder hidden, network %1").arg(network() ? "on" : "off");
  }

  using Sandbox::spawnSpec;
  SpawnSpec spawnSpec(const QString& command, const QString& cwd, const CommandAccess& access) override {
    SpawnSpec spec = bwrap("bash", {"-c", command}, cwd, {}, network() || access.network);
    addVariables(spec.env, access.env);
    if (m_limiter.isEmpty()) {
      return spec;
    }
    // systemd-run finds your user's systemd through these; env -u in the limiter removes them again.
    for (const char* name : {"XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS"}) {
      if (qEnvironmentVariableIsSet(name)) {
        spec.env.insert(name, qEnvironmentVariable(name));
      }
    }
    QStringList args = m_limiter.mid(1);
    args << spec.file << spec.args;
    spec.file = m_limiter.first();
    spec.args = args;
    return spec;
  }

  SpawnSpec programSpec(const QString& file, const QStringList& args, const QString& project, const QString& chdir) override {
    return bwrap(file, args, project, chdir, false);
  }

  QByteArray readBytes(const QString& file, const QString& project) override {
    const ProcessResult result = runProcess(programSpec("cat", {"--", file}, project, {}));
    if (result.started && result.exitCode == 0) {
      return result.out;
    }
    const QString stderrText = result.started ? utf8(result.err) : result.startError;
    SmithError error = fileError(stderrText, file, "open");
    if (error.code() == "ENOENT" && hiddenHomePath(file, project)) {
      throw SmithError(error.message() + " (the home folder is hidden in the sandbox, apart from the project and the folders in sandbox.home.show)",
                       error.code());
    }
    throw error;
  }

  void writeBytes(const QString& file, const QByteArray& content, const QString& project) override {
    const QString script = R"SH(mkdir -p -- "$(dirname -- "$1")" && cat > "$1")SH";
    RunOptions options;
    options.input = content;
    const ProcessResult result = runProcess(programSpec("bash", {"-c", script, "bash", file}, project, {}), options);
    if (!result.started || result.exitCode != 0) {
      throw fileError(result.started ? utf8(result.err) : result.startError, file, "write");
    }
  }

  // /tmp and /var/tmp are this run's own (unless the path is the project or another folder mounted on top).
  std::optional<SandboxPath> sandboxPath(const QString& path, const QString& project) override {
    for (const QString& dir : QStringList{project} + readOnlyPaths()) {
      const QString real = realPath(dir);
      if (isWithin(dir, path) || (!real.isEmpty() && isWithin(real, path))) {
        return std::nullopt;
      }
    }
    for (const auto& [dir, name] : TEMP_DIRS) {
      if (!isWithin(dir, path)) {
        continue;
      }
      const QString root = privateTemp() + "/" + name;
      return SandboxPath{root + path.mid(dir.size()), root};
    }
    return std::nullopt;
  }

  QStringList promptNotes(const QString& project) const override {
    auto outside = [&](const QStringList& dirs) {
      QStringList out;
      for (const QString& dir : dirs) {
        if (!isWithin(dir, project)) {
          out << tildify(dir);
        }
      }
      return out;
    };
    const QStringList shown = outside(m_home.show);
    const QStringList writable = outside(m_home.writable);
    QStringList visible;
    if (!shown.isEmpty()) {
      visible << shown.join(", ") + " (read-only)";
    }
    if (!writable.isEmpty()) {
      visible << writable.join(", ") + " (writable, and kept)";
    }
    return {
        "Shell commands and the file tools run in a sandbox (bubblewrap). Only the working directory, /tmp and the home folder (below) are "
        "writable; /tmp is private to this session and kept between commands. Everything else is read-only.",
        QString("The home folder is a private, empty one: other projects and dotfiles aren't there%1. Tools can write caches to it (~/.cache "
                "and the like); they last for the session.")
            .arg(visible.isEmpty() ? QString() : ", apart from " + visible.join(" and ")),
        "System packages can't be installed. Project-local installs (node_modules, a virtualenv in the working directory) work when the network "
        "is on.",
        "The network is off unless the user turns it on (/network on). The .git folder is read-only: don't commit, the user does that.",
        "While the network is off, every command has a network of its own: a server started with run_in_background can't be reached from other "
        "commands (curl localhost fails). Background jobs suit long builds and watchers; for a server you need to talk to, ask the user to turn "
        "the network on.",
    };
  }

  std::optional<QString> explainFailure(const QString& output, const QString& project) const override {
    if (READ_ONLY.match(output).hasMatch()) {
      return QString("[Sandbox: only the working directory, /tmp and the private home folder are writable; the path above is outside them. "
                     "Write inside the working directory or /tmp instead (e.g. a build or cache folder there). If a tool insists on a folder "
                     "in home, the user can share it with sandbox.home.writable in the config.]");
    }
    if (const auto note = networkNote(output, network())) {
      return note;
    }
    // A file in the hidden part of home that a command expected to find.
    const QString home = homeDir();
    static const QRegularExpression tildeRef(R"((^|[\s'"])~/)");
    static const QRegularExpression pathRef(R"((?:~|/)[^\s'":]+)");
    for (const QString& line : output.split('\n')) {
      if (!line.contains("No such file or directory") || !(line.contains(home + "/") || tildeRef.match(line).hasMatch())) {
        continue;
      }
      auto it = pathRef.globalMatch(line);
      while (it.hasNext()) {
        QString path = it.next().captured(0);
        if (path.startsWith("~")) {
          path = home + path.mid(1);
        }
        if (hiddenHomePath(path, project)) {
          return QString("[Sandbox: the home folder is hidden apart from the working directory and a few shown folders, so that file may exist "
                         "but isn't visible here. Ask the user if you need it; they can show folders with sandbox.home.show.]");
        }
      }
      break;
    }
    return std::nullopt;
  }

  void dispose() override {
    QMutexLocker lock(&m_mutex);
    if (!m_tempRoot.isEmpty()) {
      removeTree(m_tempRoot);
    }
    m_tempRoot.clear();
  }

private:
  SpawnSpec bwrap(const QString& file, const QStringList& args, const QString& project, const QString& chdir, bool network) {
    QProcessEnvironment env = sandboxEnv(ENV_ALLOW, m_extraEnv);
    env.insert("TMPDIR", "/tmp");
    const QString root = privateTemp();
    BwrapOptions options;
    options.network = network;
    options.readOnlyPaths = readOnlyPaths();
    options.tempRoot = root;
    options.home = m_home;
    options.privateHome = root + "/home";
    options.chdir = chdir;
    QStringList all = bwrapArgs(project, options);
    all << "--" << file << args;
    return SpawnSpec{"bwrap", all, env, {}, {}};
  }

  // Whether a path is in the home folder but not in anything shown there.
  bool hiddenHomePath(const QString& file, const QString& project) const {
    const QString home = homeDir();
    if (!isWithin(home, file)) {
      return false;
    }
    for (const QString& dir : QStringList{project} + readOnlyPaths() + m_home.show + m_home.writable) {
      if (isWithin(dir, file)) {
        return false;
      }
    }
    return true;
  }

  // The host folder behind the sandbox's /tmp, /var/tmp and home: private to this run, removed at exit.
  QString privateTemp() {
    QMutexLocker lock(&m_mutex);
    if (!m_tempRoot.isEmpty()) {
      return m_tempRoot;
    }
    QByteArray pattern = QFile::encodeName(QDir::tempPath() + "/smith-XXXXXX");
    if (::mkdtemp(pattern.data()) == nullptr) {
      throw SmithError("Could not create the sandbox's private temp folder.");
    }
    m_tempRoot = QFile::decodeName(pattern);
    for (const QString& name : {QString("tmp"), QString("var-tmp"), QString("home")}) {
      QDir().mkdir(m_tempRoot + "/" + name);
      QFile::setPermissions(m_tempRoot + "/" + name, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    return m_tempRoot;
  }

  bool m_network;
  QStringList m_extraEnv;
  HomeView m_home;
  QStringList m_limiter;
  QString m_tempRoot;
};

} // namespace

void addVariables(QProcessEnvironment& env, const QStringList& names) {
  for (const QString& name : names) {
    if (qEnvironmentVariableIsSet(name.toUtf8().constData())) {
      env.insert(name, qEnvironmentVariable(name.toUtf8().constData()));
    }
  }
}

const QRegularExpression PROXY_VARS("^(HTTP_PROXY|HTTPS_PROXY|NO_PROXY|ALL_PROXY|http_proxy|https_proxy|no_proxy|all_proxy)$");

QString Sandbox::readFile(const QString& file, const QString& project) {
  return utf8(readBytes(file, project));
}

void Sandbox::writeFile(const QString& file, const QString& content, const QString& project) {
  writeBytes(file, content.toUtf8(), project);
}

QStringList Sandbox::readOnlyPaths() const {
  QMutexLocker lock(&m_mutex);
  return m_readOnlyPaths;
}

void Sandbox::setReadOnlyPaths(const QStringList& paths) {
  QMutexLocker lock(&m_mutex);
  m_readOnlyPaths = paths;
}

std::shared_ptr<Sandbox> Sandbox::none(const QString& reason) {
  return std::make_shared<NoSandbox>(reason, QStringList());
}

std::shared_ptr<Sandbox> Sandbox::create(const SandboxOptions& options) {
  if (options.type == SandboxType::Off) {
    return std::make_shared<NoSandbox>("disabled with --sandbox off", options.env);
  }
  if (options.type == SandboxType::Container) {
    return std::make_shared<ContainerSandbox>(options.container, options.network, options.limits, options.env);
  }
  const QString problem = probeBwrap();
  if (!problem.isEmpty()) {
    throw SmithError(QString("Shell commands run in a sandbox, but bubblewrap can't run: %1\n"
                             "On RHEL/Fedora: sudo dnf install bubblewrap\n"
                             "To run without a sandbox anyway (e.g. inside a container that already isolates it), pass --sandbox off.")
                         .arg(problem));
  }
  return std::make_shared<BwrapSandbox>(options.network, options.limits, options.home, options.env);
}

QProcessEnvironment hostEnv() {
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  for (const QString& name : AGENT_SECRETS) {
    env.remove(name);
  }
  return env;
}

QProcessEnvironment sandboxEnv(const QRegularExpression& allowed, const QStringList& extra) {
  const QProcessEnvironment system = QProcessEnvironment::systemEnvironment();
  QProcessEnvironment env;
  for (const QString& name : system.keys()) {
    if (allowed.match(name).hasMatch()) {
      env.insert(name, system.value(name));
    }
  }
  for (const QString& entry : extra) {
    const int eq = entry.indexOf('=');
    if (eq > 0) {
      env.insert(entry.left(eq), entry.mid(eq + 1));
    } else if (system.contains(entry)) {
      env.insert(entry, system.value(entry));
    }
  }
  for (const QString& name : AGENT_SECRETS) {
    env.remove(name);
  }
  return env;
}

bool isHiddenFromAgent(const QString& file) {
  const QString home = homeDir();
  QStringList roots = HIDDEN_SYSTEM_DIRS + HIDDEN_FROM_FILE_TOOLS;
  for (const QString& rel : HIDDEN_HOME_PATHS) {
    roots << home + "/" + rel;
  }
  for (const QString& root : roots) {
    if (file == root || file.startsWith(root + "/")) {
      return true;
    }
  }
  return false;
}

SmithError fileError(const QString& stderrText, const QString& file, const QString& op) {
  struct Known {
    QRegularExpression pattern;
    QString code;
    QString message;
  };
  const QList<Known> known = {
      {QRegularExpression("No such file or directory"), "ENOENT", QString("ENOENT: no such file or directory, %1 '%2'").arg(op, file)},
      {QRegularExpression("Is a directory"), "EISDIR", QString("EISDIR: illegal operation on a directory, %1 '%2'").arg(op, file)},
      {QRegularExpression("Not a directory"), "ENOTDIR", QString("ENOTDIR: not a directory, %1 '%2'").arg(op, file)},
      {QRegularExpression("Permission denied"), "EACCES", QString("EACCES: permission denied, %1 '%2'").arg(op, file)},
      {QRegularExpression("Read-only file system"), "EROFS",
       QString("Can't write %1: the sandbox only allows writes inside the project (and its own private /tmp and home folder).").arg(file)},
      {QRegularExpression("[Nn]o such container|is not running"), "ESRCH", "The container stopped; run the command again to start a new one."},
  };
  for (const Known& entry : known) {
    if (entry.pattern.match(stderrText).hasMatch()) {
      return SmithError(entry.message, entry.code);
    }
  }
  const QString trimmed = stderrText.trimmed();
  return SmithError(trimmed.isEmpty() ? QString("Could not %1 %2").arg(op == "open" ? "read" : "write", file) : trimmed);
}

void removeTree(const QString& dir) {
  SpawnSpec spec{"chmod", {"-R", "u+w", dir}, hostEnv(), {}, {}};
  runProcess(spec);
  QDir(dir).removeRecursively();
}

QStringList bwrapArgs(const QString& cwdGiven, const BwrapOptions& options) {
  // bwrap mounts at paths as given and can't create mount points through
  // symlinks, so every path is resolved to where it really is first.
  const QString realCwd = realPath(cwdGiven);
  const QString cwd = realCwd.isEmpty() ? cwdGiven : realCwd;
  QStringList args{"--unshare-all"};
  if (options.network) {
    args << "--share-net";
  }
  args << "--die-with-parent" << "--new-session";

  // Mounts apply in order, each on top of the previous ones.
  args << "--ro-bind" << "/" << "/" << "--dev" << "/dev" << "--proc" << "/proc";
  for (const QString& dir : HIDDEN_SYSTEM_DIRS) {
    if (!isDirectory(dir)) {
      continue;
    }
    QString temp;
    for (const auto& [path, name] : TEMP_DIRS) {
      if (path == dir) {
        temp = name;
      }
    }
    if (!temp.isEmpty() && !options.tempRoot.isEmpty()) {
      args << "--bind" << options.tempRoot + "/" + temp << dir;
    } else {
      args << "--tmpfs" << dir;
    }
  }

  // The home folder: a private, empty one, with only the chosen folders shown.
  const QString home = homeDir();
  if (options.home) {
    const QString realHome = realPath(home);
    args << "--bind" << options.privateHome << (realHome.isEmpty() ? home : realHome);
    for (const QString& dir : options.home->show) {
      const QString real = realPath(dir);
      if (!real.isEmpty()) {
        args << "--ro-bind" << real << real;
      }
    }
    for (const QString& dir : options.home->writable) {
      const QString real = realPath(dir);
      if (!real.isEmpty()) {
        args << "--bind" << real << real;
      }
    }
  }

  // Read-only folders around the project (the repository it's a subfolder
  // of) go first, so the project stays writable on top of them.
  QStringList readOnly;
  for (const QString& dir : options.readOnlyPaths) {
    const QString real = realPath(dir);
    if (!real.isEmpty() && isDirectory(real)) {
      readOnly << real;
    }
  }
  auto around = [&](const QString& dir) { return cwd == dir || cwd.startsWith(dir + "/"); };
  for (const QString& dir : readOnly) {
    if (around(dir)) {
      args << "--ro-bind" << dir << dir;
    }
  }

  // The one real writable place (with home.writable). Bound after the system
  // masks so a project under /tmp or /mnt (or home) stays visible, and before
  // the home masks so a project at the home directory doesn't re-expose ~/.ssh.
  args << "--bind" << cwd << cwd;

  for (const QString& rel : HIDDEN_HOME_PATHS) {
    hide(args, home + "/" + rel);
  }

  // Others (skill folders) after the masks, so a skill in a hidden ~/.claude still runs.
  for (const QString& dir : readOnly) {
    if (!around(dir)) {
      args << "--ro-bind" << dir << dir;
    }
  }

  const QString git = cwd + "/.git";
  if (pathExists(git)) {
    args << "--ro-bind" << git << git;
  }

  // With the network on, DNS must still work when resolv.conf points into a
  // hidden directory (systemd-resolved under /run, WSL under /mnt).
  if (options.network) {
    const QString resolv = realPath("/etc/resolv.conf");
    if (!resolv.isEmpty() && resolv != "/etc/resolv.conf") {
      args << "--ro-bind" << resolv << resolv;
    }
  }

  args << "--chdir" << (options.chdir.isEmpty() ? cwd : options.chdir);
  return args;
}

} // namespace Smith
