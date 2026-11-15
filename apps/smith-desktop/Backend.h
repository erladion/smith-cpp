#pragma once

#include "frontend/Prompts.h"
#include "frontend/Runtime.h"
#include "frontend/Turns.h"
#include "frontend/Worktrees.h"
#include "Palette.h"
#include "TranscriptModel.h"

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>
#include <optional>

class QQmlEngine;
class QJSEngine;

namespace Smith::Desktop {

// What the QML window works with: startup, the conversation, agent turns,
// approvals and questions, and the session, project, endpoint and model
// controls. Agent turns run on worker threads, as in the TUI; everything
// here is called and changed on the GUI thread. The code is split by section:
// Backend.cpp, BackendProperties.cpp, BackendCommands.cpp, BackendSessions.cpp
// and BackendWorktrees.cpp.
class Backend : public QObject {
  Q_OBJECT
  QML_NAMED_ELEMENT(Backend)
  QML_SINGLETON

  // "starting", "ready" or "failed".
  Q_PROPERTY(QString state READ state NOTIFY changed)
  Q_PROPERTY(QString startupError READ startupError NOTIFY changed)
  Q_PROPERTY(bool matrix READ matrix CONSTANT)
  Q_PROPERTY(bool codeStream READ codeStream CONSTANT)
  Q_PROPERTY(QVariantMap palette READ palette CONSTANT)
  Q_PROPERTY(QStringList spinnerFrames READ spinnerFrames CONSTANT)
  Q_PROPERTY(QStringList reasoningLevels READ reasoningLevels CONSTANT)
  Q_PROPERTY(QString version READ version CONSTANT)
  Q_PROPERTY(TranscriptModel* transcript READ transcript CONSTANT)

  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(bool agentRunning READ agentRunning NOTIFY changed)
  Q_PROPERTY(QString activity READ activity NOTIFY changed)
  Q_PROPERTY(bool compacting READ compacting NOTIFY changed)
  Q_PROPERTY(qint64 turnStartedAt READ turnStartedAt NOTIFY changed)
  Q_PROPERTY(QVariantList todos READ todos NOTIFY changed)
  Q_PROPERTY(QStringList queued READ queued NOTIFY changed)
  Q_PROPERTY(QVariantMap approval READ approval NOTIFY promptChanged)
  Q_PROPERTY(QVariantMap question READ question NOTIFY promptChanged)

  Q_PROPERTY(QString endpoint READ endpoint NOTIFY changed)
  Q_PROPERTY(QString serverLabel READ serverLabel NOTIFY changed)
  Q_PROPERTY(QStringList endpoints READ endpoints NOTIFY changed)
  Q_PROPERTY(QString model READ model NOTIFY changed)
  Q_PROPERTY(QVariantList models READ models NOTIFY changed)
  Q_PROPERTY(QString reasoning READ reasoning NOTIFY changed)
  Q_PROPERTY(qint64 contextTokens READ contextTokens NOTIFY changed)
  Q_PROPERTY(int contextLimit READ contextLimit NOTIFY changed)
  Q_PROPERTY(bool sandboxActive READ sandboxActive NOTIFY changed)
  Q_PROPERTY(QString sandboxKind READ sandboxKind NOTIFY changed)
  Q_PROPERTY(QString sandboxDescription READ sandboxDescription NOTIFY changed)
  Q_PROPERTY(bool network READ network NOTIFY changed)
  Q_PROPERTY(int jobsRunning READ jobsRunning NOTIFY changed)
  Q_PROPERTY(QVariantList jobs READ jobs NOTIFY changed)
  Q_PROPERTY(QString projectPath READ projectPath NOTIFY changed)
  Q_PROPERTY(QString projectName READ projectName NOTIFY changed)
  Q_PROPERTY(QVariantList skills READ skills NOTIFY changed)
  Q_PROPERTY(QString sessionId READ sessionId NOTIFY sessionsChanged)
  Q_PROPERTY(QString sessionName READ sessionName NOTIFY sessionsChanged)
  Q_PROPERTY(bool allSessions READ allSessions WRITE setAllSessions NOTIFY sessionsChanged)
  Q_PROPERTY(QVariantList sessions READ sessions NOTIFY sessionsChanged)
  Q_PROPERTY(QVariantList recentProjects READ recentProjects NOTIFY sessionsChanged)
  Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
  // The branch, when the project is a task worktree.
  Q_PROPERTY(QString worktreeBranch READ worktreeBranch NOTIFY changed)
  // Whether the project is in a git repository (so task worktrees can be made).
  Q_PROPERTY(bool inRepository READ inRepository NOTIFY worktreesChanged)
  // [{path, label, hint, branch, task, main, current, openElsewhere, summary}], the main checkout first.
  Q_PROPERTY(QVariantList worktrees READ worktrees NOTIFY worktreesChanged)
  // The land panel: empty when closed; "stage" is "loading", "review", "landing" or "landed".
  Q_PROPERTY(QVariantMap landing READ landing NOTIFY landingChanged)

public:
  explicit Backend(const LaunchOptions& launch, QObject* parent = nullptr);
  ~Backend() override;

