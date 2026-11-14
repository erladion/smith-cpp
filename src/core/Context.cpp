#include "core/Context.h"

#include "core/Errors.h"
#include "core/Json.h"
#include "core/Tool.h"

#include <QRegularExpression>
#include <QSet>

#include <cmath>

namespace Smith {

const QString SUMMARY_PREFIX = "[Summary of the earlier conversation, written when the context window filled up]\n\n";

namespace {

const QString TRIMMED_NOTE = "[Old tool output removed to save context. Run the tool again if you need it.]";

} // namespace

const QString SUMMARY_INSTRUCTIONS =
    "You are summarizing a conversation between a user and a coding agent so the agent can continue the work with a "
    "fresh context window. The agent will see only your summary plus the most recent messages.\n"
    "\n"
    "Write a concise summary covering:\n"
    "- The user's goal and any requirements or preferences they stated.\n"
    "- What has been done so far: files read, created or changed (with paths), commands run and their important results.\n"
    "- Decisions made and why, including approaches that were tried and failed.\n"
    "- The current state of the work and the immediate next step.\n"
    "\n"
    "Keep exact file paths, function names, error messages and values that the agent may need. Leave out pleasantries "
    "and anything that no longer matters. Use plain text with short bullet points.";

qint64 estimateTokens(const QString& text) {
  return qint64(std::ceil(text.size() / CHARS_PER_TOKEN));
}

qint64 estimateTokens(const Message& message) {
  return estimateTokens(Json::stringify(message));
}

qint64 estimateTokens(const Messages& messages) {
  QJsonArray array;
  for (const Message& message : messages) {
    array.append(message);
  }
  return estimateTokens(array);
}

qint64 estimateTokens(const QJsonArray& value) {
  return estimateTokens(Json::stringify(value));
}

TrimResult trimOldToolResults(const Messages& messages, int keepRecent, int minChars) {
  QList<int> toolIndexes;
  for (int i = 0; i < messages.size(); i++) {
    if (messageRole(messages[i]) == "tool") {
      toolIndexes.append(i);
    }
  }
  QSet<int> trimmable;
  for (int i = 0; i < std::max(0, int(toolIndexes.size()) - keepRecent); i++) {
    trimmable.insert(toolIndexes[i]);
  }
  TrimResult result;
  for (int i = 0; i < messages.size(); i++) {
    Message message = messages[i];
    if (trimmable.contains(i)) {
      const QString text = messageText(message);
      if (text.size() >= minChars && text != TRIMMED_NOTE) {
        result.count++;
        message["content"] = TRIMMED_NOTE;
      }
    }
    result.messages.append(message);
  }
  return result;
}

int chooseTail(const Messages& messages, qint64 budgetTokens) {
  int start = messages.size();
  qint64 used = 0;
  for (int i = messages.size() - 1; i >= 0; i--) {
    used += estimateTokens(messages[i]);
    if (used > budgetTokens) {
      break;
    }
    if (messageRole(messages[i]) != "tool") {
      start = i;
    }
  }
  return start;
}

QString renderTranscript(const Messages& messages, int maxChars) {
  QStringList parts;
  for (const Message& message : messages) {
    const QString text = messageText(message);
    const QString role = messageRole(message);
    if (role == "user") {
      parts << "USER:\n" + truncateText(text, 6000);
    } else if (role == "assistant") {
      if (!text.isEmpty()) {
        parts << "ASSISTANT:\n" + truncateText(text, 6000);
      }
      for (const QJsonValue& value : message.value("tool_calls").toArray()) {
        const QJsonObject function = value.toObject().value("function").toObject();
        parts << QString("ASSISTANT CALLED %1(%2)").arg(function.value("name").toString(), truncateText(function.value("arguments").toString(), 1500));
      }
    } else if (role == "tool") {
      parts << "TOOL RESULT:\n" + truncateText(text, 2000);
    }
  }
  return truncateText(parts.join("\n\n"), maxChars);
}

std::optional<Overflow> contextOverflow(const std::exception& error) {
  const auto* api = dynamic_cast<const ApiError*>(&error);
  if (!api) {
    return std::nullopt;
  }
  static const QRegularExpression pattern(
      "exceed_context_size|exceeds the available context|maximum context length|context (length|window) exceeded|prompt is too long",
      QRegularExpression::CaseInsensitiveOption);
  const QString text = api->message() + " " + Json::stringify(api->body());
  const bool matches = api->code() == "context_length_exceeded" || api->body().value("code").toString() == "context_length_exceeded" ||
                       pattern.match(text).hasMatch();
  if (!matches) {
    return std::nullopt;
  }
  Overflow overflow;
  if (api->body().value("n_ctx").isDouble()) {
    overflow.contextSize = api->body().value("n_ctx").toInt();
  }
  return overflow;
}

} // namespace Smith
