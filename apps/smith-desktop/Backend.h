#pragma once

#include "frontend/Runtime.h"
#include "Palette.h"
#include "TranscriptModel.h"

#include <QElapsedTimer>
#include <QMap>
#include <QObject>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class QQmlEngine;
class QJSEngine;

namespace Smith::Desktop {

// What the QML window works with: startup, the conversation, agent turns,
// approvals and questions, and the session, project, endpoint and model
// controls. Agent turns run on worker threads, as in the TUI; everything
// here is called and changed on the GUI thread.
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

  bool busy() const { return m_busy; }
  bool agentRunning() const { return m_busy && m_pCancel != nullptr; }
  QString activity() const { return m_activity; }
  bool compacting() const { return m_compacting; }
  qint64 turnStartedAt() const { return m_turnStartedAt; }
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
  qint64 contextTokens() const { return m_contextTokens; }
  int contextLimit() const { return m_contextLimit; }
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

private:
  struct PendingApproval {
    ApprovalRequest request;
    std::shared_ptr<Pending<ApprovalDecision>> answer;
  };
  struct PendingQuestion {
    Question question;
    std::shared_ptr<Pending<Answer>> answer;
  };

  void ready();
  void printBanner(bool resumed, const std::optional<QDateTime>& lastUsed);
  void notice(NoticeLevel level, const QString& text);
  void onEvent(const AgentEvent& event);
  ApprovalDecision approveFromWorker(const ApprovalRequest& request);
  Answer askFromWorker(const Question& question);
  void runTurn(const QString& activity, std::function<void(const CancelPtr&)> work, std::function<void()> then = {});
  // Runs `work` on a worker thread with `activity` showing; its result runs on the GUI thread.
  void background(const QString& activity, std::function<std::function<void()>()> work);
  void runCommand(const QString& text);
  void switchModel(const QString& id);
  void switchEndpoint(const QString& name, const QString& preferredModel = {});
  std::optional<QList<Entry>> enterProject(const QString& folder);
  void startOver(bool resumed, const std::optional<QDateTime>& lastUsed, const QList<Entry>& entries);
  QThread* launch(std::function<void()> work, std::function<void()> finished);
  void setCompacting(bool on);

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
  bool m_busy = false;
  CancelPtr m_pCancel;
  QString m_activity;
  bool m_compacting = false;
  qint64 m_turnStartedAt = 0;
  QElapsedTimer m_turnClock;
  int m_turnToolCalls = 0;
  QString m_turnReason;
  QMap<QString, QString> m_running;
  QStringList m_runningOrder;
  std::optional<PendingApproval> m_approval;
  std::optional<PendingQuestion> m_question;
  QVariantList m_models;
  qint64 m_contextTokens = 0;
  int m_contextLimit = 0;
  bool m_allSessions = false;
  QVariantList m_sessions;
  QList<QThread*> m_workers;
  bool m_closed = false;
  std::atomic<bool> m_closing{false};
};

} // namespace Smith::Desktop
