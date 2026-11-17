#include "core/ContainerSandbox.h"
#include "compat/Utf8.h"

#include "core/Dirs.h"
#include "core/Errors.h"
#include "core/Tool.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QProcess>
#include <QRandomGenerator>

#include <algorithm>
#include <unistd.h>

namespace Smith {

namespace {

QString randomHex(int bytes) {
  QString out;
  for (int i = 0; i < bytes; i++) {
    out += QString("%1").arg(QRandomGenerator::global()->bounded(256), 2, 16, QChar('0'));
  }
  return out;
}

bool dockerIsRootless() {
  SpawnSpec spec{"docker", {"info", "--format", "{{.SecurityOptions}}"}, hostEnv(), {}, {}};
  RunOptions options;
  options.timeoutMs = 10000;
  const ProcessResult result = runProcess(spec, options);
  return utf8(result.out).contains("rootless");
}

} // namespace

Mount parseMount(const QString& spec) {
  QStringList parts = spec.split(':');
  bool readOnly = false;
  if (!parts.isEmpty() && (parts.last() == "ro" || parts.last() == "rw")) {
    readOnly = parts.takeLast() == "ro";
  }
  const QString source = expandHome(parts.value(0));
  const QString target = parts.size() > 1 && !parts[1].isEmpty() ? expandHome(parts[1]) : source;
  return Mount{QDir::cleanPath(QDir::current().absoluteFilePath(source)), QDir::cleanPath(QDir::current().absoluteFilePath(target)), readOnly};
}

ContainerSandbox::ContainerSandbox(const ContainerSettings& settings, bool network, const std::optional<Limits>& limits, const QStringList& extraEnv)
    : m_settings(settings), m_limits(limits), m_extraEnv(extraEnv), m_network(network) {
  SpawnSpec probe{settings.runtime, {"--version"}, hostEnv(), {}, {}};
  RunOptions options;
  options.timeoutMs = 10000;
  const ProcessResult result = runProcess(probe, options);
  if (!result.started || result.exitCode != 0) {
    const QString why = result.started ? utf8(result.err).trimmed() : result.startError;
    throw SmithError(QString("The sandbox is set to use containers, but %1 can't run: %2\n"
                             "On RHEL: sudo dnf install podman. Or set sandbox.type to \"bwrap\" in the config file.")
                         .arg(settings.runtime, why));
  }
  m_rootMapsToYou = settings.runtime == "podman" ? ::getuid() != 0 : dockerIsRootless();
  if (!m_rootMapsToYou) {
    m_warnings << QString("%1 runs as root here, so commands in the container run as your user (to keep files in the project yours) and can't "
                          "install system packages.")
                      .arg(settings.runtime);
  }
  QStringList all = settings.mounts;
  for (const ContainerProject& project : settings.projects) {
    all << project.mounts;
  }
  for (const QString& spec : all) {
    const Mount mount = parseMount(spec);
    if (!pathExists(mount.source)) {
      m_warnings << QString("The sandbox mount %1 doesn't exist; it's skipped.").arg(mount.source);
    }
  }
}

ContainerSandbox::~ContainerSandbox() {
  dispose();
}

bool ContainerSandbox::network() const {
  QMutexLocker lock(&m_mutex);
  return m_network;
}

void ContainerSandbox::setNetwork(bool on) {
  {
    QMutexLocker lock(&m_mutex);
    if (on == m_network) {
      return;
    }
    m_network = on;
  }
  dispose();
}

QString ContainerSandbox::describe(const QString& project) const {
  const ProjectSettings settings = project.isEmpty() ? ProjectSettings{m_settings.image, m_settings.mounts} : settingsFor(project);
  const QString extra = settings.mounts.isEmpty() ? QString() : QString(" and %1").arg(plural(settings.mounts.size(), "mount"));
  return QString("in a %1 container (%2): only the project%3, network %4").arg(m_settings.runtime, settings.image, extra, network() ? "on" : "off");
}

SpawnSpec ContainerSandbox::spawnSpec(const QString& command, const QString& cwd, const CommandAccess& access) {
  if (access.network && !network()) {
    // The container was started without a network, and one command can't be given one.
    throw SmithError("This tool needs the network, and in the container sandbox that means turning it on for every command (/network on). "
                     "The bwrap sandbox can give it to this tool alone.");
  }
  const QString name = container(cwd);
  // The command runs as a background job with job control on, so it gets a
  // process group of its own; its id goes to a file, so a timeout or
  // interrupt can kill the whole group inside the container.
  const QString pidFile = QString("/tmp/.smith-%1.pid").arg(randomHex(6));
  const QString wrapper = R"SH(set -m; bash -c "$1" & p=$!; set +m; echo $p > "$2"; wait $p; s=$?; rm -f "$2"; exit $s)SH";
  const QString runtime = m_settings.runtime;
  QStringList args{"exec", "-w", cwd};
  args << envArgs();
  QProcessEnvironment extra;
  addVariables(extra, access.env);
  for (const QString& variable : extra.keys()) {
    args << "-e" << QString("%1=%2").arg(variable, extra.value(variable));
  }
  args << name << "bash" << "-c" << wrapper << "bash" << command << pidFile;
  SpawnSpec spec{runtime, args, hostEnv(), {}, {}};
  spec.kill = [runtime, name, pidFile]() {
    const QString stop = QString(R"SH(kill -KILL -- -"$(cat %1 2>/dev/null)" 2>/dev/null; rm -f %1)SH").arg(pidFile);
    QProcess::startDetached(runtime, {"exec", name, "bash", "-c", stop});
  };
  return spec;
}

SpawnSpec ContainerSandbox::programSpec(const QString& file, const QStringList& args, const QString& project, const QString& chdir) {
  const QString name = container(project);
  QStringList all{"exec", "-w", chdir.isEmpty() ? project : chdir, name, file};
  all << args;
  return SpawnSpec{m_settings.runtime, all, hostEnv(), {}, {}};
}

QByteArray ContainerSandbox::readBytes(const QString& file, const QString& project) {
  if (isWithin(project, file)) {
    return Sandbox::none()->readBytes(file, project);
  }
  const ProcessResult result = runProcess(programSpec("cat", {"--", file}, project));
  if (!result.started || result.exitCode != 0) {
    throw fileError(result.started ? utf8(result.err) : result.startError, file, "open");
  }
  return result.out;
}

void ContainerSandbox::writeBytes(const QString& file, const QByteArray& content, const QString& project) {
  if (isWithin(project, file)) {
    Sandbox::none()->writeBytes(file, content, project);
    return;
  }
  const QString name = container(project);
  const QString script = R"SH(mkdir -p -- "$(dirname -- "$1")" && cat > "$1")SH";
  SpawnSpec spec{m_settings.runtime, {"exec", "-i", name, "bash", "-c", script, "bash", file}, hostEnv(), {}, {}};
  RunOptions options;
  options.input = content;
  const ProcessResult result = runProcess(spec, options);
  if (!result.started || result.exitCode != 0) {
    throw fileError(result.started ? utf8(result.err) : result.startError, file, "write");
  }
}

QStringList ContainerSandbox::envArgs() const {
  static const QRegularExpression nothing("^$");
  const QProcessEnvironment env = sandboxEnv(network() ? PROXY_VARS : nothing, m_extraEnv);
  QStringList args;
  for (const QString& name : env.keys()) {
    args << "-e" << QString("%1=%2").arg(name, env.value(name));
  }
  return args;
}

QStringList ContainerSandbox::promptNotes(const QString& project) const {
  const ProjectSettings settings = settingsFor(project);
  QStringList shared;
  for (const QString& spec : settings.mounts) {
    const Mount mount = parseMount(spec);
    if (pathExists(mount.source)) {
      shared << mount.target + (mount.readOnly ? " (read-only)" : "");
    }
  }
  const QString who = m_rootMapsToYou
                          ? "as root, so you can install what a build needs with the image's package manager (dnf, microdnf, apt) or pip/npm; it "
                            "changes only the container"
                          : "as the user's own account, so system packages can't be installed; project-local installs (node_modules, a virtualenv "
                            "in the working directory) work";
  return {
      QString("Shell commands run in a %1 container from the image %2, %3. The container lasts for this session: /tmp and anything installed "
              "stay until the user quits.")
          .arg(m_settings.runtime, settings.image, who),
      QString("Of the user's machine, only the working directory (same path, read-write)%1 are visible. Other paths are the container's own "
              "files: read_file, grep and list_files read them from the container.")
          .arg(shared.isEmpty() ? QString() : " and " + shared.join(", ")),
      "The network is off unless the user turns it on (/network on), which replaces the container with a new one (stopping background jobs). "
      "The .git folder is read-only: don't commit, the user does that.",
      "Background jobs (run_in_background) run in the same container, so a server started that way is reachable at localhost from later "
      "commands.",
  };
}

std::optional<QString> ContainerSandbox::explainFailure(const QString& output, const QString&) const {
  static const QRegularExpression readOnly("Read-only file system|EROFS");
  static const QRegularExpression needsRoot("Permission denied|must be root|requires root|You need to be root|are you root",
                                            QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression noNetwork(
      "Could not resolve host|Temporary failure in name resolution|Name or service not known|getaddrinfo (ENOTFOUND|EAI_AGAIN)|"
      "Network is unreachable|ENETUNREACH|Failed to establish a new connection|Could not resolve|no such host|Cannot download|Failed to connect to",
      QRegularExpression::CaseInsensitiveOption);
  if (readOnly.match(output).hasMatch()) {
    return QString("[Sandbox: the path above is read-only in the container (the .git folder, or a read-only mount). Write inside the working "
                   "directory or the container's own folders instead.]");
  }
  if (!m_rootMapsToYou && needsRoot.match(output).hasMatch()) {
    return QString("[Sandbox: commands run as the user's own account in this container (the container runtime runs as root here), so "
                   "installing system packages isn't possible. Install into the working directory instead, or tell the user what's missing.]");
  }
  if (!network() && noNetwork.match(output).hasMatch()) {
    return QString("[Sandbox: the network is off, so this couldn't download or connect. Don't work around it; tell the user they can turn it "
                   "on with /network on (which starts a new container), or carry on without it.]");
  }
  return std::nullopt;
}

std::optional<SandboxPath> ContainerSandbox::sandboxPath(const QString& path, const QString& project) {
  if (isWithin(project, path)) {
    return std::nullopt;
  }
  return SandboxPath{};
}

void ContainerSandbox::dispose() {
  QStringList names;
  {
    QMutexLocker lock(&m_mutex);
    names = QStringList(m_started.begin(), m_started.end());
    m_containers.clear();
    m_started.clear();
  }
  if (!names.isEmpty()) {
    SpawnSpec spec{m_settings.runtime, QStringList{"rm", "-f"} + names, hostEnv(), {}, {}};
    RunOptions options;
    options.timeoutMs = 30000;
    runProcess(spec, options);
  }
}

ContainerSandbox::ProjectSettings ContainerSandbox::settingsFor(const QString& project) const {
  const ContainerProject* best = nullptr;
  for (const ContainerProject& candidate : m_settings.projects) {
    if (isWithin(candidate.folder, project) && (!best || candidate.folder.size() > best->folder.size())) {
      best = &candidate;
    }
  }
  ProjectSettings settings{m_settings.image, m_settings.mounts};
  if (best) {
    if (!best->image.isEmpty()) {
      settings.image = best->image;
    }
    settings.mounts << best->mounts;
  }
  return settings;
}

QString ContainerSandbox::container(const QString& project) {
  // One start at a time; a failed start is retried by the next command.
  QMutexLocker startLock(&m_startMutex);
  {
    QMutexLocker lock(&m_mutex);
    if (m_containers.contains(project)) {
      return m_containers.value(project);
    }
  }
  const QString name = start(project);
  QMutexLocker lock(&m_mutex);
  m_containers.insert(project, name);
  m_started.insert(name);
  return name;
}

QString ContainerSandbox::start(const QString& project) {
  const QString runtime = m_settings.runtime;
  const ProjectSettings settings = settingsFor(project);
  const QString name = QString("smith-%1").arg(randomHex(4));
  const QString home = homeDir();

  QList<Mount> volumes{{project, project, false}};
  const QString git = project + "/.git";
  if (pathExists(git)) {
    volumes.append({git, git, true});
  }
  for (const QString& dir : readOnlyPaths()) {
    if (pathExists(dir)) {
      volumes.append({dir, dir, true});
    }
  }
  for (const QString& spec : settings.mounts) {
    const Mount mount = parseMount(spec);
    if (pathExists(mount.source)) {
      volumes.append(mount);
    }
  }
  // Relabelling the home folder or / for containers would upset everything else using them.
  const bool noRelabel = std::any_of(volumes.begin(), volumes.end(), [&](const Mount& v) {
    return !v.readOnly && (v.source == home || v.source == "/" || v.source == QFileInfo(home).path());
  });

  QStringList args{"run", "-d", "--rm", "--init", "--name", name, "--label", "smith=1"};
  if (m_rootMapsToYou) {
    args << "--user" << "0:0";
  } else {
    args << "--user" << QString("%1:%2").arg(::getuid()).arg(::getgid()) << "--env" << "HOME=/tmp";
  }
  if (!network()) {
    args << "--network" << "none";
  }
  args << "--security-opt" << "no-new-privileges";
  if (noRelabel) {
    args << "--security-opt" << "label=disable";
  }
  for (const Mount& v : volumes) {
    QStringList options;
    if (v.readOnly) {
      options << "ro";
    }
    if (!noRelabel && v.source != git) {
      options << "z";
    }
    args << "--volume" << QString("%1:%2%3").arg(v.source, v.target, options.isEmpty() ? QString() : ":" + options.join(","));
  }
  if (m_limits && m_limits->memory) {
    args << "--memory" << QString::number(*m_limits->memory);
  }
  if (m_limits && m_limits->processes) {
    args << "--pids-limit" << QString::number(*m_limits->processes);
  }
  if (m_limits && m_limits->cpus) {
    args << "--cpus" << QString::number(*m_limits->cpus);
  }
  args << "--workdir" << project << "--entrypoint" << "sleep" << settings.image << "infinity";

  const ProcessResult result = runProcess(SpawnSpec{runtime, args, hostEnv(), {}, {}});
  if (!result.started || result.exitCode != 0) {
    QString why = result.started ? utf8(result.err).trimmed() : result.startError;
    if (why.isEmpty()) {
      why = QString("exit code %1").arg(result.exitCode);
    }
    throw SmithError(QString("Could not start a %1 container from %2: %3").arg(runtime, settings.image, why));
  }
  return name;
}

} // namespace Smith
