#include "core/Agent.h"

#include "core/Dirs.h"
#include "core/Errors.h"
#include "core/Json.h"
#include "core/RunLater.h"
#include "core/Sandbox.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <vector>

namespace Smith {

// Running the tools the model asks for: in parallel when they only read,
// after asking when they don't, and as sub-agents for `task`.

namespace {

constexpr int MAX_REPORT_CHARS = 8000;
// Read-only tool calls running at once, so a burst of greps can't swamp the machine.
constexpr int MAX_PARALLEL_TOOLS = 8;

QString relativeSlashPath(const QString& cwd, const QString& path) {
  return QDir(cwd).relativeFilePath(QDir::cleanPath(QDir(cwd).absoluteFilePath(path)));
}

} // namespace

QList<ToolResult> Agent::runTools(const QList<ToolCall>& calls, const CancelPtr& cancel) {
  QList<ToolResult> results;
  const ToolResult interrupted = ToolResult::error("Interrupted by the user before this tool ran.");
  // Questions to the user run on their own, so they're asked in the order the model wrote them.
  auto readOnly = [&](const ToolCall& call) {
    const ToolDef* tool = findTool(call.name);
    return tool && tool->readOnly && tool->name != ASK_TOOL;
  };
  int i = 0;
  while (i < calls.size()) {
    if (cancelled(cancel)) {
      results.append(interrupted);
      i++;
      continue;
    }
    int end = i + 1;
    if (readOnly(calls[i])) {
      while (end < calls.size() && readOnly(calls[end])) {
        end++;
      }
    }
    if (end - i == 1) {
      results.append(runTool(calls[i], cancel));
    } else {
      // Consecutive read-only calls run in parallel: a few threads take them in turn.
      std::vector<ToolResult> group(end - i);
      std::atomic<int> next{i};
      QList<QThread*> threads;
      for (int n = std::min(end - i, MAX_PARALLEL_TOOLS); n > 0; n--) {
        QThread* thread = QThread::create([this, &group, &calls, &next, &interrupted, i, end, cancel]() {
          for (int j = next.fetch_add(1); j < end; j = next.fetch_add(1)) {
            group[j - i] = cancelled(cancel) ? interrupted : runTool(calls[j], cancel);
          }
        });
        threads.append(thread);
        thread->start();
      }
      for (QThread* thread : threads) {
        thread->wait();
        delete thread;
      }
      for (const ToolResult& result : group) {
        results.append(result);
      }
    }
    i = end;
  }
  return results;
}

ToolResult Agent::runSubagent(const TaskInput& input, const CancelPtr& cancel) {
  std::optional<DoneReason> reason;
  AgentOptions child;
  child.server = server();
  child.model = model();
  child.reasoning = reasoning();
  child.cwd = m_options.cwd;
  child.temperature = m_options.temperature;
  child.settingsFor = m_options.settingsFor;
  child.maxSteps = m_options.maxSteps;
  child.contextSize = contextLimit();
  child.sandbox = m_options.sandbox;
  // Read-only tools only, unless it was sent to do work. No nested sub-agents, checklist or questions either way.
  for (const ToolDef& tool : m_options.tools) {
    if (input.write || tool.forSubagents()) {
      child.tools.append(tool);
    }
  }
  child.todos = false;
  child.protectRunLater = input.write && m_options.protectRunLater;
  child.reportRunLater = false;
  child.systemPrompt = input.write ? m_options.workerPrompt : m_options.subagentPrompt;
  if (input.write) {
    // What it changes is part of this turn: undone with it, and reported with it.
    QMutexLocker lock(&m_mutex);
    if (!m_turns.isEmpty()) {
      child.checkpoint = m_turns.last().checkpoint;
    }
    child.onRunLaterApproved = [this](const QString& rel, const QString& hash) {
      QMutexLocker lock(&m_mutex);
      m_approvedRunLater.insert(rel, hash);
    };
  }
  child.alwaysAllowed = m_pAlwaysAllowed;
  const QString description = input.description;
  // Whatever needs asking, from reading outside the project to its changes, is asked through this agent.
  child.approve = [this, description, cancel](const ApprovalRequest& request) {
    ApprovalRequest forwarded = request;
    forwarded.note = QString("Sub-agent \"%1\": %2").arg(description, request.note).trimmed();
    return requestApproval(forwarded, cancel);
  };
  child.onToolRun = [this, description](const ToolRun& run) {
    if (m_options.onToolRun) {
      ToolRun tagged = run;
      tagged.subagent = description;
      m_options.onToolRun(tagged);
    }
  };
  child.onEvent = [this, description, &reason](const AgentEvent& event) {
    if (const auto* start = std::get_if<Event::ToolStart>(&event)) {
      emitEvent(Event::Activity{QString("%1 › %2 %3").arg(description, start->name, start->summary)});
    } else if (const auto* end = std::get_if<Event::ToolEnd>(&event)) {
      // What it changed (or tried to) is shown like the agent's own changes; its reading stays out of the transcript.
      const ToolDef* tool = findTool(end->name);
      if (tool && !tool->readOnly) {
        Event::ToolEnd shown = *end;
        shown.id = description + "/" + end->id;
        shown.summary = QString("%1 › %2").arg(description, end->summary);
        emitEvent(shown);
      }
    } else if (const auto* done = std::get_if<Event::Done>(&event)) {
      reason = done->reason;
    } else if (std::holds_alternative<Event::Usage>(event)) {
      // Its tokens count with the turn's.
      emitEvent(event);
    }
  };
  Agent agent(child);
  emitEvent(Event::Activity{description + " › thinking…"});
  try {
    agent.send(input.prompt, cancel);
  } catch (const std::exception& error) {
    return ToolResult::error("The sub-agent failed: " + errorMessage(error));
  }
  if (reason == DoneReason::Interrupted) {
    return ToolResult::error("The sub-agent was interrupted by the user.");
  }

  QString report = "The sub-agent finished without a report.";
  const Messages history = agent.messages();
  for (int i = history.size() - 1; i >= 0; i--) {
    if (messageRole(history[i]) == "assistant" && !messageText(history[i]).trimmed().isEmpty()) {
      report = messageText(history[i]).trimmed();
      break;
    }
  }
  if (reason == DoneReason::MaxSteps) {
    report += "\n\n[The sub-agent hit its step limit before finishing; this report may be incomplete.]";
  }
  return ToolResult{truncateText(report, MAX_REPORT_CHARS)};
}

const ToolDef* Agent::findTool(const QString& name) const {
  for (const ToolDef& tool : m_tools) {
    if (tool.name == name) {
      return &tool;
    }
  }
  return nullptr;
}

ToolResult Agent::runTool(const ToolCall& call, const CancelPtr& cancel) {
  const ToolDef* tool = findTool(call.name);
  const QString time = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  QJsonValue input = call.args;
  QString approval;
  auto log = [&](const QString& status, const QString& output, qint64 durationMs) {
    if (m_options.onToolRun) {
      m_options.onToolRun(ToolRun{time, call.name, input, approval, status, durationMs, output, {}});
    }
  };
  auto fail = [&](const QString& content, const QString& summary, const QString& status) {
    emitEvent(Event::ToolEnd{call.id, call.name, summary, content, true, std::nullopt});
    log(status, content, 0);
    return ToolResult::error(content);
  };

  if (!tool) {
    QStringList names;
    for (const ToolDef& candidate : m_tools) {
      names << candidate.name;
    }
    return fail(QString("Unknown tool \"%1\". Available tools: %2.").arg(call.name, names.join(", ")), {}, "invalid");
  }

  QJsonValue raw;
  try {
    raw = call.args.trimmed().isEmpty() ? QJsonValue(QJsonObject()) : Json::parse(call.args);
    input = raw;
  } catch (const std::exception&) {
    return fail("The arguments were not valid JSON: " + truncateText(call.args, 500), {}, "invalid");
  }
  if (const auto problems = validateSchema(raw, tool->schema)) {
    return fail("Invalid arguments:\n" + *problems, {}, "invalid");
  }

  const QJsonObject args = raw.toObject();
  const QString summary = tool->summarize ? tool->summarize(args) : QString();
  const ToolContext ctx = toolContext(*tool, cancel);

  QString reason;
  if (tool->readOnly && tool->approvalReason) {
    try {
      reason = tool->approvalReason(args, ctx).value_or(QString());
    } catch (const std::exception&) {
      // The call itself will report the problem.
    }
  }
  const QString warning = runLaterWarning(*tool, args);
  const bool needsApproval = !tool->readOnly || !reason.isEmpty() || !warning.isEmpty();
  // Commands that would run with no sandbox are asked about every time.
  const bool unsandboxed = tool->sandboxed && !ctx.sandboxOrNone()->active();
  if (m_pAlwaysAllowed->denies(tool->name, args)) {
    approval = "denied by the config";
    return fail("The user's config file doesn't allow this (permissions.deny). Don't try it another way; ask them how they want to proceed.", summary,
                "denied");
  }
  if (planning() && changes(*tool, args)) {
    approval = "refused in plan mode";
    return fail("Plan mode is on, so nothing may be changed or run yet. Finish the plan with what you can read; the user switches plan mode "
                "off to let you carry it out.",
                summary, "denied");
  }
  approval = needsApproval ? "already allowed" : "not needed";
  if (needsApproval && (!m_pAlwaysAllowed->allows(tool->name, args, unsandboxed) || !warning.isEmpty())) {
    // Show the change in the prompt; a call that would fail anyway fails without asking.
    std::optional<Diff> preview;
    if (tool->preview) {
      try {
        preview = tool->preview(args, ctx);
      } catch (const std::exception& error) {
        approval.clear();
        return fail(errorMessage(error), summary, "error");
      }
    }
    ApprovalRequest request;
    request.tool = tool->name;
    request.summary = summary;
    request.input = args;
    request.preview = preview;
    request.note = reason;
    request.warning = warning;
    if (tool->sandboxed) {
      const auto sandbox = ctx.sandboxOrNone();
      request.hasSandbox = true;
      request.sandboxActive = sandbox->active();
      request.sandboxDescription = sandbox->describe(m_options.cwd);
      if (!tool->extraAccess.isEmpty()) {
        request.sandboxDescription += "; this tool also gets " + tool->extraAccess;
      }
    }
    request.canAllowAlways = !unsandboxed;
    request.similar = tool->name == BASH_TOOL ? commandPrefix(args.value("command").toString(), unsandboxed) : QString();
    request.remembered = m_pAlwaysAllowed->remembers();
    const ApprovalDecision decision = requestApproval(request, cancel);
    if (decision == ApprovalDecision::Deny || cancelled(cancel)) {
      approval = "denied";
      return fail("The user declined this tool call. Ask them how they want to proceed.", summary, "denied");
    }
    approval = decision == ApprovalDecision::AllowAlways ? "allowed always" : decision == ApprovalDecision::AllowSimilar ? "allowed for commands like it" : "allowed";
  }

  emitEvent(Event::ToolStart{call.id, tool->name, summary});

  QElapsedTimer clock;
  clock.start();
  ToolResult result;
  try {
    result = tool->run(args, ctx);
  } catch (const std::exception& error) {
    result = ToolResult::error(errorMessage(error));
  }
  log(result.isError ? "error" : "ok", result.content, clock.elapsed());

  emitEvent(Event::ToolEnd{call.id, tool->name, summary, result.content, result.isError, result.diff});
  return result;
}

bool Agent::changes(const ToolDef& tool, const QJsonObject& args) {
  // A sub-agent sent to work would change things; one sent to look wouldn't.
  return !tool.readOnly || (tool.name == "task" && args.value("can_write").toBool(false));
}

ApprovalDecision Agent::requestApproval(const ApprovalRequest& request, const CancelPtr& cancel) {
  // One at a time. A request that waited may no longer need asking: an
  // earlier answer can have been "always allow", or the user interrupted.
  QMutexLocker lock(&m_approvalMutex);
  if (cancelled(cancel)) {
    return ApprovalDecision::Deny;
  }
  if (request.warning.isEmpty() && m_pAlwaysAllowed->allows(request.tool, request.input, !request.canAllowAlways)) {
    return ApprovalDecision::Allow;
  }
  if (!m_options.approve) {
    return ApprovalDecision::Deny;
  }
  ApprovalDecision decision = m_options.approve(request);
  // An answer that wasn't on offer allows only this call.
  if ((decision == ApprovalDecision::AllowAlways && !request.canAllowAlways) ||
      (decision == ApprovalDecision::AllowSimilar && request.similar.isEmpty())) {
    decision = ApprovalDecision::Allow;
  }
  // Remembered before the lock goes, or a request waiting for it would ask again.
  if (decision == ApprovalDecision::AllowAlways) {
    m_pAlwaysAllowed->insert(request.tool);
  } else if (decision == ApprovalDecision::AllowSimilar) {
    m_pAlwaysAllowed->insert(request.tool + ":" + request.similar);
  }
  return decision;
}

Answer Agent::askUser(const Question& question, const CancelPtr& cancel) {
  // Questions wait their turn with approvals, so only one prompt shows at a time.
  QMutexLocker lock(&m_approvalMutex);
  if (cancelled(cancel)) {
    return Answer{};
  }
  return m_options.ask(question);
}

QString Agent::runLaterWarning(const ToolDef& tool, const QJsonObject& args) const {
  if (!m_options.protectRunLater || !tool.touches) {
    return {};
  }
  QStringList reasons;
  for (const QString& path : tool.touches(args)) {
    const QString rel = relativeSlashPath(m_options.cwd, path);
    const QString why = runLaterReason(rel);
    if (!why.isEmpty()) {
      reasons << QString("%1 runs outside the sandbox later: %2. Check the change.").arg(rel, why);
    }
  }
  return reasons.join('\n');
}

void Agent::setTodos(const TodoList& todos) {
  bool changed = false;
  {
    QMutexLocker lock(&m_mutex);
    changed = todos != m_todos;
    m_todos = todos;
  }
  if (changed) {
    emitEvent(Event::Todos{todos});
  }
}

ToolContext Agent::toolContext(const ToolDef& tool, const CancelPtr& cancel) {
  std::shared_ptr<Checkpoint> checkpoint;
  {
    QMutexLocker lock(&m_mutex);
    if (!m_turns.isEmpty()) {
      checkpoint = m_turns.last().checkpoint;
    }
  }
  // Tools that change things without reporting what (bash) can't be undone.
  if (checkpoint && !tool.readOnly && !tool.preview) {
    checkpoint->addUntracked(tool.name);
  }
  ToolContext ctx;
  ctx.cwd = m_options.cwd;
  ctx.cancel = cancel;
  ctx.sandbox = m_options.sandbox;
  // Scratch files in the sandbox's /tmp aren't the project's: /undo and the report leave them out.
  ctx.beforeWrite = [this, checkpoint](const QString& file) {
    if (checkpoint && isWithin(m_options.cwd, file)) {
      checkpoint->beforeWrite(file);
    }
  };
  ctx.afterWrite = [this, checkpoint](const QString& file, const QByteArray& content) {
    if (!isWithin(m_options.cwd, file)) {
      return;
    }
    if (checkpoint) {
      checkpoint->afterWrite(file, content);
    }
    // Approved with the warning: the end-of-turn report leaves it out, unless it changes again.
    const QString rel = relativeSlashPath(m_options.cwd, file);
    if (!runLaterReason(rel).isEmpty()) {
      const QString hash = hashBytes(content);
      if (m_options.onRunLaterApproved) {
        m_options.onRunLaterApproved(rel, hash);
      }
      QMutexLocker lock(&m_mutex);
      m_approvedRunLater.insert(rel, hash);
    }
  };
  return ctx;
}

} // namespace Smith
