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
  // The text comes first; the parts after it are pictures, their labels and notes.
  const QJsonArray parts = content.toArray();
  return parts.isEmpty() ? QString() : parts.first().toObject().value("text").toString();
}

Message userMessage(const QString& text) {
  return Message{{"role", "user"}, {"content", text}};
}

namespace {

const QString IMAGE_LABEL = "[The image %1, attached here: no need to open the file.]";

} // namespace

Message userMessage(const QString& text, const QList<Image>& images, const QList<AttachedFile>& files) {
  if (images.isEmpty() && files.isEmpty()) {
    return userMessage(text);
  }
  QJsonArray parts{QJsonObject{{"type", "text"}, {"text", text}}};
  for (const AttachedFile& file : files) {
    const QString note = file.cut ? QString("only its start: read_file shows the rest") : QString("no need to read it again");
    parts.append(QJsonObject{
        {"type", "text"},
        {"text", QString("[The user attached %1, with line numbers as read_file shows them; %2.]\n<file path=\"%1\">\n%3\n</file>")
                     .arg(file.shown, note, file.content)},
    });
  }
  for (const Image& image : images) {
    const QString file = image.path.isEmpty() ? image.name : image.path;
    parts.append(QJsonObject{{"type", "text"}, {"text", IMAGE_LABEL.arg(file)}});
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
  // Attached files stay; the pictures and the lines naming them go.
  const QString label = IMAGE_LABEL.left(IMAGE_LABEL.indexOf("%1"));
  QJsonArray kept;
  for (const QJsonValue& part : message.value("content").toArray()) {
    const QJsonObject object = part.toObject();
    if (object.value("type").toString() == "text" && !object.value("text").toString().startsWith(label)) {
      kept.append(object);
    }
  }
  Message plain = message;
  plain["content"] = kept.size() == 1 ? QJsonValue(messageText(message)) : QJsonValue(kept);
  return plain;
}

Message withNote(const Message& message, const QString& note) {
  const QJsonValue content = message.value("content");
  QJsonArray parts = content.isArray() ? content.toArray() : QJsonArray{QJsonObject{{"type", "text"}, {"text", content.toString()}}};
  parts.append(QJsonObject{{"type", "text"}, {"text", note}});
  Message noted = message;
  noted["content"] = parts;
  return noted;
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
