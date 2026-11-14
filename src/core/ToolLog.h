#pragma once

#include <QJsonValue>
#include <QMutex>
#include <QSet>
#include <QString>

#include <functional>

namespace Smith {

// One tool call, as the agent reports it when the call is over.
struct ToolRun {
  // When the call started (ISO 8601).
  QString time;
  QString tool;
  // The arguments as the model sent them, parsed when they were valid JSON.
  QJsonValue input;
  // "not needed", "already allowed", "allowed", "allowed always", "denied";
  // empty when the call failed before approval was considered.
  QString approval;
  // "ok", "error", "denied" or "invalid".
  QString status;
  // How long the tool itself ran; waiting for approval isn't counted.
  qint64 durationMs = 0;
  QString output;
  // The sub-agent that made the call, for calls made inside the task tool.
  QString subagent;
};

QString toolLogDir();
QString toolLogFile(const QString& sessionId);

// Appends every tool call to logs/<session id>.jsonl, readable by you only.
// The session id is looked up per call, since the TUI can switch sessions.
class ToolLog {
public:
  ToolLog(std::function<QString()> sessionId, std::function<QString()> cwd);

  void write(const ToolRun& run);

private:
  std::function<QString()> m_sessionId;
  std::function<QString()> m_cwd;
  QMutex m_mutex;
  QSet<QString> m_secured;
};

} // namespace Smith
