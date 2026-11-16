#pragma once

#include "core/Agent.h"
#include "core/Cancel.h"
#include "core/Events.h"
#include "frontend/Entry.h"

#include <QElapsedTimer>
#include <QList>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QThread>

#include <functional>
#include <optional>

namespace Smith {

// The tokens a turn's model calls used, as the server reported them.
struct TurnUsage {
  int calls = 0;
  // Summed over the calls: each one sends the whole conversation again.
  qint64 promptTokens = 0;
  qint64 completionTokens = 0;
  // For the speed: the tokens of the calls that were timed, and how long they streamed.
  qint64 timedTokens = 0;
  qint64 generationMs = 0;

  // Tokens generated per second, when there's enough to tell.
  std::optional<double> tokensPerSecond() const;
};

// "12k in · 845 out · 34 tok/s"; empty when the server reported nothing.
QString usageText(const TurnUsage& usage);

// How a turn went, for the line under it.
struct TurnStats {
  QString reason;
  qint64 elapsedMs = 0;
  int toolCalls = 0;
  TurnUsage usage;
};

// Runs agent turns and other slow work on worker threads for a frontend, and
// keeps what the frontend shows meanwhile: whether it is busy, the status
// line's activity, the tools in flight and how full the context is. Lives on
// the UI thread; callbacks given to it run there too.
class TurnRunner : public QObject {
  Q_OBJECT

public:
  explicit TurnRunner(QObject* pParent = nullptr);

  // Something is running; the frontend takes no new work meanwhile.
  bool busy() const { return m_busy; }
  // What runs is an agent turn, which can be interrupted and takes queued messages.
  bool agentRunning() const { return m_busy && m_pCancel != nullptr; }
  const QString& activity() const { return m_activity; }
  void setActivity(const QString& activity) { m_activity = activity; }
  bool compacting() const { return m_compacting; }
  void setCompacting(bool on) { m_compacting = on; }
  int runningTools() const { return m_running.size(); }

  // How full the context is, once the agent has said.
  std::optional<qint64> tokens() const { return m_tokens; }
  std::optional<int> limit() const { return m_limit; }
  void forgetContext();

  // For a timed turn under way: when it started (ms since the epoch, else 0) and how long ago.
  qint64 startedAt() const { return m_startedAt; }
  std::optional<qint64> elapsed() const;
  // The last timed turn, once it is over.
  const TurnStats& lastTurn() const { return m_last; }
  // Tokens used by the turn under way, or else by the last one.
  const TurnUsage& usage() const { return m_timed ? m_usage : m_last.usage; }

  // Takes in an agent event. Returns the transcript entry it adds, if any.
  std::optional<Entry> apply(const AgentEvent& event);

  // An agent turn: `work` runs on a thread, with a token that `interrupt`
  // cancels. `finished` gets what went wrong, described for the user (empty
  // if nothing did). A timed turn is one the user sent; it gets TurnStats.
  void runTurn(const QString& activity, const QString& baseUrl, std::function<void(const CancelPtr&)> work,
               std::function<void(const QString& error)> finished, bool timed = false);
  // Other slow work, which can't be interrupted. `work` returns what to do
  // next on the UI thread; if it throws, `failed` gets the error instead.
  void runTask(const QString& activity, const QString& baseUrl, std::function<std::function<void()>()> work,
               std::function<void(const QString& error)> failed);
  // A thread of its own for `work`, outside `busy`; `finished` then runs on the UI thread.
  void launch(std::function<void()> work, std::function<void()> finished = {});

  // Stops the agent turn under way at its next check.
  void interrupt();
  // The turn under way was interrupted and hasn't ended yet.
  bool interrupted() const { return cancelled(m_pCancel); }
  // Cancelled by `close`. Give it to slow work started with `runTask` or
  // `launch` (a request to the server), so closing doesn't wait for it.
  const CancelPtr& closing() const { return m_pClosing; }
  // Interrupts, then waits for every thread. Nothing given to this object is called afterwards.
  void close();

Q_SIGNALS:
  // Work started or ended.
  void changed();

private:
  QString runningLabel() const;

  bool m_busy = false;
  CancelPtr m_pCancel;
  QString m_activity;
  bool m_compacting = false;
  // Tools running right now (several when read-only tools run in parallel), by call id.
  QMap<QString, QString> m_running;
  QStringList m_runningOrder;
  std::optional<qint64> m_tokens;
  std::optional<int> m_limit;

  bool m_timed = false;
  QElapsedTimer m_clock;
  qint64 m_startedAt = 0;
  int m_toolCalls = 0;
  QString m_reason;
  TurnUsage m_usage;
  TurnStats m_last;

  QList<QThread*> m_workers;
  CancelPtr m_pClosing = makeCancel();
  bool m_closed = false;
};

// /diff: a line saying what changed, then each changed file with its diff, paths relative to `cwd`.
QList<Entry> changesEntries(const SessionChanges& changes, const QString& cwd);

// What /undo did, for the transcript; paths are shown relative to `cwd`.
Entry undoNotice(const UndoResult& result, const QString& cwd);

} // namespace Smith
