#include "core/tools/Tools.h"

namespace Smith {

ToolDef taskTool(std::function<ToolResult(const TaskInput&, const ToolContext&)> run, bool workers) {
  ToolDef tool;
  tool.name = "task";
  tool.description = "Delegate a self-contained investigation to a sub-agent that can read files, search and load skills, but not change anything. "
                     "It works in its own context and returns a short report, so use it for searches or questions that would take many file "
                     "reads (e.g. 'find where X is configured and how it is used'). Give it everything it needs in prompt: it can't see this "
                     "conversation. Several task calls in one response run in parallel. For one known file, read it yourself instead.";
  QList<QPair<QString, QJsonObject>> properties = {
      {"description", Schema::string("A few words saying what the sub-agent is doing, shown to the user", 1)},
      {"prompt", Schema::string("The full task: what to find out or do, where to look, and what the report should contain. The sub-agent sees nothing else.",
                                1)},
  };
  if (workers) {
    tool.description += " With can_write, the sub-agent can also change files and run commands: use that to hand over a self-contained piece of "
                        "work (one module, one test file) with its own context. Its changes are the user's to approve, as yours are. Sub-agents "
                        "working at the same time must not touch the same files, or build in the same folder: split the work so they can't.";
    properties.append({"can_write", Schema::boolean("Let the sub-agent change files and run commands, not only read")});
  }
  tool.schema = Schema::object(properties, {"description", "prompt"});
  // Starting one needs no approval: what a sub-agent changes is asked about call by call. So several can run at once.
  tool.readOnly = true;
  tool.summarize = [](const QJsonObject& input) {
    const QString description = input.value("description").toString();
    return input.value("can_write").toBool(false) ? description + " (can change files)" : description;
  };
  tool.run = [run, workers](const QJsonObject& input, const ToolContext& ctx) {
    return run(TaskInput{input.value("description").toString(), input.value("prompt").toString(), workers && input.value("can_write").toBool(false)}, ctx);
  };
  return tool;
}

} // namespace Smith
