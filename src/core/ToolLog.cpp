#include "core/ToolLog.h"

#include "core/Dirs.h"
#include "core/Session.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>

namespace Smith {

namespace {

// Output beyond this many characters is cut from the log (the session keeps all of it).
constexpr int MAX_OUTPUT = 10000;

} // namespace

QString toolLogDir() {
  return dataDir() + "/logs";
}

QString toolLogFile(const QString& sessionId) {
  return toolLogDir() + "/" + sessionId + ".jsonl";
}

ToolLog::ToolLog(std::function<QString()> sessionId, std::function<QString()> cwd) : m_sessionId(std::move(sessionId)), m_cwd(std::move(cwd)) {}

void ToolLog::write(const ToolRun& run) {
  const QString session = m_sessionId();
  const QString file = toolLogFile(session);
  const QString output = run.output.left(MAX_OUTPUT);
  QJsonObject entry{
      {"time", run.time},
      {"session", session},
      {"cwd", m_cwd()},
      {"tool", run.tool},
      {"input", run.input},
      {"status", run.status},
      {"durationMs", run.durationMs},
      {"output", output},
  };
  if (!run.subagent.isEmpty()) {
    entry["subagent"] = run.subagent;
  }
  if (!run.approval.isEmpty()) {
    entry["approval"] = run.approval;
  }
  if (output.size() < run.output.size()) {
    entry["outputChars"] = run.output.size();
  }

  // A full disk or unwritable folder mustn't stop the agent; the session still has the call.
  QMutexLocker lock(&m_mutex);
  const bool first = !m_secured.contains(file);
  if (first) {
    privateDir(toolLogDir());
  }
  QFile handle(file);
  if (!handle.open(QIODevice::WriteOnly | QIODevice::Append)) {
    return;
  }
  handle.write(QJsonDocument(entry).toJson(QJsonDocument::Compact) + "\n");
  handle.close();
  if (first) {
    privateFile(file);
    m_secured.insert(file);
  }
}

} // namespace Smith
