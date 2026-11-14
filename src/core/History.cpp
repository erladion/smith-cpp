#include "core/History.h"

#include <QJsonArray>
#include <QSet>

namespace Smith {

QString messageRole(const Message& message) {
  return message.value("role").toString();
}

QString messageText(const Message& message) {
  const QJsonValue content = message.value("content");
  if (content.isString()) {
    return content.toString();
  }
  if (!content.isArray()) {
    return {};
  }
  QString text;
  for (const QJsonValue& part : content.toArray()) {
    text += part.toObject().value("text").toString();
  }
  return text;
}

Message userMessage(const QString& text) {
  return Message{{"role", "user"}, {"content", text}};
}

Message toolMessage(const QString& callId, const QString& content) {
  return Message{{"role", "tool"}, {"tool_call_id", callId}, {"content", content}};
}

Messages repairHistory(const Messages& messages) {
  Messages out;
  for (int i = 0; i < messages.size(); i++) {
    const Message& message = messages[i];
    out.append(message);
    const QJsonArray calls = message.value("tool_calls").toArray();
    if (messageRole(message) != "assistant" || calls.isEmpty()) {
      continue;
    }

    QSet<QString> answered;
    while (i + 1 < messages.size() && messageRole(messages[i + 1]) == "tool") {
      const Message& result = messages[++i];
      answered.insert(result.value("tool_call_id").toString());
      out.append(result);
    }
    for (const QJsonValue& call : calls) {
      const QString id = call.toObject().value("id").toString();
      if (!answered.contains(id)) {
        out.append(toolMessage(id, "Error: Interrupted before this tool ran."));
      }
    }
  }
  return out;
}

} // namespace Smith