  // For QML_SINGLETON: the one instance main() made.
  static Backend* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
  static void setInstance(Backend* backend);

  QString state() const { return m_state; }
  QString startupError() const { return m_startupError; }
  bool matrix() const { return m_theme == Theme::Matrix; }
  bool codeStream() const { return m_codeStream; }
  QVariantMap palette() const { return m_palette.toVariant(); }
  QStringList spinnerFrames() const;
  QStringList reasoningLevels() const;
  QString version() const;
  TranscriptModel* transcript() { return &m_transcript; }

  bool busy() const { return m_turns.busy(); }
  bool agentRunning() const { return m_turns.agentRunning(); }
  QString activity() const { return m_turns.activity(); }
  bool compacting() const { return m_turns.compacting(); }
  qint64 turnStartedAt() const { return m_turns.startedAt(); }
  QVariantList todos() const;
  QStringList queued() const;
  QVariantMap approval() const;
  QVariantMap question() const;

  QString endpoint() const;
  QString serverLabel() const;
  QStringList endpoints() const;
  QString model() const;
  QVariantList models() const { return m_models; }
  QString reasoning() const;
  qint64 contextTokens() const { return m_turns.tokens().value_or(0); }
  int contextLimit() const { return m_turns.limit().value_or(0); }
  bool sandboxActive() const;
  QString sandboxKind() const;
  QString sandboxDescription() const;
  bool network() const;
  int jobsRunning() const;
  QVariantList jobs() const;
  QString projectPath() const { return m_project.cwd; }
  QString projectName() const;
  QVariantList skills() const;
  QString sessionId() const;
  QString sessionName() const;
  bool allSessions() const { return m_allSessions; }
  void setAllSessions(bool all);
  QVariantList sessions() const { return m_sessions; }
  QVariantList recentProjects() const;
  bool canUndo() const;
  QString worktreeBranch() const { return m_project.worktreeBranch; }
  bool inRepository() const { return m_inRepository; }
  QVariantList worktrees() const { return m_worktrees; }
  QVariantMap landing() const { return m_landing; }

