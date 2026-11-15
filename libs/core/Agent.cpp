#include "core/Agent.h"

#include "core/Context.h"
#include "core/Errors.h"
#include "core/Http.h"
#include "core/Json.h"
#include "core/Reasoning.h"
#include "core/RunLater.h"
#include "core/Sandbox.h"
#include "core/Think.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QLocale>
#include <QMutexLocker>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace Smith {

namespace {

constexpr int MAX_UNDO = 20;
constexpr int MAX_REPORT_CHARS = 8000;
// Read-only tool calls running at once, so a burst of greps can't swamp the machine.
constexpr int MAX_PARALLEL_TOOLS = 8;

// Compact when the context is this full, and aim to get below the second figure.
constexpr double COMPACT_AT = 0.75;
constexpr double TRIM_TARGET = 0.6;
// Share of the window kept verbatim (most recent messages) when summarizing.
constexpr double TAIL_SHARE = 0.25;

QString formatNumber(qint64 n) {
  return QLocale(QLocale::English).toString(n);
}

QString relativeSlashPath(const QString& cwd, const QString& path) {
  return QDir(cwd).relativeFilePath(QDir::cleanPath(QDir(cwd).absoluteFilePath(path)));
}

} // namespace

const QString TODO_REMINDER = "[Reminder] Your todo list still has unfinished steps:";

Agent::Agent(AgentOptions options) : m_options(std::move(options)) {
  m_history = Messages{Message{{"role", "system"}, {"content", m_options.systemPrompt}}} + repairHistory(m_options.history);
  m_tools = m_options.tools;
  if (!m_options.subagentPrompt.isEmpty()) {
    m_tools.append(taskTool([this](const TaskInput& input, const ToolContext& ctx) { return runSubagent(input, ctx.cancel); }));
  }
  if (m_options.todos) {
    m_tools.append(createTodoTool([this](const TodoList& todos) { setTodos(todos); }));
  }
  if (m_options.ask) {
    m_tools.append(askTool([this](const Question& question, const ToolContext& ctx) { return askUser(question, ctx.cancel); }));
  }
  for (const ToolDef& tool : m_tools) {
    m_apiTools.append(toApiTool(tool));
  }
  m_contextSize = m_options.contextSize;
  m_pAlwaysAllowed = m_options.alwaysAllowed ? m_options.alwaysAllowed : std::make_shared<AllowedTools>();
  m_pServer = m_options.server;
  m_model = m_options.model;
  m_reasoning = m_options.reasoning.isEmpty() ? QString("default") : m_options.reasoning;
  m_todos = todosFromHistory(m_history);
}

QString Agent::model() const {
  QMutexLocker lock(&m_mutex);
  return m_model;
}

std::optional<int> Agent::contextLimit() const {
  QMutexLocker lock(&m_mutex);
  return m_contextSize;
}

QString Agent::reasoning() const {
  QMutexLocker lock(&m_mutex);
  return m_reasoning;
}

ServerPtr Agent::server() const {
  QMutexLocker lock(&m_mutex);
  return m_pServer;
}

void Agent::setModel(const QString& model, std::optional<int> contextSize, const ServerPtr& server) {
  {
    QMutexLocker lock(&m_mutex);
    if (server) {
      m_pServer = server;
    }
    m_model = model;
    m_contextSize = contextSize;
    // Token counts from the old model's tokenizer no longer apply.
    m_measured.reset();
  }
  emitContext();
}

void Agent::setReasoning(const QString& level) {
  QMutexLocker lock(&m_mutex);
  m_reasoning = level;
}

TodoList Agent::todos() const {
  QMutexLocker lock(&m_mutex);
  return m_todos;
}

Messages Agent::messages() const {
  QMutexLocker lock(&m_mutex);
  return m_history;
}

void Agent::reset() {
  replaceHistory({});
  {
    QMutexLocker lock(&m_mutex);
    m_turns.clear();
  }
  setTodos({});
}

void Agent::loadConversation(const Messages& messages) {
  {
    QMutexLocker lock(&m_mutex);
    m_history = Messages{m_history.first()} + repairHistory(messages);
    m_measured.reset();
    m_turns.clear();
  }
  setTodos(todosFromHistory(this->messages()));
  emitContext();
}

void Agent::queue(const QString& text) {
  QMutexLocker lock(&m_mutex);
  m_queued << text;
}

