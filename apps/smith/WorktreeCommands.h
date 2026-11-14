#pragma once

#include "frontend/Worktrees.h"

#include <QMap>

#include <memory>
#include <optional>

namespace Smith::Ui {

class App;

// /worktree, /worktrees, /land and /drop: a task in a worktree of its own,
// and bringing its work to the user's branch.
class WorktreeCommands {
public:
  explicit WorktreeCommands(App& app);

  // Makes (or reuses) the task's worktree and switches to it.
  void create(const QString& task);
  void openPicker();
  // The named task, or the worktree the project is in when `task` is empty.
  void land(const QString& task);
  void drop(const QString& task);

private:
  // What /land works out before asking anything.
  struct Landing {
    Worktree worktree;
    WorktreeChanges changes;
    QString mainPath;
    QString target;
    QString message;
  };

  // The task's worktree; prints why not and returns nullopt if there's none.
  std::optional<Worktree> find(const QString& task);
  bool isCurrent(const Worktree& worktree) const;
  // Prints why not when another Smith has it open.
  bool refuseIfOpenElsewhere(const Worktree& worktree);
  void showPicker(const QList<Worktree>& worktrees, const QMap<QString, QString>& summaries);
  void openMenu(const Worktree& worktree);
  void startLanding(const Worktree& worktree);
  void chooseLanding(const std::shared_ptr<Landing>& landing);
  void askMessage(const std::shared_ptr<Landing>& landing, LandMode mode);
  void finishLanding(const std::shared_ptr<Landing>& landing, LandMode mode, const QString& message);
  void startDrop(const Worktree& worktree);
  // Leaves the worktree first if it's the project, then removes it.
  void remove(const Worktree& worktree, const QString& mainPath, bool discard);

  App& m_app;
};

} // namespace Smith::Ui
