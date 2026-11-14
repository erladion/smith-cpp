#include "frontend/Worktrees.h"
#include "compat/Utf8.h"

#include "core/Agent.h"
#include "core/Dirs.h"
#include "core/Errors.h"
#include "core/Process.h"
#include "core/Sandbox.h"
#include "core/Tool.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <cerrno>
#include <signal.h>

namespace Smith {

const QString TASK_BRANCH_PREFIX = "smith/";

namespace {

// Enough to review; the whole change is in the worktree either way.
constexpr int MAX_DIFF_CHARS = 200000;
// What the model sees when suggesting a commit message.
constexpr int MAX_MESSAGE_DIFF_CHARS = 12000;
// Hooks (pre-commit) can take a while.
constexpr int GIT_TIMEOUT_MS = 300000;
const QString OPEN_MARKER = "smith.pid";

struct GitResult {
  int code = -1;
  QString out;
  QString err;

  bool ok() const { return code == 0; }
};

GitResult git(const QString& dir, const QStringList& args, const QMap<QString, QString>& extraEnv = {}) {
  QProcessEnvironment env = hostEnv();
  // Set when Smith itself runs from a git hook; they'd point every command at that repository.
  for (const QString name : {"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR", "GIT_PREFIX"}) {
    env.remove(name);
  }
  // Messages in English, to recognize them; never wait for a prompt.
  env.insert("LC_ALL", "C");
  env.insert("GIT_TERMINAL_PROMPT", "0");
  for (auto it = extraEnv.begin(); it != extraEnv.end(); ++it) {
    env.insert(it.key(), it.value());
  }
  RunOptions options;
  options.timeoutMs = GIT_TIMEOUT_MS;
  const ProcessResult result = runProcess(SpawnSpec{"git", QStringList{"-C", dir} + args, env, {}, {}}, options);
  if (!result.started) {
    throw SmithError(result.missing ? "git isn't installed." : "Could not run git: " + result.startError);
  }
  // -z output is NUL-separated: utf8() keeps the NULs.
  return GitResult{result.crashed ? -1 : result.exitCode, utf8(result.out), utf8(result.err)};
}

// git's own message for a failure, without its hint lines.
QString gitError(const GitResult& result) {
  QStringList lines;
  for (const QString& line : (result.err + "\n" + result.out).split('\n')) {
    if (!line.trimmed().isEmpty() && !line.startsWith("hint:")) {
      lines << line.trimmed();
    }
  }
  return lines.isEmpty() ? QString("git failed (exit code %1).").arg(result.code) : lines.join('\n');
}

QString gitOut(const QString& dir, const QStringList& args, const QMap<QString, QString>& extraEnv = {}) {
  const GitResult result = git(dir, args, extraEnv);
  if (!result.ok()) {
    throw SmithError(gitError(result));
  }
  return result.out;
}

QString readFirstLine(const QString& file) {
  QFile handle(file);
  if (!handle.open(QIODevice::ReadOnly)) {
    return {};
  }
  return utf8(handle.readLine()).trimmed();
}

// When the process started, in clock ticks since boot: with the pid, it tells a
// live Smith from an unrelated process that got the same pid later.
QString processStart(qint64 pid) {
  const QString stat = readFirstLine(QString("/proc/%1/stat").arg(pid));
  const int close = stat.lastIndexOf(')');
  if (close < 0) {
    return {};
  }
  // Fields after the name start at the 3rd; the start time is the 22nd.
  const QStringList fields = stat.mid(close + 2).split(' ');
  return fields.size() > 19 ? fields.at(19) : QString();
}

qint64 openBy(const QString& checkout) {
  const std::optional<GitDirs> dirs = gitDirs(checkout);
  if (!dirs || !dirs->linked) {
    return 0;
  }
  const QStringList parts = readFirstLine(dirs->gitDir + "/" + OPEN_MARKER).split(' ');
  if (parts.size() != 2) {
    return 0;
  }
  const qint64 pid = parts.at(0).toLongLong();
  if (pid <= 0 || (::kill(pid_t(pid), 0) != 0 && errno != EPERM)) {
    return 0;
  }
  return processStart(pid) == parts.at(1) ? pid : 0;
}

// Files named in git's -z output for --name-status and --numstat (no renames).
QList<FileChange> parseChanges(const QString& nameStatus, const QString& numstat) {
  QMap<QString, FileChange> byPath;
  QStringList order;
  const QStringList names = nameStatus.split(QChar(0), Qt::SkipEmptyParts);
  for (int i = 0; i + 1 < names.size(); i += 2) {
    FileChange change;
    change.status = names.at(i).at(0);
    change.path = names.at(i + 1);
    byPath.insert(change.path, change);
    order << change.path;
  }
  for (const QString& record : numstat.split(QChar(0), Qt::SkipEmptyParts)) {
    const QStringList parts = record.split('\t');
    if (parts.size() < 3 || !byPath.contains(parts.at(2))) {
      continue;
    }
    FileChange& change = byPath[parts.at(2)];
    change.binary = parts.at(0) == "-";
    change.added = parts.at(0).toInt();
    change.removed = parts.at(1).toInt();
  }
  QList<FileChange> files;
  for (const QString& path : order) {
    files.append(byPath.value(path));
  }
  return files;
}

// Commits on `branch` that `target` doesn't have an equivalent of, oldest first: "sha subject".
QStringList pendingOn(const QString& dir, const QString& target, const QString& branch) {
  QStringList pending;
  for (const QString& line : gitOut(dir, {"cherry", "-v", target, branch}).split('\n', Qt::SkipEmptyParts)) {
    if (line.startsWith("+ ")) {
      pending << line.mid(2);
    }
  }
  return pending;
}

QString shortSha(const QString& dir, const QString& rev = "HEAD") {
  return gitOut(dir, {"rev-parse", "--short", rev}).trimmed();
}

// The files a conflict or a dirty checkout got in the way with, from git's output.
QStringList filesInTheWay(const QString& output) {
  QStringList files;
  bool listing = false;
  for (const QString& line : output.split('\n')) {
    if (line.contains("would be overwritten by")) {
      listing = true;
    } else if (listing && line.startsWith('\t')) {
      files << line.trimmed();
    } else {
      listing = false;
    }
  }
  return files;
}

} // namespace

std::optional<GitDirs> gitDirs(const QString& checkout) {
  const QString dotGit = checkout + "/.git";
  const QFileInfo info(dotGit);
  if (info.isDir()) {
    const QString dir = QDir::cleanPath(info.absoluteFilePath());
    return GitDirs{dir, dir, false};
  }
  if (!info.isFile()) {
    return std::nullopt;
  }
  const QString line = readFirstLine(dotGit);
  if (!line.startsWith("gitdir:")) {
    return std::nullopt;
  }
  GitDirs dirs;
  dirs.gitDir = QDir::cleanPath(QDir(checkout).absoluteFilePath(line.mid(7).trimmed()));
  const QString common = readFirstLine(dirs.gitDir + "/commondir");
  // Only linked worktrees have a commondir; a submodule's git folder is its own.
  dirs.linked = !common.isEmpty();
  dirs.commonDir = dirs.linked ? QDir::cleanPath(QDir(dirs.gitDir).absoluteFilePath(common)) : dirs.gitDir;
  return dirs;
}

QString currentBranch(const QString& checkout) {
  const std::optional<GitDirs> dirs = gitDirs(checkout);
  if (!dirs) {
    return {};
  }
  const QString head = readFirstLine(dirs->gitDir + "/HEAD");
  return head.startsWith("ref: refs/heads/") ? head.mid(16) : QString();
}

void checkTaskName(const QString& task) {
  static const QRegularExpression valid("^[A-Za-z0-9][A-Za-z0-9._-]*$");
  if (task.isEmpty()) {
    throw SmithError("Name the task, e.g. /worktree fix-login.");
  }
  if (!valid.match(task).hasMatch() || task.contains("..") || task.endsWith(".lock") || task.size() > 64) {
    throw SmithError(QString("\"%1\" can't be a task name: use letters, digits, '-', '_' and '.' (up to 64), e.g. fix-login.").arg(task));
  }
}

QString mainCheckout(const QString& dir) {
  const GitResult result = git(dir, {"worktree", "list", "--porcelain"});
  if (!result.ok()) {
    throw SmithError(QString("%1 isn't in a git repository.").arg(tildify(dir)));
  }
  const QStringList lines = result.out.split('\n');
  if (lines.size() > 2 && lines.at(2) == "bare") {
    throw SmithError("Task worktrees need a repository with a checkout of its own; this one is bare.");
  }
  if (lines.isEmpty() || !lines.first().startsWith("worktree ")) {
    throw SmithError("git didn't list the repository's checkouts.");
  }
  return lines.first().mid(9);
}

QList<Worktree> listWorktrees(const QString& dir) {
  QList<Worktree> worktrees;
  bool skip = false;
  // Drops the entry just read when it was marked to skip.
  auto finish = [&]() {
    if (!worktrees.isEmpty() && skip) {
      worktrees.removeLast();
    }
    skip = false;
  };
  for (const QString& line : gitOut(dir, {"worktree", "list", "--porcelain"}).split('\n')) {
    if (line.startsWith("worktree ")) {
      finish();
      Worktree worktree;
      worktree.path = line.mid(9);
      worktree.main = worktrees.isEmpty();
      worktrees.append(worktree);
    } else if (worktrees.isEmpty()) {
      continue;
    } else if (line.startsWith("HEAD ")) {
      worktrees.last().head = line.mid(5, 7);
    } else if (line.startsWith("branch refs/heads/")) {
      worktrees.last().branch = line.mid(18);
    } else if (line == "bare" || line.startsWith("prunable")) {
      // A bare repository has no checkout; a prunable one's folder is gone.
      skip = true;
    }
  }
  finish();
  for (Worktree& worktree : worktrees) {
    if (!worktree.main && worktree.branch.startsWith(TASK_BRANCH_PREFIX)) {
      worktree.task = worktree.branch.mid(TASK_BRANCH_PREFIX.size());
    }
    worktree.openBy = openBy(worktree.path);
  }
  return worktrees;
}

CreatedWorktree createTaskWorktree(const QString& dir, const QString& task) {
  checkTaskName(task);
  const QString main = mainCheckout(dir);
  const QString branch = TASK_BRANCH_PREFIX + task;
  for (const Worktree& worktree : listWorktrees(dir)) {
    if (worktree.branch == branch) {
      return CreatedWorktree{worktree, true, {}};
    }
  }

  const QFileInfo repo(main);
  const QString folder = repo.path() + "/" + repo.fileName() + "-worktrees/" + task;
  if (pathExists(folder)) {
    throw SmithError(QString("%1 already exists and isn't the task's worktree; pick another name.").arg(tildify(folder)));
  }
  CreatedWorktree created;
  if (git(main, {"show-ref", "--verify", "--quiet", "refs/heads/" + branch}).ok()) {
    // The branch outlived its worktree (removed by hand, or kept when landing).
    gitOut(main, {"worktree", "add", folder, branch});
    created.from = branch;
  } else {
    const QString base = currentBranch(gitOut(dir, {"rev-parse", "--show-toplevel"}).trimmed());
    const QString sha = shortSha(dir);
    gitOut(dir, {"worktree", "add", "-b", branch, folder, "HEAD"});
    created.from = base.isEmpty() ? sha : base + " @ " + sha;
  }
  const QString real = realPath(folder);
  for (const Worktree& worktree : listWorktrees(dir)) {
    if (worktree.path == folder || worktree.path == real) {
      created.worktree = worktree;
      return created;
    }
  }
  throw SmithError("git made the worktree but doesn't list it.");
}

WorktreeChanges worktreeChanges(const Worktree& worktree) {
  // A throwaway index: the worktree's own stays as it is.
  QTemporaryDir temp;
  if (!temp.isValid()) {
    throw SmithError("Could not create a temporary folder for git.");
  }
  const QMap<QString, QString> env{{"GIT_INDEX_FILE", temp.path() + "/index"}};
  const QString dir = worktree.path;
  gitOut(dir, {"read-tree", "HEAD"}, env);
  gitOut(dir, {"add", "-A"}, env);
  WorktreeChanges changes;
  changes.files = parseChanges(gitOut(dir, {"diff", "--cached", "--no-renames", "--name-status", "-z"}, env),
                               gitOut(dir, {"diff", "--cached", "--no-renames", "--numstat", "-z"}, env));
  for (const FileChange& file : changes.files) {
    changes.added += file.added;
    changes.removed += file.removed;
  }
  changes.diff = gitOut(dir, {"diff", "--cached", "--no-renames"}, env);
  if (changes.diff.size() > MAX_DIFF_CHARS) {
    changes.diff = changes.diff.left(MAX_DIFF_CHARS) + "\n… (cut here; the rest is in the worktree)\n";
  }
  if (!worktree.branch.isEmpty()) {
    const QString target = currentBranch(mainCheckout(dir));
    if (!target.isEmpty() && target != worktree.branch) {
      changes.pendingCommits = pendingOn(dir, target, worktree.branch);
    }
  }
  return changes;
}

QList<FileDiff> fileDiffs(const WorktreeChanges& changes) {
  static const QRegularExpression hunkHeader(R"(^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,\d+)? @@)");
  QList<FileDiff> files;
  bool inHunk = false;
  // The next line numbers in the old and new file; 0 before the file's first hunk.
  int oldLine = 0;
  int newLine = 0;
  for (const QString& line : changes.diff.split('\n')) {
    if (line.startsWith("diff --git ")) {
      // Same order as the file list: both come sorted by path.
      FileDiff file;
      if (files.size() < changes.files.size()) {
        file.change = changes.files.at(files.size());
      } else {
        file.change.path = line.section(" b/", -1);
      }
      files.append(file);
      inHunk = false;
      oldLine = 0;
      newLine = 0;
      continue;
    }
    if (files.isEmpty()) {
      continue;
    }
    Diff& diff = files.last().diff;
    const QRegularExpressionMatch hunk = hunkHeader.match(line);
    if (hunk.hasMatch()) {
      // A new file's hunk starts at "-0,0": there's no old line 0.
      const int oldStart = std::max(1, hunk.captured(1).toInt());
      const int newStart = std::max(1, hunk.captured(3).toInt());
      // Unchanged lines before the hunk: from the file's start, or since the last hunk.
      const int skipped = oldStart - (oldLine == 0 ? 1 : oldLine);
      if (skipped > 0) {
        diff.append(DiffLine{DiffLine::Kind::Gap, {}, 0, 0, skipped});
      }
      oldLine = oldStart;
      newLine = newStart;
      inHunk = true;
      continue;
    }
    if (!inHunk || line.isEmpty() || line.startsWith('\\')) {
      continue;
    }
    const QString text = line.mid(1);
    if (line.startsWith('+')) {
      diff.append(DiffLine{DiffLine::Kind::Add, text, 0, newLine++, 0});
    } else if (line.startsWith('-')) {
      diff.append(DiffLine{DiffLine::Kind::Remove, text, oldLine++, 0, 0});
    } else if (line.startsWith(' ')) {
      diff.append(DiffLine{DiffLine::Kind::Context, text, oldLine++, newLine++, 0});
    }
  }
  return files;
}

LandResult landWorktree(const Worktree& worktree, const QString& message, LandMode mode) {
  if (worktree.main) {
    throw SmithError("That's the repository's own checkout, not a task worktree.");
  }
  if (worktree.branch.isEmpty()) {
    throw SmithError(QString("%1 isn't on a branch, so there's nothing to land it as.").arg(tildify(worktree.path)));
  }
  LandResult result;
  const QString dir = worktree.path;
  if (!gitOut(dir, {"status", "--porcelain"}).trimmed().isEmpty()) {
    if (message.trimmed().isEmpty()) {
      throw SmithError("The commit needs a message.");
    }
    gitOut(dir, {"add", "-A"});
    gitOut(dir, {"commit", "-q", "-m", message.trimmed()});
    result.commit = shortSha(dir);
  }
  if (mode == LandMode::BranchOnly) {
    return result;
  }

  const QString main = mainCheckout(dir);
  result.target = currentBranch(main);
  if (result.target.isEmpty()) {
    result.problem = QString("Your checkout (%1) isn't on a branch, so the work stays on %2.").arg(tildify(main), worktree.branch);
    return result;
  }
  const QStringList pending = pendingOn(main, result.target, worktree.branch);
  if (pending.isEmpty()) {
    result.landed = shortSha(main);
    return result;
  }
  QStringList args;
  QStringList abort;
  if (mode == LandMode::CherryPick) {
    args = QStringList{"cherry-pick"};
    for (const QString& line : pending) {
      args << line.section(' ', 0, 0);
    }
    abort = QStringList{"cherry-pick", "--abort"};
  } else {
    args = QStringList{"merge", "--no-edit", worktree.branch};
    abort = QStringList{"merge", "--abort"};
  }
  const GitResult picked = git(main, args);
  if (picked.ok()) {
    result.landed = shortSha(main);
    return result;
  }
  const QStringList inTheWay = filesInTheWay(picked.err + "\n" + picked.out);
  if (!inTheWay.isEmpty()) {
    // git stopped before changing anything.
    result.conflicts = inTheWay;
    result.problem = QString("Your uncommitted changes in %1 touch the same files. Commit or stash them, then land again; the work is on %2.")
                         .arg(tildify(main), worktree.branch);
    return result;
  }
  result.conflicts = gitOut(main, {"diff", "--name-only", "--diff-filter=U"}).split('\n', Qt::SkipEmptyParts);
  git(main, abort);
  if (result.conflicts.isEmpty()) {
    result.problem = QString("git couldn't land it: %1\nThe work is on %2.").arg(gitError(picked), worktree.branch);
  } else {
    result.problem = QString("It conflicts with %1, so nothing changed there. The work is on %2: %3 it yourself to resolve the conflicts.")
                         .arg(result.target, worktree.branch, mode == LandMode::CherryPick ? "cherry-pick" : "merge");
  }
  return result;
}

void removeWorktree(const Worktree& worktree, bool deleteBranch, bool discard) {
  if (worktree.main) {
    throw SmithError("That's the repository's own checkout; it can't be removed.");
  }
  const QString main = mainCheckout(worktree.path);
  QStringList args{"worktree", "remove"};
  if (discard) {
    args << "--force";
  }
  const GitResult removed = git(main, args << worktree.path);
  if (!removed.ok()) {
    throw SmithError(gitError(removed).contains("contains modified or untracked files")
                         ? QString("%1 has changes that aren't landed yet.").arg(tildify(worktree.path))
                         : gitError(removed));
  }
  if (deleteBranch && !worktree.branch.isEmpty()) {
    gitOut(main, {"branch", "-D", worktree.branch});
  }
  // <repo>-worktrees once its last task is gone; rmdir leaves it if anything's left.
  QDir().rmdir(QFileInfo(worktree.path).path());
}

QString changeSummary(const WorktreeChanges& changes) {
  QStringList parts;
  if (!changes.files.isEmpty()) {
    parts << QString("%1 changed (+%2 −%3)").arg(plural(changes.files.size(), "file")).arg(changes.added).arg(changes.removed);
  }
  if (!changes.pendingCommits.isEmpty()) {
    parts << plural(changes.pendingCommits.size(), "commit") + " to land";
  }
  return parts.isEmpty() ? QString("no changes") : parts.join(", ");
}

QList<Entry> changeEntries(const WorktreeChanges& changes) {
  QList<Entry> entries;
  for (const FileDiff& file : fileDiffs(changes)) {
    Entry entry;
    entry.kind = Entry::Kind::Tool;
    entry.name = file.change.path;
    if (file.change.binary) {
      entry.summary = "binary";
    } else if (file.change.status == 'A') {
      entry.summary = QString("new, +%1").arg(file.change.added);
    } else if (file.change.status == 'D') {
      entry.summary = QString("deleted, −%1").arg(file.change.removed);
    } else {
      entry.summary = QString("+%1 −%2").arg(file.change.added).arg(file.change.removed);
    }
    if (!file.diff.isEmpty()) {
      entry.diff = file.diff;
    }
    entries.append(entry);
  }
  return entries;
}

QString firstRequest(const Messages& messages) {
  for (const Message& message : messages) {
    if (messageRole(message) == "user") {
      return messageText(message).trimmed();
    }
  }
  return {};
}

QString landingReport(const Worktree& worktree, LandMode mode, const LandResult& result, const QString& mainPath) {
  QStringList lines;
  if (!result.commit.isEmpty()) {
    lines << QString("Committed %1 on %2.").arg(result.commit, worktree.branch);
  }
  if (!result.problem.isEmpty()) {
    lines << result.problem;
    if (!result.conflicts.isEmpty()) {
      lines << "  " + result.conflicts.join("\n  ");
    }
  } else if (mode == LandMode::BranchOnly) {
    lines << QString("It's there to land when you want: /land %1.").arg(worktree.task.isEmpty() ? QString() : worktree.task).trimmed();
  } else if (mode == LandMode::CherryPick) {
    lines << QString("Cherry-picked onto %1 in %2, now at %3. Your own uncommitted changes there weren't touched.")
                 .arg(result.target, tildify(mainPath), result.landed);
  } else {
    lines << QString("Merged into %1 in %2, now at %3. Your own uncommitted changes there weren't touched.")
                 .arg(result.target, tildify(mainPath), result.landed);
  }
  return lines.join('\n');
}

void markWorktreeOpen(const QString& checkout) {
  const std::optional<GitDirs> dirs = checkout.isEmpty() ? std::nullopt : gitDirs(checkout);
  if (!dirs || !dirs->linked) {
    return;
  }
  const qint64 pid = QCoreApplication::applicationPid();
  QFile file(dirs->gitDir + "/" + OPEN_MARKER);
  if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    file.write(QString("%1 %2\n").arg(pid).arg(processStart(pid)).toUtf8());
  }
}

