#include "core/tools/Tools.h"
#include "compat/Utf8.h"

#include "core/Errors.h"
#include "core/Jobs.h"
#include "core/Sandbox.h"

#include <QThread>

namespace Smith {

namespace {

constexpr int DEFAULT_TIMEOUT_MS = 300000;
constexpr int MAX_TIMEOUT_MS = 600000;
constexpr int MAX_OUTPUT_CHARS = 20000;
// Stop buffering far past what we would show, so a runaway command can't eat memory.
constexpr qint64 MAX_CAPTURE_CHARS = 1000000;

const QString DESCRIPTION = QString("Run a shell command with bash in the working directory and return its combined stdout and stderr. Commands run "
                                    "non-interactively with no stdin, so avoid commands that prompt for input. Times out after %1s unless "
                                    "timeout_ms is set.")
                                .arg(DEFAULT_TIMEOUT_MS / 1000);

QString trimEnd(QString text) {
  while (!text.isEmpty() && text.back().isSpace()) {
    text.chop(1);
  }
  return text;
}

// Runs a command to completion (or its timeout) in the sandbox.
ToolResult runCommand(const QJsonObject& input, const ToolContext& ctx) {
  const QString command = input.value("command").toString();
  const int timeoutMs = input.value("timeout_ms").toInt(DEFAULT_TIMEOUT_MS);
  const auto sandbox = ctx.sandboxOrNone();
  SpawnSpec spec;
  try {
    spec = sandbox->spawnSpec(command, ctx.cwd);
  } catch (const SmithError& error) {
    // E.g. the container couldn't start.
    return ToolResult::error(error.message());
  }
  RunOptions options;
  options.cwd = ctx.cwd;
  options.mergeChannels = true;
  options.timeoutMs = timeoutMs;
  options.cancel = ctx.cancel;
  options.maxCapture = MAX_CAPTURE_CHARS;
  const ProcessResult result = runProcess(spec, options);
  if (!result.started) {
    return ToolResult::error(QString("Failed to start %1: %2").arg(spec.file, result.startError));
  }
  const QString output = utf8(result.out);
  QString body = truncateLog(trimEnd(output), MAX_OUTPUT_CHARS);
  if (body.isEmpty()) {
    body = "(no output)";
  }
  if (result.stoppedBy == ProcessResult::Stop::Timeout) {
    return ToolResult::error(body + QString("\n\n[Command timed out after %1ms and was killed]").arg(timeoutMs));
  }
  if (result.stoppedBy == ProcessResult::Stop::Interrupt) {
    return ToolResult::error(body + "\n\n[Command interrupted by the user]");
  }
  if (result.exitCode != 0) {
    // When it looks like the sandbox's doing, say so, or a model may "fix" code that was fine.
    const auto note = sandbox->explainFailure(output, ctx.cwd);
    const QString code = result.crashed ? QString("killed by a signal") : QString::number(result.exitCode);
    return ToolResult::error(body + "\n\n[Exit code " + code + "]" + (note ? "\n" + *note : QString()));
  }
  return ToolResult{body};
}

QJsonObject commandProperties(bool background) {
  QList<QPair<QString, QJsonObject>> properties = {
      {"command", Schema::string("The command to run", 1)},
      {"timeout_ms", Schema::integer("Timeout in milliseconds", 1, MAX_TIMEOUT_MS)},
  };
  if (background) {
    properties.append({"run_in_background",
                       Schema::boolean("Start it and return at once, for servers, watchers and long builds; read its output with bash_output")});
  }
  return Schema::object(properties, {"command"});
}

} // namespace

ToolDef bashTool(const std::shared_ptr<BackgroundJobs>& jobs) {
  ToolDef tool;
  tool.name = "bash";
  tool.readOnly = false;
  tool.sandboxed = true;
  if (!jobs) {
    tool.description = DESCRIPTION;
    tool.schema = commandProperties(false);
    tool.summarize = [](const QJsonObject& input) { return input.value("command").toString(); };
    tool.run = runCommand;
    return tool;
  }
  tool.description = DESCRIPTION +
                     " Set run_in_background for commands that keep running (dev servers, watchers) or take longer than the timeout; it returns "
                     "a job id at once. Don't append & yourself.";
  tool.schema = commandProperties(true);
  tool.summarize = [](const QJsonObject& input) {
    const QString command = input.value("command").toString();
    return input.value("run_in_background").toBool(false) ? command + " (in the background)" : command;
  };
  tool.run = [jobs](const QJsonObject& input, const ToolContext& ctx) {
    if (!input.value("run_in_background").toBool(false)) {
      return runCommand(input, ctx);
    }
    const auto sandbox = ctx.sandboxOrNone();
    SpawnSpec spec;
    try {
      spec = sandbox->spawnSpec(input.value("command").toString(), ctx.cwd);
    } catch (const SmithError& error) {
      return ToolResult::error(error.message());
    }
    const JobStatus job = jobs->start(input.value("command").toString(), spec, ctx.cwd);
    // A moment to catch commands that fail at once (a typo, a port in use).
    // A peek: what it printed stays unread, so bash_output (and its until) still sees it.
    const BackgroundJobs::ReadResult read = jobs->read(job.id, 1000, std::nullopt, ctx.cancel, true);
    const QString shown = truncateLog(trimEnd(read.output), MAX_OUTPUT_CHARS);
    if (!read.status.running) {
      const bool failed = read.status.exitCode != 0;
      const auto note = failed ? sandbox->explainFailure(read.output, ctx.cwd) : std::nullopt;
      const QString body = QString("Background job %1 %2 right away.").arg(job.id, describeJob(read.status)) +
                           (shown.isEmpty() ? QString() : "\n\n" + shown) + (note ? "\n" + *note : QString());
      return ToolResult{body, failed, std::nullopt};
    }
    const QString head = QString("Started background job %1. Read its output with bash_output (id \"%1\"; wait_ms and until wait for it), and stop "
                                 "it with bash_kill when you're done.")
                             .arg(job.id);
    return ToolResult{shown.isEmpty() ? head : head + "\n\nOutput so far:\n" + shown};
  };
  return tool;
}

} // namespace Smith
