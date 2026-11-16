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
  // The text comes first; the parts after it are pictures and their labels.
  const QJsonArray parts = content.toArray();
  return parts.isEmpty() ? QString() : parts.first().toObject().value("text").toString();
}

Message userMessage(const QString& text) {
  return Message{{"role", "user"}, {"content", text}};
}

Message userMessage(const QString& text, const QList<Image>& images) {
  if (images.isEmpty()) {
    return userMessage(text);
  }
  QJsonArray parts{QJsonObject{{"type", "text"}, {"text", text}}};
  for (const Image& image : images) {
    const QString file = image.path.isEmpty() ? image.name : image.path;
    parts.append(QJsonObject{{"type", "text"}, {"text", QString("[The image %1, attached here: no need to open the file.]").arg(file)}});
    const QString url = QString("data:%1;base64,%2").arg(image.type, QString::fromLatin1(image.data.toBase64()));
    parts.append(QJsonObject{{"type", "image_url"}, {"image_url", QJsonObject{{"url", url}}}});
  }
  return Message{{"role", "user"}, {"content", parts}};
}

bool hasImages(const Message& message) {
  for (const QJsonValue& part : message.value("content").toArray()) {
    if (part.toObject().value("type").toString() == "image_url") {
      return true;
    }
  }
  return false;
}

Message withoutImages(const Message& message) {
  if (!hasImages(message)) {
    return message;
  }
  Message plain = message;
  plain["content"] = messageText(message);
  return plain;
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
