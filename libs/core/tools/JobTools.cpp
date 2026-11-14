#include "core/tools/Tools.h"

#include "core/Errors.h"
#include "core/Jobs.h"

#include <cmath>

namespace Smith {

namespace {

constexpr int MAX_OUTPUT_CHARS = 20000;
constexpr int MAX_WAIT_MS = 120000;

} // namespace

ToolList jobTools(const std::shared_ptr<BackgroundJobs>& jobs) {
  ToolDef output;
  output.name = "bash_output";
  output.description = "Read what a background job (started with bash's run_in_background) printed since you last read it, and whether it's still "
                       "running. wait_ms waits up to that long for the job to end first; with until, it stops waiting as soon as new output "
                       "matches that regular expression (e.g. \"listening on|ready\").";
  output.schema = Schema::object(
      {
          {"id", Schema::string(R"(The job id, e.g. "bg1")", 1)},
          {"wait_ms", Schema::integer("How long to wait, in milliseconds", 0, MAX_WAIT_MS)},
          {"until", Schema::string("Stop waiting when new output matches this regular expression", 1)},
      },
      {"id"});
  output.readOnly = true;
  output.summarize = [](const QJsonObject& input) {
    QStringList parts{input.value("id").toString()};
    const int waitMs = input.value("wait_ms").toInt(0);
    if (waitMs) {
      parts << QString("waiting up to %1s").arg(std::lround(waitMs / 1000.0));
    }
    if (!input.value("until").toString().isEmpty()) {
      parts << "until /" + input.value("until").toString() + "/";
    }
    return parts.join(", ");
  };
  output.run = [jobs](const QJsonObject& input, const ToolContext& ctx) {
    const QString id = input.value("id").toString();
    const QString until = input.value("until").toString();
    std::optional<QRegularExpression> pattern;
    if (!until.isEmpty()) {
      QRegularExpression regex(until, QRegularExpression::MultilineOption);
      if (!regex.isValid()) {
        return ToolResult::error("until is not a valid regular expression: " + regex.errorString());
      }
      pattern = regex;
    }
    const BackgroundJobs::ReadResult read = jobs->read(id, input.value("wait_ms").toInt(0), pattern, ctx.cancel);
    const QString lost = read.dropped ? QString("[%1 earlier characters were dropped]\n").arg(read.dropped) : QString();
    QString trimmed = read.output;
    while (!trimmed.isEmpty() && trimmed.back().isSpace()) {
      trimmed.chop(1);
    }
    const QString text = truncateLog(trimmed, MAX_OUTPUT_CHARS);
    const QString body = text.isEmpty() ? lost + "(no new output)" : lost + text;
    QString waited;
    if (pattern && read.status.running) {
      waited = read.matched ? "; the output matched /" + until + "/" : "; nothing matched /" + until + "/ yet";
    }
    return ToolResult{body + "\n\n[Job " + id + ": " + describeJob(read.status) + waited + "]"};
  };

  ToolDef kill;
  kill.name = "bash_kill";
  kill.description = "Stop a background job and everything it started. Stop the jobs you started once you no longer need them.";
  kill.schema = Schema::object({{"id", Schema::string(R"(The job id, e.g. "bg1")", 1)}}, {"id"});
  // It only stops what the agent itself started, so it doesn't ask.
  kill.readOnly = true;
  // Sub-agents can read a job's output, but not stop it.
  kill.subagents = false;
  kill.summarize = [](const QJsonObject& input) { return input.value("id").toString(); };
  kill.run = [jobs](const QJsonObject& input, const ToolContext&) {
    const QString id = input.value("id").toString();
    for (const JobStatus& job : jobs->list()) {
      if (job.id == id && !job.running) {
        return ToolResult{QString("Job %1 had already ended (%2).").arg(id, describeJob(job))};
      }
    }
    const JobStatus status = jobs->stop(id);
    return ToolResult{QString("Stopped job %1 (%2).").arg(id, describeJob(status))};
  };

  return {output, kill};
}

ToolList defaultTools(const std::shared_ptr<BackgroundJobs>& jobs) {
  ToolList tools{readFileTool(), listFilesTool(), grepTool(), globTool(), writeFileTool(), editFileTool(), bashTool(jobs)};
  if (jobs) {
    tools << jobTools(jobs);
  }
  return tools;
}

} // namespace Smith