QStringList Agent::queuedMessages() const {
  QMutexLocker lock(&m_mutex);
  return m_queued;
}

QStringList Agent::takeQueued() {
  QMutexLocker lock(&m_mutex);
  QStringList taken = m_queued;
  m_queued.clear();
  return taken;
}

bool Agent::canUndo() const {
  QMutexLocker lock(&m_mutex);
  return !m_turns.isEmpty();
}

std::optional<UndoResult> Agent::undo() {
  Turn turn;
  Messages kept;
  {
    QMutexLocker lock(&m_mutex);
    if (m_turns.isEmpty()) {
      return std::nullopt;
    }
    turn = m_turns.takeLast();
    kept = m_history.mid(1, turn.start - 1);
  }
  const RestoreResult restored = restore(*turn.checkpoint);
  replaceHistory(kept);
  setTodos(todosFromHistory(messages()));
  emitContext();
  return UndoResult{turn.prompt, restored.restored, restored.skipped, turn.checkpoint->untracked()};
}

void Agent::emitEvent(const AgentEvent& event) const {
  if (m_options.onEvent) {
    m_options.onEvent(event);
  }
}

void Agent::send(const QString& text, const CancelPtr& cancel, const QList<Image>& images) {
  {
    QMutexLocker lock(&m_mutex);
    m_turns.append(Turn{int(m_history.size()), text, std::make_shared<Checkpoint>()});
    if (m_turns.size() > MAX_UNDO) {
      m_turns.removeFirst();
    }
    m_approvedRunLater.clear();
  }
  const int sent = int(messages().size());
  append(userMessage(text, images));
  std::optional<QMap<QString, QString>> before;
  if (m_options.protectRunLater) {
    try {
      before = snapshotRunLater(m_options.cwd);
    } catch (const std::exception&) {
      // A failed snapshot only means no report.
    }
  }
  try {
    interruptible(cancel, [&]() { loop(cancel); });
  } catch (...) {
    if (before) {
      reportRunLater(*before);
    }
    if (!images.isEmpty()) {
      dropUnansweredImages(sent);
    }
    throw;
  }
  if (before) {
    reportRunLater(*before);
  }
}

// A request with pictures that failed before the model answered: most likely
// the model can't read them, and with them left in the conversation every
// later request would fail the same way. The message stays, as text.
void Agent::dropUnansweredImages(int index) {
  const Messages history = messages();
  if (history.size() != index + 1 || !hasImages(history[index])) {
    return;
  }
  Messages kept = history.mid(1, index - 1);
  kept.append(withoutImages(history[index]));
  replaceHistory(kept);
  emitEvent(Event::Notice{NoticeLevel::Warn, "The request failed, so the pictures were taken out of the conversation: if this model can't read "
                                             "images, they would fail every request after it. Your message is still there."});
}

void Agent::reportRunLater(const QMap<QString, QString>& before) {
  QMap<QString, QString> after;
  try {
    after = snapshotRunLater(m_options.cwd);
  } catch (const std::exception&) {
    return;
  }
  QMap<QString, QString> approved;
  {
    QMutexLocker lock(&m_mutex);
    approved = m_approvedRunLater;
  }
  QStringList lines;
  for (const RunLaterChange& change : runLaterChanges(before, after)) {
    if (approved.contains(change.rel) && approved.value(change.rel) == after.value(change.rel)) {
      continue;
    }
    lines << QString("- %1 (%2): %3").arg(change.rel, change.kind, change.why);
  }
  if (lines.isEmpty()) {
    return;
  }
  emitEvent(Event::Notice{NoticeLevel::Warn, "This turn changed files that run outside the sandbox later:\n" + lines.join('\n') +
                                                      "\nLook at the changes (git diff) before you do any of that."});
}

void Agent::compactNow(const CancelPtr& cancel) {
  interruptible(cancel, [&]() { compact(cancel, "manual"); });
}

void Agent::interruptible(const CancelPtr& cancel, const std::function<void()>& work) {
  try {
    work();
  } catch (...) {
    if (!cancelled(cancel)) {
      throw;
    }
    emitEvent(Event::ResponseEnd{});
    emitEvent(Event::Done{DoneReason::Interrupted});
  }
}

