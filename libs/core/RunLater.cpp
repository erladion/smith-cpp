#include "core/RunLater.h"
#include "compat/Utf8.h"

#include "core/Process.h"
#include "core/Sandbox.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace Smith {

namespace {

struct Rule {
  QRegularExpression pattern;
  QString why;
};

const QList<Rule>& rules() {
  static const QList<Rule> list = {
      {QRegularExpression(R"((^|/)\.envrc$)"), "direnv runs it when you enter the folder"},
      {QRegularExpression(R"((^|/)(\.vscode/|[^/]*\.code-workspace$))"), "VS Code reads it when you open the folder, and can run tasks from it"},
      {QRegularExpression(R"((^|/)\.idea/)"), "JetBrains IDEs read it when you open the project"},
      {QRegularExpression(R"((^|/)(\.nvim\.lua|\.nvimrc|\.exrc|\.dir-locals\.el)$)"), "your editor can run it when you open files here"},
      {QRegularExpression(R"((^|/)(\.husky|\.githooks)/)"), "git hooks run when you commit"},
      {QRegularExpression(R"((^|/)(\.pre-commit-config\.ya?ml|\.?lefthook\.ya?ml|\.lintstagedrc[^/]*|lint-staged\.config\.[^/]+)$)"),
       "it runs when you commit"},
      {QRegularExpression(R"((^|/)\.gitmodules$)"), "git fetches what it points to"},
      {QRegularExpression(R"((^|/)\.git(/|$))"), "git runs its hooks and what its config names whenever git runs there: a shell prompt, an editor, a commit"},
      {QRegularExpression(
           R"((^|/)(\.github/workflows/|\.gitlab-ci\.ya?ml$|Jenkinsfile$|\.circleci/|azure-pipelines\.ya?ml$|\.travis\.ya?ml$|bitbucket-pipelines\.ya?ml$))"),
       "CI runs it when you push, often with secrets"},
      {QRegularExpression(R"((^|/)(package\.json|\.npmrc|\.yarnrc(\.ya?ml)?|\.pnpmfile\.c?js|bunfig\.toml)$)"),
       "npm, yarn or pnpm run scripts from it when you install or build"},
      {QRegularExpression(R"((^|/)(setup\.py|setup\.cfg|pyproject\.toml|requirements[^/]*\.txt|Pipfile)$)"),
       "pip runs or installs what it says when you install"},
      {QRegularExpression(R"((^|/)(Makefile|makefile|GNUmakefile|[^/]+\.mk|CMakeLists\.txt|[^/]+\.cmake|meson\.build|configure(\.ac)?)$)"),
       "it runs when you build"},
      {QRegularExpression(
           R"((^|/)(build\.gradle(\.kts)?|settings\.gradle(\.kts)?|gradle-wrapper\.properties|gradlew|mvnw|pom\.xml|build\.rs|Cargo\.toml)$|(^|/)\.mvn/)"),
       "it runs when you build"},
      {QRegularExpression(R"((^|/)(Dockerfile|Containerfile|[^/]+\.Dockerfile|(docker-)?compose[^/]*\.ya?ml)$|(^|/)\.devcontainer/)"),
       "it runs when you build or start containers, and can mount anything"},
  };
  return list;
}

// Folders never worth looking in.
const QSet<QString> SKIP_DIRS = {".git", "node_modules", "dist", "build", ".venv", "venv", "__pycache__", "target"};
constexpr int MAX_FILES = 50000;
constexpr int MAX_HASHED = 2000;
constexpr int MAX_FOLDERS = 50000;

QString hashFile(const QString& file) {
  QFile handle(file);
  if (!handle.open(QIODevice::ReadOnly)) {
    return {};
  }
  QCryptographicHash hash(QCryptographicHash::Sha1);
  hash.addData(&handle);
  return QString::fromLatin1(hash.result().toHex());
}

// The project's files, relative: what git knows of (tracked and untracked, not
// ignored) in a repository, else a walk that skips the usual folders.
QStringList projectFiles(const QString& cwd) {
  // fsmonitor off: git must not run anything from the repository's config here.
  SpawnSpec spec{"git", {"-c", "core.fsmonitor=false", "ls-files", "-z", "--cached", "--others", "--exclude-standard"}, hostEnv(), cwd, {}};
  RunOptions options;
  options.timeoutMs = 10000;
  options.maxCapture = 64 * 1024 * 1024;
  const ProcessResult git = runProcess(spec, options);
  if (git.started && git.exitCode == 0) {
    QStringList files;
    for (const QByteArray& name : git.out.split('\0')) {
      if (!name.isEmpty()) {
        files << utf8(name);
      }
      if (files.size() >= MAX_FILES) {
        break;
      }
    }
    return files;
  }

  QStringList files;
  QStringList stack{""};
  while (!stack.isEmpty() && files.size() < MAX_FILES) {
    const QString dir = stack.takeLast();
    const QFileInfoList entries = QDir(dir.isEmpty() ? cwd : cwd + "/" + dir).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot | QDir::System);
    for (const QFileInfo& entry : entries) {
      const QString rel = dir.isEmpty() ? entry.fileName() : dir + "/" + entry.fileName();
      if (entry.isDir() && !entry.isSymLink()) {
        if (!SKIP_DIRS.contains(entry.fileName())) {
          stack << rel;
        }
      } else {
        files << rel;
      }
    }
  }
  return files;
}