void clearWorktreeOpen(const QString& checkout) {
  const std::optional<GitDirs> dirs = checkout.isEmpty() ? std::nullopt : gitDirs(checkout);
  if (!dirs || !dirs->linked) {
    return;
  }
  const QString file = dirs->gitDir + "/" + OPEN_MARKER;
  // Only our own record: another window may have opened it since.
  if (readFirstLine(file).section(' ', 0, 0).toLongLong() == QCoreApplication::applicationPid()) {
    QFile::remove(file);
  }
}

QString suggestCommitMessage(const std::shared_ptr<Agent>& agent, const Worktree& worktree, const WorktreeChanges& changes,
                             const QString& request, const CancelPtr& cancel) {
  const QString fallback =
      changes.files.size() == 1 ? QString("Update %1").arg(QFileInfo(changes.files.first().path).fileName()) : "Changes from " + worktree.branch;
  if (!agent || changes.files.isEmpty()) {
    return fallback;
  }
  QStringList files;
  for (const FileChange& file : changes.files) {
    files << QString("%1 %2").arg(file.status).arg(file.path);
  }
  QString input;
  if (!request.trimmed().isEmpty()) {
    input += "What the user asked for:\n" + request.trimmed().left(2000) + "\n\n";
  }
  input += "Files changed:\n" + files.join('\n') + "\n\nDiff:\n" + changes.diff.left(MAX_MESSAGE_DIFF_CHARS);
  const QString instructions =
      "You write git commit messages. Reply with the subject line only: what the change does, in the imperative mood (\"Fix\", \"Add\"), at "
      "most 72 characters, without quotes or a trailing period.";
  try {
    for (QString line : agent->complete(instructions, input, cancel).split('\n')) {
      static const QRegularExpression wrapping(R"(^[\s"'`*]+|[\s"'`*.]+$)");
      line = line.remove(wrapping);
      if (!line.isEmpty()) {
        return line.left(100);
      }
    }
  } catch (const AbortError&) {
    throw;
  } catch (const std::exception&) {
  }
  return fallback;
}

} // namespace Smith
