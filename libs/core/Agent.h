#pragma once

#include "core/Cancel.h"
#include "core/Config.h"
#include "core/Events.h"
#include "core/History.h"
#include "core/Permissions.h"
#include "core/Server.h"
#include "core/Tool.h"
#include "core/ToolLog.h"
#include "core/Undo.h"
#include "core/tools/Tools.h"

#include <QJsonArray>
#include <QMap>
#include <QMutex>
#include <QSet>

#include <functional>
#include <memory>
#include <optional>

namespace Smith {

class Sandbox;

// AllowAlways: the tool from now on. AllowSimilar: commands that start like this one (ApprovalRequest::similar).
enum class ApprovalDecision { Allow, AllowAlways, AllowSimilar, Deny };

struct ApprovalRequest {
  QString tool;
  QString summary;
  QJsonObject input;
  // For tools that run in the sandbox: whether it's active, and what it allows.
  bool hasSandbox = false;
  bool sandboxActive = false;
  QString sandboxDescription;
  // For file changes: the diff the call would apply.
  std::optional<Diff> preview;
  // Why a normally prompt-free tool asks this time (e.g. reading outside the project).
  QString note;
  // The call changes a file that runs outside the sandbox later; asked even when the tool is always allowed.
  QString warning;
  // Whether "always allow" is on offer: not for commands that run without a sandbox.
  bool canAllowAlways = true;
  // For a command: the start that "always allow commands like this" would cover ("npm test"); empty when that isn't on offer.
  QString similar;
  // Whether an "always" answer is kept for the project's later sessions, not only this one.
  bool remembered = false;
};

struct UndoResult {
  // The undone turn's message, to put back in the input box.
  QString prompt;
  QStringList restored;
  // Files left alone because they changed after the agent wrote them.
  QStringList skipped;
  // Tools that ran in the turn whose changes can't be undone (bash).
  QStringList untracked;
};

// What the agent changed in files since the conversation started or was loaded.
struct SessionChanges {
  // Files that differ now from before the agent first changed them, by path.
  QList<ChangedFile> files;
  // Tools that ran whose changes can't be seen (bash).
  QStringList untracked;
};

// Starts the message that reminds the model of unfinished todos when it stops early.
extern const QString TODO_REMINDER;

struct AgentOptions {
  ServerPtr server;
  QString model;
  // How hard the model should think; see Reasoning.h for what each level sends.
  QString reasoning = "default";
  QString cwd;
  ToolList tools;
  QString systemPrompt;
  // Sampling temperature; omitted from requests when unset so the server default applies.
  std::optional<double> temperature;
  // The config's settings for a model on an endpoint (Config::settingsFor). Asked for each
  // request, so they follow /model and /endpoint. Unset: no extra fields, no loop detection.
  std::function<ModelSettings(const QString& endpoint, const QString& model)> settingsFor;
  // Cap on model calls per user message, so a confused model can't loop forever.
  int maxSteps = 50;
  // The server's context window in tokens. Enables compaction before it fills up.
  std::optional<int> contextSize;
  // Where shell commands run. Null means no sandbox.
  std::shared_ptr<Sandbox> sandbox;
  // System prompt for sub-agents; when set, the agent gets a `task` tool.
  QString subagentPrompt;
  // System prompt for sub-agents that may change files and run commands; when set, `task` offers that.
  QString workerPrompt;
  // For a sub-agent: where its file changes are recorded (the turn that started it), instead of in turns of its own.
  std::shared_ptr<Checkpoint> checkpoint;
  // For a sub-agent: told of each run-later file written with the user's approval, as the agent that started it reports them.
  std::function<void(const QString& rel, const QString& hash)> onRunLaterApproved;
  std::shared_ptr<AllowedTools> alwaysAllowed;
  // Earlier conversation to continue (without the system prompt).
  Messages history;
  // Called on every change to the conversation, for persistence.
  std::function<void(const HistoryChange&)> onHistory;
  // Asks the user whether a tool may run. Blocks; the frontend must answer Deny if the user interrupts.
  std::function<ApprovalDecision(const ApprovalRequest&)> approve;
  // Asks the user a question for ask_user; without it the tool isn't offered.
  std::function<Answer(const Question&)> ask;
  // Gives the agent the todo_write tool.
  bool todos = true;
  // Starts in plan mode (see Agent::setPlanning).
  bool planning = false;
  // Always ask before the file tools change a file that runs outside the sandbox later, and report other changes after each turn.
  bool protectRunLater = true;
  // The report after each turn; off for a sub-agent, whose changes are in its parent's.
  bool reportRunLater = true;
  // Called from whichever thread the agent (or one of its parallel tools) runs on.
  std::function<void(const AgentEvent&)> onEvent;
  // Called when each tool call is over, whatever the outcome, e.g. for the tool log.
  std::function<void(const ToolRun&)> onToolRun;
};

// The agent loop: calls the model, runs the tools it asks for, feeds the
// results back, and repeats until the model answers without tools. `send`,
// `compactNow` and `undo` block and run on a worker thread; the rest may be
// called from the UI thread at any time.
class Agent {
public:
  explicit Agent(AgentOptions options);

  QString model() const;
  // The context window in tokens, if known.
  std::optional<int> contextLimit() const;
  QString reasoning() const;
  ServerPtr server() const;
  // What runs without asking, and what the user allowed.
  std::shared_ptr<AllowedTools> allowed() const { return m_pAlwaysAllowed; }