// Every git folder in the project (".git", "vendor/lib/.git"), relative. A
// sandboxed command can make one anywhere it can write, with hooks and config
// of its own; git doesn't list them, so this walks the folders.
QStringList gitFolders(const QString& cwd) {
  QStringList found;
  QStringList stack{""};
  int seen = 0;
  while (!stack.isEmpty() && seen < MAX_FOLDERS) {
    const QString dir = stack.takeLast();
    const QString abs = dir.isEmpty() ? cwd : cwd + "/" + dir;
    const QFileInfo git(abs + "/.git");
    if (git.exists() || git.isSymLink()) {
      found << (dir.isEmpty() ? QString(".git") : dir + "/.git");
    }
    for (const QString& name : QDir(abs).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
      seen++;
      if (name != ".git") {
        stack << (dir.isEmpty() ? name : dir + "/" + name);
      }
    }
  }
  return found;
}

QByteArray readStart(const QString& file, qint64 max = 1 << 20) {
  QFile handle(file);
  return handle.open(QIODevice::ReadOnly) ? handle.read(max) : QByteArray();
}

// What git would run from a git folder: its config and its hooks.
void addGitDir(QCryptographicHash& hash, const QString& dir) {
  hash.addData(readStart(dir + "/config"));
  for (const QString& name : QDir(dir + "/hooks").entryList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name)) {
    hash.addData(name.toUtf8());
    hash.addData(hashFile(dir + "/hooks/" + name).toLatin1());
  }
}

// A fingerprint of a git folder, or of the one a .git file points to.
QString hashGit(const QString& path) {
  QCryptographicHash hash(QCryptographicHash::Sha1);
  QString dir = path;
  if (QFileInfo(path).isFile()) {
    const QByteArray pointer = readStart(path, 4096);
    hash.addData(pointer);
    if (!pointer.startsWith("gitdir:")) {
      return QString::fromLatin1(hash.result().toHex());
    }
    dir = QDir::cleanPath(QDir(QFileInfo(path).path()).absoluteFilePath(utf8(pointer.mid(7)).trimmed()));
  }
  addGitDir(hash, dir);
  // A linked worktree's config and hooks are in the repository it belongs to.
  const QString common = utf8(readStart(dir + "/commondir", 4096)).trimmed();
  if (!common.isEmpty()) {
    addGitDir(hash, QDir::cleanPath(QDir(dir).absoluteFilePath(common)));
  }
  return QString::fromLatin1(hash.result().toHex());
}

} // namespace

QString runLaterReason(const QString& rel) {
  for (const Rule& rule : rules()) {
    if (rule.pattern.match(rel).hasMatch()) {
      return rule.why;
    }
  }
  return {};
}

QMap<QString, QString> snapshotRunLater(const QString& cwd) {
  QMap<QString, QString> snapshot;
  for (const QString& rel : gitFolders(cwd)) {
    snapshot.insert(rel, hashGit(cwd + "/" + rel));
  }
  for (const QString& rel : projectFiles(cwd)) {
    // A .git file is in there already, by what it points to.
    if (runLaterReason(rel).isEmpty() || snapshot.contains(rel)) {
      continue;
    }
    const QString hash = hashFile(cwd + "/" + rel);
    if (!hash.isEmpty()) {
      snapshot.insert(rel, hash);
    }
    if (snapshot.size() >= MAX_HASHED) {
      break;
    }
  }
  return snapshot;
}

QString hashText(const QString& text) {
  return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QList<RunLaterChange> runLaterChanges(const QMap<QString, QString>& before, const QMap<QString, QString>& after) {
  QList<RunLaterChange> changes;
  for (auto it = after.begin(); it != after.end(); ++it) {
    const auto old = before.find(it.key());
    if (old == before.end()) {
      changes.append({it.key(), "created", runLaterReason(it.key())});
    } else if (old.value() != it.value()) {
      changes.append({it.key(), "changed", runLaterReason(it.key())});
    }
  }
  for (auto it = before.begin(); it != before.end(); ++it) {
    if (!after.contains(it.key())) {
      changes.append({it.key(), "deleted", runLaterReason(it.key())});
    }
  }
  std::sort(changes.begin(), changes.end(), [](const RunLaterChange& a, const RunLaterChange& b) { return a.rel < b.rel; });
  return changes;
}

} // namespace Smith
