#include "core/Agent.h"

#include "core/Context.h"
#include "core/RunLater.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonDocument>
#include <QMutexLocker>

namespace Smith {

// The agent's loop and its state: turns, undo, the queue and the checklist.
// Talking to the model and keeping within its context is in AgentModel.cpp;
// running tools and sub-agents in AgentTools.cpp.

namespace {

constexpr int MAX_UNDO = 20;

const QString PLAN_NOTE = "[Plan mode is on. Look into this with the tools that read and search, then reply with a plan: the steps, the files "
                          "each one changes, and anything the user needs to decide. Don't change files or run commands yet: those calls are "
                          "refused until the user switches plan mode off.]";
const QString PLAN_OVER_NOTE = "[Plan mode is off: you can change files and run commands again.]";

// Goes with the request after a response that went round in circles.
const QString LOOP_NOTE = "[Smith stopped your last response: it kept repeating the same passage. Don't go over it again. Decide, and take the "
                          "next step: call a tool, or answer with what you have.]";

// With loop detection: how often one call may give the same result in a turn before Smith steps in.
constexpr int REPEAT_LIMIT = 3;

// Goes with the result of a call that keeps coming back the same.
const QString REPEAT_NOTE = "[Smith: this exact call has now given this exact result %1 times this turn. Running it again won't change "
                            "anything. Don't repeat it: try something different, or answer with what you have and say what's in the way.]";

// A call and what came of it: the same key means the same call came back the same.
QString repeatKey(const QString& name, const QString& args, const ToolResult& result) {
  const QJsonDocument parsed = QJsonDocument::fromJson(args.toUtf8());
  const QByteArray call = parsed.isObject() ? parsed.toJson(QJsonDocument::Compact) : args.toUtf8();
  const QByteArray outcome = (result.isError ? "error\n" : "ok\n") + result.content.toUtf8();
  return name + '\n' + QString::fromLatin1(QCryptographicHash::hash(call + '\n' + outcome, QCryptographicHash::Sha1).toHex());
}

} // namespace

const QString TODO_REMINDER = "[Reminder] Your todo list still has unfinished steps:";

Agent::Agent(AgentOptions options) : m_options(std::move(options)) {
  m_history = Messages{Message{{"role", "system"}, {"content", m_options.systemPrompt}}} + repairHistory(m_options.history);
  m_tools = m_options.tools;
  if (!m_options.subagentPrompt.isEmpty()) {
    m_tools.append(
        taskTool([this](const TaskInput& input, const ToolContext& ctx) { return runSubagent(input, ctx.cancel); }, !m_options.workerPrompt.isEmpty()));
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
  m_planning = m_options.planning;
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

void Agent::setPlanning(bool on) {
  QMutexLocker lock(&m_mutex);
  m_planning = on;
}

bool Agent::planning() const {
  QMutexLocker lock(&m_mutex);
  return m_planning;
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
    m_planned = false;
    m_originals.clear();
    m_untracked.clear();
  }
  setTodos({});
}

void Agent::loadConversation(const Messages& messages) {
  {
    QMutexLocker lock(&m_mutex);
    m_history = Messages{m_history.first()} + repairHistory(messages);
    m_measured.reset();
    m_turns.clear();
    m_planned = false;
    m_originals.clear();
    m_untracked.clear();
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

void Agent::absorb(const Checkpoint& checkpoint) {
  const QMap<QString, Checkpoint::FileChange> files = checkpoint.files();
  const QStringList untracked = checkpoint.untracked();
  QMutexLocker lock(&m_mutex);
  for (auto it = files.begin(); it != files.end(); ++it) {
    if (!m_originals.contains(it.key())) {
      m_originals.insert(it.key(), it.value().before);
    }
  }
  for (const QString& tool : untracked) {
    if (!m_untracked.contains(tool)) {
      m_untracked << tool;
    }
  }
}

SessionChanges Agent::sessionChanges() const {
  QMap<QString, std::optional<QByteArray>> originals;
  SessionChanges changes;
  std::shared_ptr<Checkpoint> current;
  {
    QMutexLocker lock(&m_mutex);
    originals = m_originals;
    changes.untracked = m_untracked;
    if (!m_turns.isEmpty()) {
      current = m_turns.last().checkpoint;
    }
  }
  // The turn under way isn't absorbed yet.
  if (current) {
    const QMap<QString, Checkpoint::FileChange> files = current->files();
    for (auto it = files.begin(); it != files.end(); ++it) {
      if (!originals.contains(it.key())) {
        originals.insert(it.key(), it.value().before);
      }
    }
    for (const QString& tool : current->untracked()) {
      if (!changes.untracked.contains(tool)) {
        changes.untracked << tool;
      }
    }
  }
  for (auto it = originals.begin(); it != originals.end(); ++it) {
    const std::optional<QByteArray> now = readOrNull(it.key());
    if (now != it.value()) {
      changes.files.append(ChangedFile{it.key(), it.value(), now});
    }
  }
  return changes;
}

void Agent::emitEvent(const AgentEvent& event) const {
  if (m_options.onEvent) {
    m_options.onEvent(event);
  }
}

void Agent::send(const QString& text, const CancelPtr& cancel, const QList<Image>& images, const QList<AttachedFile>& files) {
  const std::shared_ptr<Checkpoint> checkpoint = m_options.checkpoint ? m_options.checkpoint : std::make_shared<Checkpoint>();
  QString note;
  {
    QMutexLocker lock(&m_mutex);
    m_turns.append(Turn{int(m_history.size()), text, checkpoint});
    if (m_turns.size() > MAX_UNDO) {
      m_turns.removeFirst();
    }
    m_approvedRunLater.clear();
    if (m_planning) {
      note = PLAN_NOTE;
    } else if (m_planned) {
      note = PLAN_OVER_NOTE;
    }
    m_planned = m_planning;
  }
  const int sent = int(messages().size());
  const Message message = userMessage(text, images, files);
  append(note.isEmpty() ? message : withNote(message, note));
  std::optional<QMap<QString, QString>> before;
  if (m_options.protectRunLater && m_options.reportRunLater) {
    try {
      before = snapshotRunLater(m_options.cwd);
    } catch (const std::exception&) {
      // A failed snapshot only means no report.
    }
  }
  try {
    interruptible(cancel, [&]() { loop(cancel); });
  } catch (...) {
    absorb(*checkpoint);
    if (before) {
      reportRunLater(*before);
    }
    if (!images.isEmpty()) {
      dropUnansweredImages(sent);
    }
    throw;
  }
  absorb(*checkpoint);
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
  // After a response that looped, the note for the next request; a second loop in a row ends the turn.
  QString loopNote;
  // With loop detection: how often each call gave each result, until the history is compacted; and whether
  // the model was already told it's repeating a call, after which one more such call ends the turn.
  QHash<QString, int> repeats;
  bool toldOfRepeat = false;
  // A list the agent didn't touch this turn is from an earlier task: answering
  // something else shouldn't send it back to that one.
  const TodoList atStart = todos();

  for (int step = 0; step < maxSteps; step++) {
    if (step > 0) {
      deliverQueued();
    }
    if (maybeCompact(cancel)) {
      repeats.clear();
    }

    StreamedResponse response;
    try {
      response = streamResponse(cancel, loopNote);
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
      repeats.clear();
      continue;
    }
    retriedOverflow = false;
    emitEvent(Event::ResponseEnd{});
    if (response.looped) {
      if (!loopNote.isEmpty()) {
        emitEvent(Event::Notice{NoticeLevel::Warn, "The model went round in circles again, so this turn stops here. Sampling settings can help "
                                                   "(\"params\" in the config, e.g. presence_penalty), and so can /reasoning low."});
        emitEvent(Event::Done{DoneReason::Stop});
        return;
      }
      loopNote = LOOP_NOTE;
      emitEvent(Event::Notice{NoticeLevel::Warn, "The model was repeating itself, so Smith stopped that response and asked it to move on."});
      continue;
    }
    loopNote.clear();
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
      // Small models often stop halfway through a plan: point them back to it,
      // once a turn. Not for steps waiting on the user: stopping there is right.
      const TodoList current = todos();
      const TodoList left = actionable(current);
      if (!left.isEmpty() && current != atStart && !reminded && response.finishReason != "length") {
        reminded = true;
        emitEvent(Event::Notice{NoticeLevel::Info, QString("Reminded the agent of %1.").arg(plural(left.size(), "unfinished todo"))});
        append(userMessage(TODO_REMINDER + "\n" + formatTodos(current) +
                           "\n\nContinue with them. If they're done or no longer needed, update the list with todo_write. If one needs the "
                           "user (a manual test, a decision), mark it waiting and tell them what you need."));
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
    const bool watch = modelSettings().loopDetection.value_or(false) && !cancelled(cancel);
    bool repeated = false;
    bool stuck = false;
    for (int i = 0; i < response.toolCalls.size(); i++) {
      const ToolCall& call = response.toolCalls[i];
      const ToolResult& result = results[i];
      QString content = result.isError ? "Error: " + result.content : result.content;
      if (watch) {
        const int seen = ++repeats[repeatKey(call.name, call.args, result)];
        if (seen >= REPEAT_LIMIT) {
          repeated = true;
          stuck = stuck || toldOfRepeat;
          content += "\n\n" + REPEAT_NOTE.arg(seen);
        }
      }
      append(toolMessage(call.id, content));
    }
    if (cancelled(cancel)) {
      emitEvent(Event::Done{DoneReason::Interrupted});
      return;
    }
    if (stuck) {
      emitEvent(Event::Notice{NoticeLevel::Warn, "The model kept making the same calls and getting the same results, so this turn stops here. "
                                                 "Sampling settings can help (\"params\" in the config, e.g. presence_penalty), and so can "
                                                 "/reasoning low."});
      emitEvent(Event::Done{DoneReason::Stop});
      return;
    }
    if (repeated) {
      toldOfRepeat = true;
      emitEvent(Event::Notice{NoticeLevel::Warn, "The model made the same call with the same result again, so Smith told it to try something "
                                                 "else."});
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

} // namespace Smith
