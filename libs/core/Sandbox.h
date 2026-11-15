#pragma once

#include "core/Errors.h"
#include "core/Process.h"

#include <QMutex>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>

namespace Smith {

enum class SandboxType { Bwrap, Container, Off };

QString sandboxTypeName(SandboxType type);

// Caps for each shell command; unset means no cap.
struct Limits {
  std::optional<qint64> memory;
  std::optional<int> processes;
  std::optional<double> cpus;

  bool any() const { return memory || processes || cpus; }
};

struct ContainerProject {
  QString folder;
  QString image;
  QStringList mounts;
};

struct ContainerSettings {
  QString runtime = "podman";
  // The image when no `projects` entry matches.
  QString image;
  // Host folders or files to mount: "src", "src:ro", "src:dst", "src:dst:ro" (~ expanded).
  QStringList mounts;
  // Per-folder image and extra mounts; the most specific folder containing the project wins.
  QList<ContainerProject> projects;
};

struct HomeView {
  QStringList show;
  QStringList writable;
};

struct SandboxOptions {
  SandboxType type = SandboxType::Bwrap;
  bool network = false;
  // More variables for commands, with or without a sandbox: "NAME" passes it through, "NAME=value" sets it.
  QStringList env;
  std::optional<Limits> limits;
  HomeView home;
  ContainerSettings container;
};

// For a path outside the project that belongs to the sandbox itself (its
// private /tmp, a container's own files): where it really is on this machine
// (`host`, inside `root`), or both empty if it has no place here at all.
struct SandboxPath {
  QString host;
  QString root;
};

// Where shell commands run, and where the file tools read and write: in
// bubblewrap, in a container, or (when asked for) directly. Used from
// several threads at once; implementations lock what they change.
class Sandbox {
public:
  virtual ~Sandbox() = default;

  virtual SandboxType type() const = 0;
  // Whether commands are isolated at all.
  virtual bool active() const = 0;
  // Why it isn't active, when it isn't.
  virtual QString reason() const { return {}; }
  // Whether commands can use the network. Can be changed at runtime.
  virtual bool network() const = 0;
  virtual void setNetwork(bool on) = 0;

  // Extra folders made visible read-only: skill folders, the repository a submodule is in.
  QStringList readOnlyPaths() const;
  void setReadOnlyPaths(const QStringList& paths);
  // Problems with the settings worth telling the user about at startup.
  QStringList warnings() const { return m_warnings; }

  // One line for the banner and approval prompts.
  virtual QString describe(const QString& project = {}) const = 0;
  // How to run `command` with bash in `cwd` (the project).
  virtual SpawnSpec spawnSpec(const QString& command, const QString& cwd) = 0;
  // How to run a program directly (no shell), starting in `chdir` (default: the project). No network.
  virtual SpawnSpec programSpec(const QString& file, const QStringList& args, const QString& project, const QString& chdir = {}) = 0;
  // Reads a file as commands see it. Throws SmithError with a code (ENOENT, EISDIR, ...).
  virtual QString readFile(const QString& file, const QString& project) = 0;
  // Writes a file, creating its folder; only where commands could write it.
  virtual void writeFile(const QString& file, const QString& content, const QString& project) = 0;
  // See SandboxPath; nullopt for ordinary paths.
  virtual std::optional<SandboxPath> sandboxPath(const QString& path, const QString& project) = 0;
  // What the model should know about where its commands run, for the system prompt.
  virtual QStringList promptNotes(const QString& project) const = 0;
  // A note for the model when a failed command's output looks like the sandbox's doing.
  virtual std::optional<QString> explainFailure(const QString& output, const QString& project) const = 0;
  // Stops and removes what it started (containers, temp folders); also done at exit.
  virtual void dispose() = 0;

  // No sandbox: commands run directly with bash, with the same short list of environment variables as in one.
  static std::shared_ptr<Sandbox> none(const QString& reason = "sandbox disabled");
  // bwrap requires bubblewrap and container needs podman or docker: throws if they can't run.
  static std::shared_ptr<Sandbox> create(const SandboxOptions& options);

protected:
  mutable QMutex m_mutex;
  QStringList m_readOnlyPaths;
  QStringList m_warnings;
};

// The environment for programs Smith itself runs (git, bwrap, the container runtime): everything but Smith's own keys.
QProcessEnvironment hostEnv();
// The variables for sandboxed commands: those matching `allowed`, then `extra` from the config.
QProcessEnvironment sandboxEnv(const QRegularExpression& allowed, const QStringList& extra);
extern const QRegularExpression PROXY_VARS;

// Whether a path is off limits to the agent's file tools.
bool isHiddenFromAgent(const QString& file);

// A SmithError (with a code) from what cat, mkdir or bash printed.
SmithError fileError(const QString& stderrText, const QString& file, const QString& op);

// Deletes a folder even when it holds read-only directories.
void removeTree(const QString& dir);

struct BwrapOptions {
  bool network = false;
  QStringList readOnlyPaths;
  // Host folder whose tmp/ and var-tmp/ become /tmp and /var/tmp.
  QString tempRoot;
  // Where the command starts; the project by default.
  QString chdir;
  // Hides the home folder behind `privateHome`, then shows `show` read-only and `writable` read-write on top.
  std::optional<HomeView> home;
  QString privateHome;
};

QStringList bwrapArgs(const QString& cwd, const BwrapOptions& options);

} // namespace Smith