void Agent::loop(const CancelPtr& cancel) {
  const int maxSteps = m_options.maxSteps > 0 ? m_options.maxSteps : 50;
  bool retriedOverflow = false;
  bool reminded = false;

  for (int step = 0; step < maxSteps; step++) {
    if (step > 0) {
      deliverQueued();
    }
    maybeCompact(cancel);

    StreamedResponse response;
    try {
      response = streamResponse(cancel);
    } catch (const std::exception& error) {
      // The server rejected the prompt as too long: our estimate was off or
      // we didn't know the window size. Compact once and retry.
      const std::optional<Overflow> overflow = cancelled(cancel) ? std::nullopt : contextOverflow(error);
      if (!overflow || retriedOverflow) {
        throw;
      }
      retriedOverflow = true;
      if (overflow->contextSize) {
        QMutexLocker lock(&m_mutex);
        m_contextSize = overflow->contextSize;
      }
      emitEvent(Event::ResponseEnd{});
      emitEvent(Event::Notice{NoticeLevel::Warn, "The conversation no longer fits the server's context window."});
      if (!compact(cancel, "overflow")) {
        throw;
      }
      continue;
    }
    retriedOverflow = false;
    emitEvent(Event::ResponseEnd{});
    append(assistantMessage(response));
    if (response.totalTokens) {
      QMutexLocker lock(&m_mutex);
      m_measured = qMakePair(*response.totalTokens, int(m_history.size()));
    }
    emitContext();

    if (response.toolCalls.isEmpty()) {
      // Something you typed while it answered: keep going so the model sees it.
      if (!queuedMessages().isEmpty() && response.finishReason != "length") {
        continue;
      }
      // Small models often stop halfway through a plan: point them back to it, once a turn.
      const TodoList current = todos();
      const TodoList left = unfinished(current);
      if (!left.isEmpty() && !reminded && response.finishReason != "length") {
        reminded = true;
        emitEvent(Event::Notice{NoticeLevel::Info, QString("Reminded the agent of %1.").arg(plural(left.size(), "unfinished todo"))});
        append(userMessage(TODO_REMINDER + "\n" + formatTodos(current) +
                           "\n\nContinue with them. If they're done or no longer needed, update the list with todo_write. If you need something "
                           "from the user, ask them."));
        continue;
      }
      if (response.finishReason == "length") {
        emitEvent(Event::Notice{NoticeLevel::Warn, "The response hit the server's token limit and was cut off."});
        emitEvent(Event::Done{DoneReason::Length});
      } else {
        emitEvent(Event::Done{DoneReason::Stop});
      }
      return;
    }

    // Every tool call needs a matching tool message, even when the user
    // interrupts partway, or the next request will be rejected.
    const QList<ToolResult> results = runTools(response.toolCalls, cancel);
    for (int i = 0; i < response.toolCalls.size(); i++) {
      const ToolResult& result = results[i];
      append(toolMessage(response.toolCalls[i].id, result.isError ? "Error: " + result.content : result.content));
    }
    if (cancelled(cancel)) {
      emitEvent(Event::Done{DoneReason::Interrupted});
      return;
    }
  }

  emitEvent(Event::Notice{NoticeLevel::Warn, QString("Stopped after %1 model calls without a final answer. Send a message (e.g. \"continue\") to "
                                                          "let it go on, or raise maxSteps in the config file.")
                                                      .arg(maxSteps)});
  emitEvent(Event::Done{DoneReason::MaxSteps});
}

