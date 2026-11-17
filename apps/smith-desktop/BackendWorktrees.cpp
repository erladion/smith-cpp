#include "Backend.h"

#include "core/Dirs.h"
#include "core/Errors.h"

#include <QCoreApplication>
#include <QFileInfo>

namespace Smith::Desktop {

// ---- task worktrees ----

void Backend::refreshWorktrees() {
  if (!m_pRuntime) {
    return;
  }
  const int generation = ++m_worktreesGeneration;
  const QString cwd = m_controller.project().cwd;
  auto listed = std::make_shared<std::optional<QList<Worktree>>>();
  auto summaries = std::make_shared<QMap<QString, QString>>();
  launch(
      [cwd, listed, summaries]() {
        try {
          *listed = listWorktrees(cwd);
        } catch (const std::exception&) {
          // Not a repository.
          return;
        }
        for (const Worktree& worktree : **listed) {
          if (worktree.main) {
            continue;
          }
          try {
            summaries->insert(worktree.path, changeSummary(worktreeChanges(worktree)));
          } catch (const std::exception& error) {
            summaries->insert(worktree.path, errorMessage(error));
          }
        }
      },
      [this, generation, listed, summaries]() {
        if (generation != m_worktreesGeneration) {
          return;
        }
        m_inRepository = listed->has_value();
        m_worktreeList = listed->value_or(QList<Worktree>{});
        m_worktrees.clear();
        const qint64 self = QCoreApplication::applicationPid();
        for (const Worktree& worktree : m_worktreeList) {
          const QString summary = summaries->value(worktree.path);
          const bool elsewhere = worktree.openBy != 0 && worktree.openBy != self;
          QStringList hint{worktree.branch.isEmpty() ? "detached at " + worktree.head : worktree.branch};
          if (!worktree.main) {
            hint << summary;
          }
          if (elsewhere) {
            hint << "open in another Smith";
          }
          const QString label = worktree.main ? QString("your checkout") : worktree.task.isEmpty() ? QFileInfo(worktree.path).fileName() : worktree.task;
          m_worktrees.append(QVariantMap{
              {"path", worktree.path},
              {"label", label},
              {"hint", hint.join(" · ")},
              {"branch", worktree.branch},
              {"task", worktree.task},
              {"main", worktree.main},
              {"current", isCurrent(worktree)},
              {"openElsewhere", elsewhere},
              {"summary", summary},
          });
        }
        Q_EMIT worktreesChanged();
      });
}

bool Backend::isCurrent(const Worktree& worktree) const {
  const QString cwd = realPath(m_controller.project().cwd);
  return cwd == worktree.path || cwd.startsWith(worktree.path + "/");
}

std::optional<Worktree> Backend::worktreeAt(const QString& path) const {
  for (const Worktree& worktree : m_worktreeList) {
    if (worktree.path == path) {
      return worktree;
    }
  }
  if (m_landingState && m_landingState->worktree.path == path) {
    return m_landingState->worktree;
  }
  return std::nullopt;
}

std::optional<Worktree> Backend::findTask(const QString& task) {
  if (!m_inRepository) {
    notice(NoticeLevel::Error, QString("%1 isn't in a git repository.").arg(tildify(m_controller.project().cwd)));
    return std::nullopt;
  }
  for (const Worktree& worktree : m_worktreeList) {
    if (!worktree.main && (task.isEmpty() ? isCurrent(worktree) : worktree.task == task)) {
      return worktree;
    }
  }
  notice(NoticeLevel::Error, task.isEmpty() ? QString("You're not in a task worktree: name the task (e.g. /land fix-login), or pick one in the sidebar.")
                                            : QString("There's no task \"%1\" here; the sidebar lists them.").arg(task));
  return std::nullopt;
}

void Backend::setLanding(const QVariantMap& landing) {
  m_landing = landing;
  Q_EMIT landingChanged();
}

void Backend::createWorktree(const QString& task) {
  if (!m_controller.agent() || m_controller.turns().busy()) {
    return;
  }
  const QString name = task.trimmed();
  if (name.isEmpty()) {
    notice(NoticeLevel::Error, "Name the task, e.g. /worktree fix-login.");
    return;
  }
  if (!sandboxActive()) {
    notice(NoticeLevel::Error, "Task worktrees need the sandbox: it's what keeps git read-only for the agent. Start Smith without "
                               "--sandbox off (or \"sandbox\": \"off\" in the config) to use them.");
    return;
  }
  const QString cwd = m_controller.project().cwd;
  background("Making the worktree…", [this, cwd, name]() -> std::function<void()> {
    const CreatedWorktree created = createTaskWorktree(cwd, name);
    return [this, created]() {
      const Worktree& worktree = created.worktree;
      if (isCurrent(worktree)) {
        notice(NoticeLevel::Info, QString("You're in %1's worktree already.").arg(worktree.branch));
        return;
      }
      if (created.reused) {
        m_controller.resumeIn(worktree.path, {Entry::notice(NoticeLevel::Info, QString("Back in the task's worktree, on %1.").arg(worktree.branch))});
        return;
      }
      m_controller.newSession(worktree.path, {Entry::notice(NoticeLevel::Info, QString("Made a worktree for the task: %1, on %2 (from %3). The agent's "
                                                                               "changes stay here; git is read-only for it, so it can't commit. "
                                                                               "Land them on your branch when it's done.")
                                                                           .arg(tildify(worktree.path), worktree.branch, created.from))});
    };
  });
}

void Backend::switchToWorktree(const QString& path) {
  if (!m_controller.agent() || m_controller.turns().busy()) {
    return;
  }
  const std::optional<Worktree> worktree = worktreeAt(path);
  if (worktree && !isCurrent(*worktree)) {
    m_controller.resumeIn(worktree->path);
  }
}

void Backend::reviewLanding(const QString& path) {
  if (!m_controller.agent() || m_controller.turns().busy()) {
    return;
  }
  const std::optional<Worktree> found = worktreeAt(path);
  if (!found || found->main) {
    return;
  }
  if (found->openBy != 0 && found->openBy != QCoreApplication::applicationPid()) {
    notice(NoticeLevel::Error, QString("%1 is open in another Smith (pid %2), where the agent may still be working. Land it from there, or quit "
                                       "that one first.")
                                   .arg(found->branch)
                                   .arg(found->openBy));
    return;
  }
  const Worktree worktree = *found;
  // What was asked for helps the model name the commit; only known in the worktree's own session.
  const QString request = isCurrent(worktree) ? firstRequest(m_controller.agent()->messages()) : QString();
  const std::shared_ptr<Agent> agent = m_controller.agent();
  setLanding(QVariantMap{{"stage", "loading"}, {"branch", worktree.branch}});
  background("Reading the changes…", [this, worktree, request, agent]() -> std::function<void()> {
    Landing landing;
    QString message;
    try {
      landing.worktree = worktree;
      landing.changes = worktreeChanges(worktree);
      landing.mainPath = mainCheckout(worktree.path);
      landing.target = currentBranch(landing.mainPath);
      if (!landing.changes.files.isEmpty()) {
        message = suggestCommitMessage(agent, worktree, landing.changes, request, makeCancel());
      }
    } catch (const std::exception& error) {
      const QString text = errorMessage(error);
      return [this, text]() {
        setLanding({});
        notice(NoticeLevel::Error, text);
      };
    }
    return [this, landing, message]() {
      if (landing.changes.isEmpty()) {
        setLanding({});
        notice(NoticeLevel::Info, QString("Nothing to land: %1 has no changes, and %2 has all its commits.")
                                      .arg(landing.worktree.branch, landing.target.isEmpty() ? QString("your checkout") : landing.target));
        return;
      }
      m_landingState = landing;
      QVariantList files;
      for (const Entry& entry : changeEntries(landing.changes)) {
        files.append(QVariantMap{{"name", entry.name}, {"summary", entry.summary}, {"diff", entry.diff ? diffRows(*entry.diff) : QVariantList()}});
      }
      setLanding(QVariantMap{
          {"stage", "review"},
          {"path", landing.worktree.path},
          {"branch", landing.worktree.branch},
          {"task", landing.worktree.task},
          {"target", landing.target},
          {"mainPath", tildify(landing.mainPath)},
          {"summary", changeSummary(landing.changes)},
          {"files", files},
          {"pending", landing.changes.pendingCommits},
          {"message", message},
          {"uncommitted", !landing.changes.files.isEmpty()},
      });
    };
  });
}

void Backend::land(const QString& mode, const QString& message) {
  if (!m_landingState || m_controller.turns().busy()) {
    return;
  }
  const Landing landing = *m_landingState;
  const LandMode landMode = mode == "merge" ? LandMode::Merge : mode == "branch" ? LandMode::BranchOnly : LandMode::CherryPick;
  QVariantMap next = m_landing;
  if (!landing.changes.files.isEmpty() && message.trimmed().isEmpty()) {
    next["error"] = "The commit needs a message.";
    setLanding(next);
    return;
  }
  next.remove("error");
  next["stage"] = "landing";
  setLanding(next);
  const QString text = message.trimmed();
  background("Landing…", [this, landing, landMode, text]() -> std::function<void()> {
    LandResult result;
    try {
      result = landWorktree(landing.worktree, text, landMode);
    } catch (const std::exception& error) {
      const QString failure = errorMessage(error);
      return [this, failure]() {
        m_landingState.reset();
        setLanding({});
        notice(NoticeLevel::Error, failure);
      };
    }
    return [this, landing, landMode, result]() {
      const QString report = landingReport(landing.worktree, landMode, result, landing.mainPath);
      const bool failed = !result.problem.isEmpty();
      notice(failed ? NoticeLevel::Warn : NoticeLevel::Info, report);
      refreshWorktrees();
      if (failed || landMode == LandMode::BranchOnly) {
        m_landingState.reset();
        setLanding({});
        return;
      }
      setLanding(QVariantMap{
          {"stage", "landed"},
          {"path", landing.worktree.path},
          {"branch", landing.worktree.branch},
          {"task", landing.worktree.task},
          {"target", result.target},
          {"report", report},
      });
    };
  });
}

void Backend::closeLanding() {
  if (m_landing.value("stage").toString() == "landing") {
    return;
  }
  m_landingState.reset();
  setLanding({});
}

void Backend::removeWorktree(const QString& path, bool discard) {
  if (!m_controller.agent() || m_controller.turns().busy()) {
    return;
  }
  const std::optional<Worktree> found = worktreeAt(path);
  if (!found || found->main) {
    return;
  }
  if (found->openBy != 0 && found->openBy != QCoreApplication::applicationPid()) {
    notice(NoticeLevel::Error, QString("%1 is open in another Smith (pid %2). Remove it from there, or quit that one first.")
                                   .arg(found->branch)
                                   .arg(found->openBy));
    return;
  }
  const Worktree worktree = *found;
  m_landingState.reset();
  setLanding({});
  QString mainPath;
  try {
    mainPath = mainCheckout(worktree.path);
  } catch (const std::exception& error) {
    notice(NoticeLevel::Error, errorMessage(error));
    return;
  }
  if (isCurrent(worktree)) {
    // Out of it first: its session, agent and background jobs go with the switch.
    m_controller.resumeIn(mainPath, {Entry::notice(NoticeLevel::Info, QString("Back in your checkout, %1.").arg(tildify(mainPath)))});
  }
  background("Removing the worktree…", [this, worktree, discard]() -> std::function<void()> {
    Smith::removeWorktree(worktree, true, discard);
    return [this, worktree]() {
      notice(NoticeLevel::Info, QString("Removed %1 and %2.").arg(tildify(worktree.path), worktree.branch));
      refreshWorktrees();
    };
  });
}

} // namespace Smith::Desktop
