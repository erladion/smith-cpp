#pragma once

#include "core/Cancel.h"
#include "core/Diff.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

namespace Smith {

class Sandbox;

struct ToolContext {
  // Root directory the agent works in; file tools are confined to it.
  QString cwd;
  // Cancelled when the user interrupts; long-running tools should stop.
  CancelPtr cancel;
  // Where shell commands run. Null means no sandbox.
  std::shared_ptr<Sandbox> sandbox;
  // File tools call these around each write, so the turn can be undone.
  std::function<void(const QString& file)> beforeWrite;
  std::function<void(const QString& file, const QByteArray& content)> afterWrite;

  // The sandbox, or a "none" sandbox when there is no sandbox.
  std::shared_ptr<Sandbox> sandboxOrNone() const;
};

struct ToolResult {
  // What the model sees.
  QString content;
  bool isError = false;
  // For file changes: the diff, shown to the user instead of `content`.
  std::optional<Diff> diff;

  static ToolResult error(const QString& content) { return ToolResult{content, true, std::nullopt}; }
};

// A tool the model can call. Inputs are validated against `schema` (JSON
// Schema) before any of the functions see them. Functions report failure
// by throwing SmithError.
struct ToolDef {
  QString name;
  QString description;
  QJsonObject schema;
  // Read-only tools run without asking the user.
  bool readOnly = false;
  // Whether sub-agents get this tool; defaults to `readOnly`.
  std::optional<bool> subagents;
  // Runs inside the sandbox when one is active (shown in the approval prompt).
  bool sandboxed = false;
  // What a call gets beyond what commands get, for the approval prompt: "the network, JIRA_TOKEN".
  QString extraAccess;
  // One-line description of a call, shown in the transcript and approval prompt.
  std::function<QString(const QJsonObject&)> summarize;
  // For read-only tools: why this call needs approval anyway (e.g. it reads outside the project).
  std::function<std::optional<QString>(const QJsonObject&, const ToolContext&)> approvalReason;
  // The change the call would make, for the approval prompt. Throwing means the call would fail.
  std::function<Diff(const QJsonObject&, const ToolContext&)> preview;
  // Files the call would write, as given (relative to the project), for the run-later check.
  std::function<QStringList(const QJsonObject&)> touches;
  std::function<ToolResult(const QJsonObject&, const ToolContext&)> run;

  bool forSubagents() const { return subagents.value_or(readOnly); }
};

using ToolList = QList<ToolDef>;

// Builders for the JSON Schema of tool parameters.
namespace Schema {

QJsonObject string(const QString& description, int minLength = 0);
QJsonObject integer(const QString& description, std::optional<int> minimum = std::nullopt, std::optional<int> maximum = std::nullopt);
QJsonObject number(const QString& description);
QJsonObject boolean(const QString& description);
QJsonObject enumeration(const QStringList& values, const QString& description = {});
QJsonObject array(const QJsonObject& items, const QString& description, std::optional<int> maxItems = std::nullopt);
// Properties in order; `required` lists the ones that must be present.
QJsonObject object(const QList<QPair<QString, QJsonObject>>& properties, const QStringList& required);

} // namespace Schema

// Checks a value against a schema made with the builders; returns the problems, or nullopt if valid.
std::optional<QString> validateSchema(const QJsonValue& value, const QJsonObject& schema);

// The OpenAI chat-completions `tools` entry for a tool.
QJsonObject toApiTool(const ToolDef& tool);

// One-line description of a recorded call, for replaying a saved session.
QString describeCall(const ToolList& tools, const QString& name, const QString& args);

// Keeps the head and tail of long output so the model sees both ends.
QString truncateText(const QString& text, int maxChars);

// Like truncateText, but keeps lines that look like errors from the part that would be dropped.
QString truncateLog(const QString& text, int maxChars);

QString plural(int count, const QString& singular, const QString& pluralForm = {});

} // namespace Smith