void Agent::deliverQueued() {
  const QStringList texts = takeQueued();
  if (texts.isEmpty()) {
    return;
  }
  for (const QString& text : texts) {
    emitEvent(Event::UserMessage{text});
  }
  append(userMessage(texts.join("\n\n")));
}

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
  child.maxSteps = m_options.maxSteps;
  child.contextSize = contextLimit();
  child.sandbox = m_options.sandbox;
  // Read-only tools only (which also rules out nested sub-agents), no checklist or questions.
  for (const ToolDef& tool : m_options.tools) {
    if (tool.forSubagents()) {
      child.tools.append(tool);
    }
  }
  child.todos = false;
  child.protectRunLater = false;
  child.systemPrompt = m_options.subagentPrompt;
  child.alwaysAllowed = m_pAlwaysAllowed;
  const QString description = input.description;
  // Its tools are read-only, but reading outside the project still asks you.
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
    } else if (const auto* done = std::get_if<Event::Done>(&event)) {
      reason = done->reason;
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

// ---- context management ----

qint64 Agent::estimateContext() const {
  QMutexLocker lock(&m_mutex);
  if (m_measured) {
    return m_measured->first + estimateTokens(m_history.mid(m_measured->second));
  }
  return estimateTokens(m_history) + estimateTokens(m_apiTools);
}

void Agent::emitContext() const {
  emitEvent(Event::Context{estimateContext(), contextLimit()});
}

void Agent::maybeCompact(const CancelPtr& cancel) {
  const std::optional<int> limit = contextLimit();
  if (limit && estimateContext() > *limit * COMPACT_AT) {
    compact(cancel, "auto");
  }
}

bool Agent::compact(const CancelPtr& cancel, const QString& reason) {
  const qint64 before = estimateContext();
  // Without a known window (overflow on an unknown server), assume we're just past it.
  const qint64 limit = contextLimit().value_or(std::max<qint64>(4096, qint64(before * 0.9)));
  const Messages history = this->messages();
  const qint64 fixedCost = estimateTokens(history.first()) + estimateTokens(m_apiTools);

  const TrimResult trimmed = trimOldToolResults(history.mid(1));
  if (reason == "auto" && trimmed.count > 0 && fixedCost + estimateTokens(trimmed.messages) < limit * TRIM_TARGET) {
    replaceHistory(trimmed.messages);
    emitEvent(Event::Notice{NoticeLevel::Info, QString("Cleared %1 old tool outputs to free context (~%2 → ~%3 tokens).")
                                                        .arg(QString::number(trimmed.count), formatNumber(before), formatNumber(estimateContext()))});
    emitContext();
    return true;
  }

  const Messages& messages = trimmed.messages;
  const int tailStart = chooseTail(messages, qint64(limit * TAIL_SHARE));
  const Messages head = messages.mid(0, tailStart);
  const Messages tail = messages.mid(tailStart);
  if (head.isEmpty()) {
    if (trimmed.count == 0) {
      emitEvent(Event::Notice{NoticeLevel::Warn, "Nothing to compact: the conversation is only the latest messages."});
      return false;
    }
    replaceHistory(messages);
    emitContext();
    return true;
  }

  emitEvent(Event::Activity{"Summarizing the conversation to free context…", true});
  const int maxChars = int(std::max<double>(8000, std::floor((limit - fixedCost - 4096) * CHARS_PER_TOKEN * 0.8)));
  const QString summary = summarize(head, maxChars, cancel);
  // The checklist lives in the tool calls being summarized away; carry it over.
  const TodoList current = todos();
  const QString todoText = unfinished(current).isEmpty() ? QString() : "\n\nYour todo list so far:\n" + formatTodos(current);
  const QString intro = SUMMARY_PREFIX + summary + todoText;

  // Keep user/assistant alternation: some chat templates reject two user messages in a row.
  Messages next;
  if (!tail.isEmpty() && messageRole(tail.first()) == "user") {
    next.append(userMessage(intro + "\n\n---\n\n" + messageText(tail.first())));
    next.append(tail.mid(1));
  } else {
    next.append(userMessage(tail.isEmpty() ? intro + "\n\nContinue the task from where it left off." : intro));
    next.append(tail);
  }
  replaceHistory(next);
  // Turn boundaries no longer exist in the summarized history.
  bool hadUndo = false;
  {
    QMutexLocker lock(&m_mutex);
    hadUndo = !m_turns.isEmpty();
    m_turns.clear();
  }
  const QString undoNote = hadUndo ? QString(" Earlier turns can no longer be undone.") : QString();
  emitEvent(Event::Notice{NoticeLevel::Info, QString("Summarized %1 earlier messages (~%2 → ~%3 tokens).%4")
                                                      .arg(QString::number(head.size()), formatNumber(before), formatNumber(estimateContext()), undoNote)});
  emitContext();
  return true;
}

QString Agent::complete(const QString& instructions, const QString& input, const CancelPtr& cancel) {
  const QString level = reasoning();
  // These don't need deep thought; keep them quick unless the user opted out of reasoning settings.
  QJsonObject body = requestBase(level == "default" || level == "off" ? level : QString("low"));
  body["messages"] = QJsonArray{
      QJsonObject{{"role", "system"}, {"content", instructions}},
      QJsonObject{{"role", "user"}, {"content", input}},
  };
  const ServerPtr target = server();
  const QJsonObject response = Http::postJson(target->url("/chat/completions"), target->apiKey(), body, cancel);
  const QString content = response.value("choices").toArray().at(0).toObject().value("message").toObject().value("content").toString();
  return stripThinkTags(content);
}

QString Agent::summarize(const Messages& messages, int maxChars, const CancelPtr& cancel) {
  const QString summary = complete(SUMMARY_INSTRUCTIONS, "Conversation to summarize:\n\n" + renderTranscript(messages, maxChars), cancel);
  if (summary.isEmpty()) {
    throw SmithError("The model returned an empty summary; the conversation was not compacted.");
  }
  return summary;
}

// ---- history bookkeeping ----

void Agent::append(const Message& message) {
  {
    QMutexLocker lock(&m_mutex);
    m_history.append(message);
  }
  if (m_options.onHistory) {
    HistoryChange change;
    change.type = HistoryChange::Type::Append;
    change.message = message;
    m_options.onHistory(change);
  }
}

void Agent::replaceHistory(const Messages& messages) {
  {
    QMutexLocker lock(&m_mutex);
    m_history = Messages{m_history.first()} + messages;
    m_measured.reset();
  }
  if (m_options.onHistory) {
    HistoryChange change;
    change.type = HistoryChange::Type::Replace;
    change.messages = messages;
    m_options.onHistory(change);
  }
}

// ---- model calls ----

QJsonObject Agent::requestBase(const QString& reasoningLevel) const {
  QJsonObject body{{"model", model()}};
  const QJsonObject params = reasoningParams(reasoningLevel);
  for (auto it = params.begin(); it != params.end(); ++it) {
    body[it.key()] = it.value();
  }
  if (m_options.temperature) {
    body["temperature"] = *m_options.temperature;
  }
  return body;
}

Agent::StreamedResponse Agent::streamResponse(const CancelPtr& cancel) {
  QJsonObject body = requestBase(reasoning());
  QJsonArray messages;
  for (const Message& message : this->messages()) {
    messages.append(message);
  }
  body["messages"] = messages;
  if (!m_apiTools.isEmpty()) {
    body["tools"] = m_apiTools;
    // Lets the model ask for several tools at once; llama.cpp only allows one per response unless asked.
    body["parallel_tool_calls"] = true;
  }
  body["stream"] = true;
  body["stream_options"] = QJsonObject{{"include_usage", true}};

  StreamedResponse result;
  QList<ToolCall> calls;
  ThinkTagParser thinkTags;
  auto output = [&](BlockKind kind, const QString& text) {
    if (kind == BlockKind::Text) {
      result.text += text;
    } else {
      result.reasoning += text;
    }
    emitEvent(Event::Delta{kind, text});
  };

  const ServerPtr target = server();
  const int skipped = Http::postStream(target->url("/chat/completions"), target->apiKey(), body, cancel, [&](const QJsonObject& chunk) {
    const QJsonObject usage = chunk.value("usage").toObject();
    if (!usage.isEmpty()) {
      result.totalTokens = qint64(usage.value("prompt_tokens").toDouble() + usage.value("completion_tokens").toDouble());
    }
    const QJsonArray choices = chunk.value("choices").toArray();
    if (choices.isEmpty()) {
      return;
    }
    const QJsonObject choice = choices.first().toObject();
    const QJsonObject delta = choice.value("delta").toObject();

    QString reasoningText = delta.value("reasoning_content").toString();
    if (reasoningText.isEmpty()) {
      reasoningText = delta.value("reasoning").toString();
    }
    if (!reasoningText.isEmpty()) {
      output(BlockKind::Reasoning, reasoningText);
    }
    // Servers that don't separate reasoning leave <think> tags in the content.
    const QString content = delta.value("content").toString();
    if (!content.isEmpty()) {
      for (const Segment& segment : thinkTags.push(content)) {
        output(segment.kind, segment.text);
      }
    }

    // Tool calls arrive in fragments keyed by index: the first fragment
    // carries the id and name, later ones append to the JSON arguments.
    for (const QJsonValue& value : delta.value("tool_calls").toArray()) {
      const QJsonObject fragment = value.toObject();
      const int index = fragment.value("index").toInt(0);
      while (calls.size() <= index) {
        calls.append(ToolCall{});
      }
      ToolCall& slot = calls[index];
      if (!fragment.value("id").toString().isEmpty()) {
        slot.id = fragment.value("id").toString();
      }
      const QJsonObject function = fragment.value("function").toObject();
      const QString name = function.value("name").toString();
      if (!name.isEmpty()) {
        if (slot.name.isEmpty()) {
          emitEvent(Event::ToolPrepare{name});
        }
        slot.name += name;
      }
      const QJsonValue arguments = function.value("arguments");
      if (arguments.isString()) {
        slot.args += arguments.toString();
      } else if (arguments.isObject()) {
        // A non-streaming fallback may carry the arguments as an object.
        slot.args += Json::stringify(arguments);
      }
    }
    const QString finish = choice.value("finish_reason").toString();
    if (!finish.isEmpty()) {
      result.finishReason = finish;
    }
  });
  for (const Segment& segment : thinkTags.flush()) {
    output(segment.kind, segment.text);
  }
  if (skipped > 0) {
    emitEvent(Event::Notice{NoticeLevel::Warn, QString("The server sent %1 that couldn't be read as JSON; %2 skipped, so this response may be "
                                                            "incomplete.")
                                                        .arg(plural(skipped, "chunk"), skipped == 1 ? "it was" : "they were")});
  }

  // Some local servers omit tool-call ids; the protocol needs them to pair results.
  for (ToolCall call : calls) {
    if (call.name.isEmpty()) {
      continue;
    }
    if (call.id.isEmpty()) {
      call.id = "call_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    result.toolCalls.append(call);
  }
  return result;
}

Message Agent::assistantMessage(const StreamedResponse& response) {
  const QString text = response.text.trimmed();
  if (response.toolCalls.isEmpty()) {
    return Message{{"role", "assistant"}, {"content", text}};
  }
  QJsonArray toolCalls;
  for (const ToolCall& call : response.toolCalls) {
    toolCalls.append(QJsonObject{
        {"id", call.id},
        {"type", "function"},
        {"function", QJsonObject{{"name", call.name}, {"arguments", call.args.isEmpty() ? QString("{}") : call.args}}},
    });
  }
  Message message{{"role", "assistant"}, {"content", text.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(text)}, {"tool_calls", toolCalls}};
  // Models such as gpt-oss need their reasoning replayed between tool calls;
  // llama.cpp reads it from this field, and chat templates that don't use it drop it.
  if (!response.reasoning.isEmpty()) {
    message["reasoning_content"] = response.reasoning;
  }
  return message;
}

// ---- tools ----

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
    }
    request.canAllowAlways = !unsandboxed;
    request.similar = tool->sandboxed ? commandPrefix(args.value("command").toString()) : QString();
    request.remembered = m_pAlwaysAllowed->remembers();
    ApprovalDecision decision = requestApproval(request, cancel);
    // An answer that wasn't on offer allows only this call.
    if ((decision == ApprovalDecision::AllowAlways && unsandboxed) || (decision == ApprovalDecision::AllowSimilar && request.similar.isEmpty())) {
      decision = ApprovalDecision::Allow;
    }
    if (decision == ApprovalDecision::Deny || cancelled(cancel)) {
      approval = "denied";
      return fail("The user declined this tool call. Ask them how they want to proceed.", summary, "denied");
    }
    approval = decision == ApprovalDecision::AllowAlways ? "allowed always" : decision == ApprovalDecision::AllowSimilar ? "allowed for commands like it" : "allowed";
    if (decision == ApprovalDecision::AllowAlways) {
      m_pAlwaysAllowed->insert(tool->name);
    } else if (decision == ApprovalDecision::AllowSimilar) {
      m_pAlwaysAllowed->insert(tool->name + ":" + request.similar);
    }
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
  return m_options.approve(request);
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
  ctx.beforeWrite = [checkpoint](const QString& file) {
    if (checkpoint) {
      checkpoint->beforeWrite(file);
    }
  };
  ctx.afterWrite = [this, checkpoint](const QString& file, const QByteArray& content) {
    if (checkpoint) {
      checkpoint->afterWrite(file, content);
    }
    // Approved with the warning: the end-of-turn report leaves it out, unless it changes again.
    const QString rel = relativeSlashPath(m_options.cwd, file);
    if (!runLaterReason(rel).isEmpty()) {
      QMutexLocker lock(&m_mutex);
      m_approvedRunLater.insert(rel, hashBytes(content));
    }
  };
  return ctx;
}

} // namespace Smith
