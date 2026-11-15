#pragma once

#include "core/Skills.h"
#include "core/Tool.h"

#include <functional>
#include <memory>

namespace Smith {

class BackgroundJobs;

// Shown as the approval reason when a read-only tool reads outside the project.
inline const QString OUTSIDE = QStringLiteral("Reads outside the project folder.");

ToolDef readFileTool();
ToolDef writeFileTool();
ToolDef editFileTool();
ToolDef listFilesTool();
ToolDef grepTool();
ToolDef globTool();

// `bash`; with `jobs`, it can also start commands in the background.
ToolDef bashTool(const std::shared_ptr<BackgroundJobs>& jobs = {});
// bash_output and bash_kill, for commands bash started in the background.
ToolList jobTools(const std::shared_ptr<BackgroundJobs>& jobs);

// The `skill` tool, for the skills found at startup.
ToolDef skillTool(const QList<Skill>& skills);

// The file, search and shell tools; bash can run commands in the background as `jobs` when given.
ToolList defaultTools(const std::shared_ptr<BackgroundJobs>& jobs = {});

// The `task` tool: hands an investigation to a sub-agent and returns its report.
struct TaskInput {
  QString description;
  QString prompt;
};
ToolDef taskTool(std::function<ToolResult(const TaskInput&, const ToolContext&)> run);

// The `ask_user` tool.
struct Question {
  QString question;
  // Suggested answers; the user can also type their own.
  QStringList options;
};

// The user's answer: one of the options (by index) or their own words; Dismissed if they dismissed it.
struct Answer {
  enum class Kind { Dismissed, Option, Text };
  Kind kind = Kind::Dismissed;
  int option = 0;
  QString text;
};

inline const QString ASK_TOOL = QStringLiteral("ask_user");

ToolDef askTool(std::function<Answer(const Question&, const ToolContext&)> ask);

} // namespace Smith
