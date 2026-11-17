#include "WorktreeCommands.h"

#include "core/Dirs.h"
#include "core/Errors.h"
#include "core/Tool.h"
#include "App.h"
#include "SessionCommands.h"

#include <QCoreApplication>
#include <QFileInfo>

namespace Smith::Ui {

namespace {

const QString DIFF = "diff";

QString folderName(const QString& path) {
  return QFileInfo(path).fileName();
}

} // namespace

WorktreeCommands::WorktreeCommands(App& app) : m_app(app) {}

void WorktreeCommands::create(const QString& task) {
  if (task.isEmpty()) {
    m_app.print(Entry::notice(NoticeLevel::Error, "Name the task, e.g. /worktree fix-login. /worktrees lists the ones there are."));
    return;
  }
  if (!m_app.opts().controller.sandbox->active()) {
    m_app.print(Entry::notice(NoticeLevel::Error, "Task worktrees need the sandbox: it's what keeps git read-only for the agent. Start Smith "
                                                  "without --sandbox off (or \"sandbox\": \"off\" in the config) to use them."));
    return;
  }
  const QString cwd = m_app.project().cwd;
  m_app.withActivity("Making the worktree…", [this, cwd, task]() -> std::function<void()> {
    const CreatedWorktree created = createTaskWorktree(cwd, task);
    return [this, created]() {
      const Worktree& worktree = created.worktree;
      if (isCurrent(worktree)) {
        m_app.print(Entry::notice(NoticeLevel::Info, QString("You're in %1's worktree already.").arg(worktree.branch)));
        return;
      }
      if (created.reused) {
        m_app.controller().resumeIn(worktree.path, {Entry::notice(NoticeLevel::Info, QString("Back in the task's worktree, on %1.").arg(worktree.branch))});
        return;
      }
      m_app.controller().newSession(
          worktree.path, {Entry::notice(NoticeLevel::Info, QString("Made a worktree for the task: %1, on %2 (from %3). The agent's changes stay here; "
                                                                   "git is read-only for it, so it can't commit. /land brings them to your branch.")
                                                               .arg(tildify(worktree.path), worktree.branch, created.from))});
    };
  });
}

void WorktreeCommands::openPicker() {
  const QString cwd = m_app.project().cwd;
  m_app.withActivity("Looking at the worktrees…", [this, cwd]() -> std::function<void()> {
    const QList<Worktree> worktrees = listWorktrees(cwd);
    QMap<QString, QString> summaries;
    for (const Worktree& worktree : worktrees) {
      if (worktree.main) {
        continue;
      }
      try {
        summaries.insert(worktree.path, changeSummary(worktreeChanges(worktree)));
      } catch (const std::exception& error) {
        summaries.insert(worktree.path, errorMessage(error));
      }
    }
    return [this, worktrees, summaries]() { showPicker(worktrees, summaries); };
  });
}

void WorktreeCommands::showPicker(const QList<Worktree>& worktrees, const QMap<QString, QString>& summaries) {
  if (worktrees.size() < 2) {
    m_app.print(Entry::notice(NoticeLevel::Info, "There are no task worktrees here yet. /worktree <task> makes one."));
    return;
  }
  const qint64 self = QCoreApplication::applicationPid();
  QList<Tui::PickerItem> items;
  QString current;
  for (const Worktree& worktree : worktrees) {
    QStringList hint;
    hint << (worktree.branch.isEmpty() ? QString("detached at %1").arg(worktree.head) : worktree.branch);
    if (!worktree.main) {
      hint << summaries.value(worktree.path);
    }
    if (worktree.openBy == self) {
      hint << "this window";
    } else if (worktree.openBy) {
      hint << "open in another Smith";
    }
    const QString label = worktree.main ? folderName(worktree.path) + " (your checkout)"
                                        : (worktree.task.isEmpty() ? folderName(worktree.path) : worktree.task);
    items.append({worktree.path, label, hint.join(" · ")});
    if (isCurrent(worktree)) {
      current = worktree.path;
    }
  }
  Tui::PickerOptions options;
  options.help = "type to filter · enter opens it (land or drop from there) · esc cancels";
  m_app.openPicker(std::make_unique<Tui::Picker>("Worktrees", items, current, options), [this, worktrees](const QString& path) {
    for (const Worktree& worktree : worktrees) {
      if (worktree.path != path) {
        continue;
      }
      if (!worktree.main) {
        openMenu(worktree);
      } else if (!isCurrent(worktree)) {
        m_app.controller().resumeIn(worktree.path);
      }
    }
  });
}

void WorktreeCommands::openMenu(const Worktree& worktree) {
  QList<Tui::PickerItem> items;
  if (!isCurrent(worktree)) {
    items.append({"switch", "Switch to it", tildify(worktree.path)});
  }
  items.append({"land", "Land it…", "commit its changes and bring them to your branch"});
  items.append({"drop", "Drop it…", "remove the worktree and its branch"});
  const QString title = worktree.branch.isEmpty() ? folderName(worktree.path) : worktree.branch;
  m_app.openPicker(std::make_unique<Tui::Picker>(title, items), [this, worktree](const QString& value) {
    if (value == "switch") {
      m_app.controller().resumeIn(worktree.path);
    } else if (value == "land") {
      startLanding(worktree);
    } else if (value == "drop") {
      startDrop(worktree);
    }
  });
}

std::optional<Worktree> WorktreeCommands::find(const QString& task) {
  QList<Worktree> worktrees;
  try {
    worktrees = listWorktrees(m_app.project().cwd);
  } catch (const std::exception& error) {
    m_app.print(Entry::notice(NoticeLevel::Error, errorMessage(error)));
    return std::nullopt;
  }
  for (const Worktree& worktree : worktrees) {
    if (worktree.main) {
      continue;
    }
    if (task.isEmpty() ? isCurrent(worktree) : worktree.task == task) {
      return worktree;
    }
  }
  m_app.print(Entry::notice(NoticeLevel::Error, task.isEmpty()
                                                    ? QString("You're not in a task worktree: name the task (e.g. /land fix-login), or pick one in /worktrees.")
                                                    : QString("There's no task \"%1\" here. /worktrees lists them.").arg(task)));
  return std::nullopt;
}

bool WorktreeCommands::isCurrent(const Worktree& worktree) const {
  const QString cwd = realPath(m_app.project().cwd);
  return cwd == worktree.path || cwd.startsWith(worktree.path + "/");
}

bool WorktreeCommands::refuseIfOpenElsewhere(const Worktree& worktree) {
  if (worktree.openBy == 0 || worktree.openBy == QCoreApplication::applicationPid()) {
    return false;
  }
  m_app.print(Entry::notice(NoticeLevel::Error, QString("%1 is open in another Smith (pid %2), where the agent may still be working. Do it from "
                                                        "there, or quit that one first.")
                                                    .arg(worktree.branch)
                                                    .arg(worktree.openBy)));
  return true;
}

void WorktreeCommands::land(const QString& task) {
  if (const std::optional<Worktree> worktree = find(task)) {
    startLanding(*worktree);
  }
}

void WorktreeCommands::startLanding(const Worktree& worktree) {
  if (refuseIfOpenElsewhere(worktree)) {
    return;
  }
  // What was asked for helps the model name the commit; only known in the worktree's own session.
  const QString request = isCurrent(worktree) ? firstRequest(m_app.agent()->messages()) : QString();
  const std::shared_ptr<Agent> agent = m_app.agent();
  m_app.withActivity("Reading the changes…", [this, worktree, request, agent]() -> std::function<void()> {
    auto landing = std::make_shared<Landing>();
    landing->worktree = worktree;
    landing->changes = worktreeChanges(worktree);
    landing->mainPath = mainCheckout(worktree.path);
    landing->target = currentBranch(landing->mainPath);
    if (!landing->changes.files.isEmpty()) {
      landing->message = suggestCommitMessage(agent, worktree, landing->changes, request, makeCancel());
    }
    return [this, landing]() {
      const WorktreeChanges& changes = landing->changes;
      if (changes.isEmpty()) {
        m_app.print(Entry::notice(NoticeLevel::Info, QString("Nothing to land: %1 has no changes, and %2 has all its commits.")
                                                         .arg(landing->worktree.branch, landing->target.isEmpty() ? "your checkout" : landing->target)));
        return;
      }
      QStringList lines{QString("%1: %2").arg(landing->worktree.branch, changeSummary(changes))};
      for (const FileChange& file : changes.files) {
        lines << QString("  %1 %2  %3").arg(file.status).arg(file.path, file.binary ? QString("binary") : QString("+%1 −%2").arg(file.added).arg(file.removed));
      }
      if (!changes.pendingCommits.isEmpty()) {
        lines << "Committed already, not on " + (landing->target.isEmpty() ? QString("your branch") : landing->target) + " yet:";
        for (const QString& commit : changes.pendingCommits) {
          lines << "  " + commit;
        }
      }
      m_app.print(Entry::notice(NoticeLevel::Info, lines.join('\n')));
      chooseLanding(landing);
    };
  });
}

void WorktreeCommands::chooseLanding(const std::shared_ptr<Landing>& landing) {
  const QString branch = landing->worktree.branch;
  const QString target = landing->target;
  const bool uncommitted = !landing->changes.files.isEmpty();
  QList<Tui::PickerItem> items;
  if (!target.isEmpty()) {
    items.append({"cherry-pick", "Cherry-pick onto " + target, QString("a commit on top of %1, in %2").arg(target, tildify(landing->mainPath))});
    items.append({"merge", "Merge into " + target, "a merge commit (or a fast-forward)"});
  }
  if (uncommitted) {
    items.append({"branch", QString("Commit on %1 only").arg(branch), "and land it yourself later"});
    items.append({DIFF, "Show the diff", plural(landing->changes.files.size(), "file")});
  }
  if (items.isEmpty()) {
    m_app.print(Entry::notice(NoticeLevel::Error, QString("Your checkout (%1) isn't on a branch, so there's nowhere to land %2. Check out a branch "
                                                          "there first.")
                                                      .arg(tildify(landing->mainPath), branch)));
    return;
  }
  m_app.openPicker(std::make_unique<Tui::Picker>("Land " + branch, items), [this, landing, uncommitted](const QString& value) {
    if (value == DIFF) {
      for (const Entry& entry : changeEntries(landing->changes)) {
        m_app.print(entry);
      }
      chooseLanding(landing);
      return;
    }
    const LandMode mode = value == "merge" ? LandMode::Merge : value == "branch" ? LandMode::BranchOnly : LandMode::CherryPick;
    if (!uncommitted) {
      finishLanding(landing, mode, {});
      return;
    }
    askMessage(landing, mode);
  });
}

void WorktreeCommands::askMessage(const std::shared_ptr<Landing>& landing, LandMode mode) {
  m_app.promptText("Commit message for " + landing->worktree.branch, landing->message, [this, landing, mode](const QString& message) {
    if (message.trimmed().isEmpty()) {
      m_app.print(Entry::notice(NoticeLevel::Warn, "The commit needs a message."));
      askMessage(landing, mode);
      return;
    }
    finishLanding(landing, mode, message.trimmed());
  });
}

void WorktreeCommands::finishLanding(const std::shared_ptr<Landing>& landing, LandMode mode, const QString& message) {
  m_app.withActivity("Landing…", [this, landing, mode, message]() -> std::function<void()> {
    const LandResult result = landWorktree(landing->worktree, message, mode);
    return [this, landing, mode, result]() {
      const Worktree& worktree = landing->worktree;
      const bool failed = !result.problem.isEmpty();
      m_app.print(Entry::notice(failed ? NoticeLevel::Warn : NoticeLevel::Info, landingReport(worktree, mode, result, landing->mainPath)));
      m_app.refreshTasks();
      if (failed || mode == LandMode::BranchOnly) {
        return;
      }
      const QString task = worktree.task.isEmpty() ? folderName(worktree.path) : worktree.task;
      QList<Tui::PickerItem> items{
          {"remove", "Remove them", "the work is on " + result.target + " now"},
          {"keep", "Keep them", QString("/drop %1 removes them later").arg(task)},
      };
      m_app.openPicker(std::make_unique<Tui::Picker>(QString("Remove the worktree and %1?").arg(worktree.branch), items),
                       [this, landing](const QString& value) {
                         if (value == "remove") {
                           remove(landing->worktree, landing->mainPath, false);
                         }
                       });
    };
  });
}

void WorktreeCommands::drop(const QString& task) {
  if (const std::optional<Worktree> worktree = find(task)) {
    startDrop(*worktree);
  }
}

void WorktreeCommands::startDrop(const Worktree& worktree) {
  if (refuseIfOpenElsewhere(worktree)) {
    return;
  }
  m_app.withActivity("Reading the changes…", [this, worktree]() -> std::function<void()> {
    const WorktreeChanges changes = worktreeChanges(worktree);
    const QString mainPath = mainCheckout(worktree.path);
    return [this, worktree, changes, mainPath]() {
      QStringList lost;
      if (!changes.files.isEmpty()) {
        lost << plural(changes.files.size(), "changed file");
      }
      if (!changes.pendingCommits.isEmpty()) {
        lost << plural(changes.pendingCommits.size(), "commit") + " that aren't landed";
      }
      QList<Tui::PickerItem> items{
          {"drop", "Drop it", lost.isEmpty() ? QString("nothing in it is unlanded") : "throws away " + lost.join(" and ")},
          {"keep", "Keep it", {}},
      };
      const QString title = QString("Drop %1 and its worktree, %2?").arg(worktree.branch, tildify(worktree.path));
      m_app.openPicker(std::make_unique<Tui::Picker>(title, items, "keep"), [this, worktree, mainPath](const QString& value) {
        if (value == "drop") {
          remove(worktree, mainPath, true);
        }
      });
    };
  });
}

void WorktreeCommands::remove(const Worktree& worktree, const QString& mainPath, bool discard) {
  if (isCurrent(worktree)) {
    // Out of it first: its session, agent and background jobs go with the switch.
    m_app.controller().resumeIn(mainPath, {Entry::notice(NoticeLevel::Info, QString("Back in your checkout, %1.").arg(tildify(mainPath)))});
  }
  m_app.withActivity("Removing the worktree…", [this, worktree, discard]() -> std::function<void()> {
    removeWorktree(worktree, true, discard);
    return [this, worktree]() {
      m_app.print(Entry::notice(NoticeLevel::Info, QString("Removed %1 and %2.").arg(tildify(worktree.path), worktree.branch)));
      m_app.refreshTasks();
    };
  });
}

} // namespace Smith::Ui