  // Switches model, and optionally server, for the following requests. The conversation carries over.
  void setModel(const QString& model, std::optional<int> contextSize, const ServerPtr& server = {});
  void setReasoning(const QString& level);

  // Plan mode: the model may read and search but not change files or run
  // commands; such calls are refused, and each message tells it so. The
  // first message after it is switched off says it's over.
  void setPlanning(bool on);
  bool planning() const;

  // The model's checklist, as last written with todo_write.
  TodoList todos() const;
  // The full conversation, including the system prompt.
  Messages messages() const;

  // Clears the conversation but keeps the system prompt and "always allow" grants.
  void reset();
  // Replaces the conversation with a saved one (switching sessions); not reported to onHistory.
  void loadConversation(const Messages& messages);

  // Queues a message typed while the agent works; handed to the model at its next step.
  void queue(const QString& text);
  QStringList queuedMessages() const;
  // Removes and returns undelivered messages.
  QStringList takeQueued();

  bool canUndo() const;
  // Undoes the last turn: puts back files it changed and removes the turn from the conversation.
  std::optional<UndoResult> undo();

  // Compares the files the agent changed with how they were before its first change. Reads them from disk.
  SessionChanges sessionChanges() const;

  // Runs one user turn, with pictures and files to show the model if any. Throws on API errors; interruption via `cancel` is not an error.
  void send(const QString& text, const CancelPtr& cancel, const QList<Image>& images = {}, const QList<AttachedFile>& files = {});
  // Summarizes the conversation now, regardless of how full the context is.
  void compactNow(const CancelPtr& cancel);
  // One quick reply from the model, outside the conversation: no history, no
  // tools. Think tags are stripped. Throws on API errors.
  QString complete(const QString& instructions, const QString& input, const CancelPtr& cancel);

private:
  struct ToolCall {
    QString id;
    QString name;
    QString args;
  };

  struct StreamedResponse {
    QString text;
    QString reasoning;
    QList<ToolCall> toolCalls;
    QString finishReason;
    // Prompt + completion tokens, when the server reports usage.
    std::optional<qint64> totalTokens;
    qint64 promptTokens = 0;
    qint64 completionTokens = 0;
    // From the first token streamed to the last.
    qint64 generationMs = 0;
    // Stopped partway because it kept repeating itself; the rest is unusable.
    bool looped = false;
  };

  struct Turn {
    int start = 0;
    QString prompt;
    std::shared_ptr<Checkpoint> checkpoint;
  };

  void emitEvent(const AgentEvent& event) const;
  void interruptible(const CancelPtr& cancel, const std::function<void()>& work);
  void loop(const CancelPtr& cancel);
  void dropUnansweredImages(int index);
  void deliverQueued();
  QList<ToolResult> runTools(const QList<ToolCall>& calls, const CancelPtr& cancel);
  ToolResult runSubagent(const TaskInput& input, const CancelPtr& cancel);

  qint64 estimateContext() const;
  void emitContext() const;
  void maybeCompact(const CancelPtr& cancel);
  bool compact(const CancelPtr& cancel, const QString& reason);
  QString summarize(const Messages& messages, int maxChars, const CancelPtr& cancel);

  void append(const Message& message);
  void replaceHistory(const Messages& messages);

  ModelSettings modelSettings() const;
  QJsonObject requestBase(const QString& reasoningLevel) const;
  // `note`, when set, goes to the model with this request only.
  StreamedResponse streamResponse(const CancelPtr& cancel, const QString& note = {});
  static Message assistantMessage(const StreamedResponse& response);

  ToolResult runTool(const ToolCall& call, const CancelPtr& cancel);
  ApprovalDecision requestApproval(const ApprovalRequest& request, const CancelPtr& cancel);
  Answer askUser(const Question& question, const CancelPtr& cancel);
  QString runLaterWarning(const ToolDef& tool, const QJsonObject& args) const;
  void setTodos(const TodoList& todos);
  ToolContext toolContext(const ToolDef& tool, const CancelPtr& cancel);
  void reportRunLater(const QMap<QString, QString>& before);
  // Adds a turn's changes to what sessionChanges compares.
  void absorb(const Checkpoint& checkpoint);
  // Whether a call would change something, which plan mode refuses.
  static bool changes(const ToolDef& tool, const QJsonObject& args);
  const ToolDef* findTool(const QString& name) const;

  AgentOptions m_options;
  ToolList m_tools;
  QJsonArray m_apiTools;
  std::shared_ptr<AllowedTools> m_pAlwaysAllowed;

  // Guards the state the UI thread reads while a turn runs.
  mutable QMutex m_mutex;
  Messages m_history;
  QList<Turn> m_turns;
  QStringList m_queued;
  std::optional<int> m_contextSize;
  ServerPtr m_pServer;
  QString m_model;
  QString m_reasoning;
  // Last exact token count from the server, and how many messages it covered.
  std::optional<QPair<qint64, int>> m_measured;
  TodoList m_todos;
  bool m_planning = false;
  // The last message went out in plan mode, so the next one says it's over.
  bool m_planned = false;
  // For sessionChanges: each file as it was before the agent first changed it, and the tools whose changes can't be seen.
  QMap<QString, std::optional<QByteArray>> m_originals;
  QStringList m_untracked;
  // Run-later files the user approved this turn, with the content they approved: not reported again.
  QMap<QString, QString> m_approvedRunLater;

  // Approvals and questions are asked one at a time, even when tools run in parallel.
  QMutex m_approvalMutex;
};

} // namespace Smith