  // Starts up on a worker thread: server, model, sandbox, session.
  Q_INVOKABLE void start();
  // A message for the agent, or a slash command. While the agent works, a message is queued.
  Q_INVOKABLE void submit(const QString& text);
  Q_INVOKABLE void interrupt();
  // "allow", "always", "deny" or "interrupt" (deny and stop the turn).
  Q_INVOKABLE void decide(const QString& decision);
  Q_INVOKABLE void answerOption(int index);
  Q_INVOKABLE void answerText(const QString& text);
  Q_INVOKABLE void dismissQuestion();
  Q_INVOKABLE void setModel(const QString& id);
  Q_INVOKABLE void setReasoning(const QString& level);
  Q_INVOKABLE void setEndpoint(const QString& name);
  Q_INVOKABLE void refreshModels();
  Q_INVOKABLE void setNetwork(bool on);
  Q_INVOKABLE void undo();
  Q_INVOKABLE void compact();
  Q_INVOKABLE void newSession(const QString& folder = {});
  Q_INVOKABLE void openSession(const QString& id);
  Q_INVOKABLE void deleteSession(const QString& id);
  Q_INVOKABLE void renameSession(const QString& name);
  Q_INVOKABLE void refreshSessions();
  // A folder from a dialog (file:// URL) or typed path; starts a new session there.
  Q_INVOKABLE void openProject(const QString& folder);
  Q_INVOKABLE void stopJob(const QString& id);
  // Task worktrees, as /worktree, /worktrees, /land and /drop do in the terminal.
  Q_INVOKABLE void refreshWorktrees();
  Q_INVOKABLE void createWorktree(const QString& task);
  // Continues its newest session, or starts one there.
  Q_INVOKABLE void switchToWorktree(const QString& path);
  // Opens the land panel: the worktree's changes and a suggested commit message.
  Q_INVOKABLE void reviewLanding(const QString& path);
  // "cherry-pick", "merge" or "branch" (commit on the task branch only).
  Q_INVOKABLE void land(const QString& mode, const QString& message);
  Q_INVOKABLE void closeLanding();
  // Removes a worktree and its branch, leaving it first if it's the project; `discard` drops unlanded work.
  Q_INVOKABLE void removeWorktree(const QString& path, bool discard);
  // Completions for a slash command being typed, as in the terminal: [{text, label, hint, takesArgument}].
  Q_INVOKABLE QVariantList suggest(const QString& input) const;
  // The idle stream of glyphs for the status bar.
  Q_INVOKABLE QString codeStreamText(int offset, int width) const;
  // Stops what is running and quits.
  Q_INVOKABLE void quit();

Q_SIGNALS:
  void changed();
  void promptChanged();
  void sessionsChanged();
  // Text to put back in the input box (an undone turn, or queued messages that weren't delivered).
  void restoreInput(const QString& text);
  // A command wants a picker: "model", "endpoint", "reasoning", "sessions", "folder", "jobs", "skills".
  void openPicker(const QString& kind);
  // /redpill.
  void redPill();
  void worktreesChanged();
  void landingChanged();
  // /drop: the window asks before dropping the worktree at `path`.
  void confirmDrop(const QString& path);

private:
  void ready();
  void printBanner(bool resumed, const std::optional<QDateTime>& lastUsed);
  void notice(NoticeLevel level, const QString& text);
  void onEvent(const AgentEvent& event);
  // Runs an agent turn; `then` follows on the GUI thread. A timed turn is one the user sent.
  void runTurn(const QString& activity, std::function<void(const CancelPtr&)> work, std::function<void()> then = {}, bool timed = false);
  // Runs `work` on a worker thread with `activity` showing; its result runs on the GUI thread.
  void background(const QString& activity, std::function<std::function<void()>()> work);
  void runCommand(const QString& text);
  void switchModel(const QString& id);
  void switchEndpoint(const QString& name, const QString& preferredModel = {});
  std::optional<QList<Entry>> enterProject(const QString& folder);
  void startOver(bool resumed, const std::optional<QDateTime>& lastUsed, const QList<Entry>& entries);
  void launch(std::function<void()> work, std::function<void()> finished);
  void startNewSession(const QString& folder, const QList<Entry>& extra);
  void openSessionIn(const QString& id, const QString& folder, const QList<Entry>& extra);
  void resumeIn(const QString& folder, const QList<Entry>& extra = {});
  // The task's worktree from the last listing (the project's own when `task` is empty); notes why not.
  std::optional<Worktree> findTask(const QString& task);
  std::optional<Worktree> worktreeAt(const QString& path) const;
  bool isCurrent(const Worktree& worktree) const;
  void setLanding(const QVariantMap& landing);

  static inline Backend* s_instance = nullptr;

  LaunchOptions m_launch;
  Theme m_theme = Theme::Matrix;
  bool m_codeStream = true;
  Palette m_palette;
  TranscriptModel m_transcript;
  std::shared_ptr<Runtime> m_pRuntime;
  std::shared_ptr<Agent> m_pAgent;
  ServerPtr m_pServer;
  ProjectView m_project;
  SessionControl m_sessionControl;
  AgentHooks m_hooks;

  QString m_state = "starting";
  QString m_startupError;
  // Approvals and questions from the agent, and the turn or task under way.
  Prompts m_prompts;
  TurnRunner m_turns;
  QVariantList m_models;
  bool m_allSessions = false;
  QVariantList m_sessions;
  QList<Worktree> m_worktreeList;
  QVariantList m_worktrees;
  bool m_inRepository = false;
  // Listings finish in any order; only the newest one counts.
  int m_worktreesGeneration = 0;
  // What the land panel is landing.
  struct Landing {
    Worktree worktree;
    WorktreeChanges changes;
    QString mainPath;
    QString target;
  };
  std::optional<Landing> m_landingState;
  QVariantMap m_landing;
  bool m_closed = false;
};

} // namespace Smith::Desktop
