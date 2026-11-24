#pragma once

// Task worktrees: a second checkout of the repository, on a smith/<task>
// branch, where an agent works while the user works in their own checkout.
// The agent can't commit there (git is read-only in its sandbox); these
// functions are what the user does: create one, see what changed, commit
// and land it on their branch, remove it. They run git directly, as the user.

#include "core/Cancel.h"
#include "core/Diff.h"
#include "core/History.h"
#include "frontend/Entry.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>

namespace Smith {

class Agent;

// Where a checkout keeps its git data. In a linked worktree, `.git` is a file
// pointing at `gitDir`, a folder inside the main repository's .git
// (`commonDir`); in an ordinary checkout both are its .git folder.
struct GitDirs {
  QString gitDir;
  QString commonDir;
  bool linked = false;
};

// Read from the files git leaves, without running git; nullopt if `checkout` has no .git.
std::optional<GitDirs> gitDirs(const QString& checkout);
// The branch checked out in `checkout`; empty when detached or not a checkout.
// Read from HEAD when it's a plain file naming a branch, otherwise asks git.
QString currentBranch(const QString& checkout);

// Task branches are TASK_BRANCH_PREFIX + the task's name.
extern const QString TASK_BRANCH_PREFIX;

// Throws SmithError unless `task` works as a branch and folder name.
void checkTaskName(const QString& task);

struct Worktree {
  QString path;
  QString branch;
  QString head;
  // The repository's own checkout (not a linked worktree).
  bool main = false;
  // The task, for a worktree on a task branch; empty otherwise.
  QString task;
  // A running Smith with this folder as its project; 0 if none.
  qint64 openBy = 0;
};

// The main checkout of the repository `dir` is in. Throws if it isn't in one.
QString mainCheckout(const QString& dir);
// Every checkout of the repository `dir` is in, the main one first.
QList<Worktree> listWorktrees(const QString& dir);

struct CreatedWorktree {
  Worktree worktree;
  // It already existed and was reused.
  bool reused = false;
  // What it branched from, e.g. "main @ 4e1c2a9".
  QString from;
};

// A worktree for `task` beside the repository (in <repo>-worktrees/<task>),
// branching from what's checked out in `dir`. Reuses the task's worktree or
// branch if it has one. Throws SmithError.
CreatedWorktree createTaskWorktree(const QString& dir, const QString& task);

struct FileChange {
  QString path;
  // 'A', 'M' or 'D'.
  QChar status;
  int added = 0;
  int removed = 0;
  bool binary = false;
};

struct WorktreeChanges {
  // Uncommitted changes, new files included (ignored files aren't).
  QList<FileChange> files;
  int added = 0;
  int removed = 0;
  // The same as a unified diff, cut at a size fit for showing.
  QString diff;
  // Commits on the branch that the main checkout's branch doesn't have yet.
  QStringList pendingCommits;

  bool isEmpty() const { return files.isEmpty() && pendingCommits.isEmpty(); }
};

// What changed in a worktree, without touching its index. Throws SmithError.
WorktreeChanges worktreeChanges(const Worktree& worktree);

struct FileDiff {
  FileChange change;
  // Empty for a binary file.
  Diff diff;
};

// The changes' diff split by file, in the form the edit tools show diffs in.
QList<FileDiff> fileDiffs(const WorktreeChanges& changes);

enum class LandMode {
  CherryPick,
  Merge,
  // Commit on the task branch and leave it there.
  BranchOnly,
};

struct LandResult {
  // The commit made on the task branch; empty if there was nothing uncommitted.
  QString commit;
  // The branch landed on, in the main checkout; empty for BranchOnly.
  QString target;
  // The main checkout's new HEAD.
  QString landed;
  // Why it couldn't land (the commit is kept on the task branch); empty on success.
  QString problem;
  QStringList conflicts;
};

// Commits the worktree's changes on its branch (with `message`), then brings
// them to the main checkout's branch. A conflict, or uncommitted changes of
// the user's in the way, stops it with nothing changed there; see `problem`.
LandResult landWorktree(const Worktree& worktree, const QString& message, LandMode mode);

// Removes the worktree's folder, and with `deleteBranch` its branch.
// `discard` throws away uncommitted changes; without it, they make this fail.
void removeWorktree(const Worktree& worktree, bool deleteBranch, bool discard);

// "3 files changed (+41 −6)", with commits waiting to land if any; "no changes" when there's nothing.
QString changeSummary(const WorktreeChanges& changes);
// The changes as transcript entries, one per file with its diff, like the edit tools'.
QList<Entry> changeEntries(const WorktreeChanges& changes);
// What the user first asked for in a conversation; empty if nothing.
QString firstRequest(const Messages& messages);
// What landing did, for the user: the commit, and where it landed or why it didn't.
QString landingReport(const Worktree& worktree, LandMode mode, const LandResult& result, const QString& mainPath);

// Records that this process has `checkout` open (when it's a linked
// worktree), for listWorktrees' `openBy`. Clearing removes the record.
void markWorktreeOpen(const QString& checkout);
void clearWorktreeOpen(const QString& checkout);

// A one-line commit message for the changes, from the model; `request` is
// what the user asked for, when known. Falls back to a plain message when
// the model can't help.
QString suggestCommitMessage(const std::shared_ptr<Agent>& agent, const Worktree& worktree, const WorktreeChanges& changes,
                             const QString& request, const CancelPtr& cancel);

} // namespace Smith
