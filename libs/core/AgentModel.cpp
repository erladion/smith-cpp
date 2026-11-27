#include "core/Agent.h"

#include "core/Context.h"
#include "core/Errors.h"
#include "core/Http.h"
#include "core/Json.h"
#include "core/LoopDetector.h"
#include "core/Reasoning.h"
#include "core/Think.h"

#include <QElapsedTimer>
#include <QLocale>
#include <QMutexLocker>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace Smith {

// Talking to the model: building requests, reading the streamed answer, and
// keeping the conversation inside the context window by clearing old tool
// output or summarizing.

namespace {

// Compact when the context is this full, unless the config says otherwise ("compactAt").
constexpr double COMPACT_AT = 0.75;
// Clearing old tool output is enough when it gets below this share of that.
constexpr double TRIM_TARGET = 0.8;
// Share of it kept verbatim (most recent messages) when summarizing.
constexpr double TAIL_SHARE = 1.0 / 3;

QString formatNumber(qint64 n) {
  return QLocale(QLocale::English).toString(n);
}

} // namespace

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

double Agent::compactAt() const {
  return modelSettings().compactAt.value_or(COMPACT_AT);
}

bool Agent::maybeCompact(const CancelPtr& cancel) {
  const std::optional<int> limit = contextLimit();
  return limit && estimateContext() > *limit * compactAt() && compact(cancel, "auto");
}

bool Agent::compact(const CancelPtr& cancel, const QString& reason) {
  const qint64 before = estimateContext();
  // Without a known window (overflow on an unknown server), assume we're just past it.
  const qint64 limit = contextLimit().value_or(std::max<qint64>(4096, qint64(before * 0.9)));
  const Messages history = this->messages();
  const qint64 fixedCost = estimateTokens(history.first()) + estimateTokens(m_apiTools);
  // Below the point compaction starts again, or it would come back at the next step.
  const double threshold = compactAt();

  const TrimResult trimmed = trimOldToolResults(history.mid(1));
  if (reason == "auto" && trimmed.count > 0 && fixedCost + estimateTokens(trimmed.messages) < limit * threshold * TRIM_TARGET) {
    replaceHistory(trimmed.messages);
    emitEvent(Event::Notice{NoticeLevel::Info, QString("Cleared %1 old tool outputs to free context (~%2 → ~%3 tokens).")
                                                        .arg(QString::number(trimmed.count), formatNumber(before), formatNumber(estimateContext()))});
    emitContext();
    return true;
  }

  const Messages& messages = trimmed.messages;
  const int tailStart = chooseTail(messages, qint64(limit * threshold * TAIL_SHARE));
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
  const QJsonObject usage = response.value("usage").toObject();
  if (!usage.isEmpty()) {
    emitEvent(Event::Usage{qint64(usage.value("prompt_tokens").toDouble()), qint64(usage.value("completion_tokens").toDouble()), 0});
  }
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

ModelSettings Agent::modelSettings() const {
  const ServerPtr target = server();
  return m_options.settingsFor ? m_options.settingsFor(target ? target->name() : QString(), model()) : ModelSettings{};
}

QJsonObject Agent::requestBase(const QString& reasoningLevel) const {
  // The config's fields first, then Smith's: a /reasoning or --temperature you chose wins.
  QJsonObject body = Json::overlay(modelSettings().params, reasoningParams(reasoningLevel));
  body["model"] = model();
  if (m_options.temperature) {
    body["temperature"] = *m_options.temperature;
  }
  return body;
}

Agent::StreamedResponse Agent::streamResponse(const CancelPtr& cancel, const QString& note) {
  QJsonObject body = requestBase(reasoning());
  Messages history = this->messages();
  if (!note.isEmpty()) {
    // Beside the last message you sent, or after the tool results: two user messages in a row upset some chat templates.
    if (messageRole(history.last()) == "user") {
      history.last() = withNote(history.last(), note);
    } else {
      history.append(userMessage(note));
    }
  }
  QJsonArray messages;
  for (const Message& message : history) {
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
  // Stops this response alone when it loops; the turn's token still stops it too.
  const auto request = std::make_shared<CancelToken>(cancel);
  const bool watch = modelSettings().loopDetection.value_or(false);
  LoopDetector reasoningLoop;
  LoopDetector textLoop;
  // Timed from the first token, so the speed leaves out reading the prompt.
  QElapsedTimer generating;
  auto output = [&](BlockKind kind, const QString& text) {
    if (result.looped) {
      return;
    }
    if (kind == BlockKind::Text) {
      result.text += text;
    } else {
      result.reasoning += text;
    }
    emitEvent(Event::Delta{kind, text});
    if (watch && (kind == BlockKind::Text ? textLoop : reasoningLoop).push(text)) {
      result.looped = true;
      request->cancel();
    }
  };

  const ServerPtr target = server();
  int skipped = 0;
  try {
    skipped = Http::postStream(target->url("/chat/completions"), target->apiKey(), body, request, [&](const QJsonObject& chunk) {
      if (result.looped) {
        return;
      }
      const QJsonObject usage = chunk.value("usage").toObject();
      if (!usage.isEmpty()) {
        result.promptTokens = qint64(usage.value("prompt_tokens").toDouble());
        result.completionTokens = qint64(usage.value("completion_tokens").toDouble());
        result.totalTokens = result.promptTokens + result.completionTokens;
      }
      const QJsonArray choices = chunk.value("choices").toArray();
      if (choices.isEmpty()) {
        return;
      }
      const QJsonObject choice = choices.first().toObject();
      const QJsonObject delta = choice.value("delta").toObject();
      if (!generating.isValid() && !delta.isEmpty()) {
        generating.start();
      }

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
  } catch (const AbortError&) {
    if (!result.looped || cancelled(cancel)) {
      throw;
    }
  }
  if (result.looped) {
    return result;
  }
  for (const Segment& segment : thinkTags.flush()) {
    output(segment.kind, segment.text);
  }
  if (result.totalTokens) {
    result.generationMs = generating.isValid() ? generating.elapsed() : 0;
    emitEvent(Event::Usage{result.promptTokens, result.completionTokens, result.generationMs});
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

} // namespace Smith
