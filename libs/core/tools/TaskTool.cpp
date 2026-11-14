#include "core/tools/Tools.h"

namespace Smith {

ToolDef taskTool(std::function<ToolResult(const TaskInput&, const ToolContext&)> run) {
  ToolDef tool;
  tool.name = "task";
  tool.description = "Delegate a self-contained investigation to a sub-agent that can read files, search and load skills, but not change anything. "
                     "It works in its own context and returns a short report, so use it for searches or questions that would take many file "
                     "reads (e.g. 'find where X is configured and how it is used'). Give it everything it needs in prompt: it can't see this "
                     "conversation. Several task calls in one response run in parallel. For one known file, read it yourself instead.";
  tool.schema = Schema::object(
      {
          {"description", Schema::string("A few words saying what the sub-agent is doing, shown to the user", 1)},
          {"prompt", Schema::string("The full task: what to find out, where to look, and what the report should contain. The sub-agent sees nothing else.",
                                    1)},
      },
      {"description", "prompt"});
  // Sub-agents can't change anything, so several can run at once without asking.
  tool.readOnly = true;
  tool.summarize = [](const QJsonObject& input) { return input.value("description").toString(); };
  tool.run = [run](const QJsonObject& input, const ToolContext& ctx) {
    return run(TaskInput{input.value("description").toString(), input.value("prompt").toString()}, ctx);
  };
  return tool;
}

} // namespace Smith
