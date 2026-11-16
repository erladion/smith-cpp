#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace Smith {

// A chat message in the OpenAI format: {"role": ..., "content": ..., "tool_calls": [...], ...}.
using Message = QJsonObject;
using Messages = QList<Message>;

// How the conversation changed; persisted by the session log.
struct HistoryChange {
  enum class Type { Append, Replace };
  Type type = Type::Append;
  Message message;
  Messages messages;
};

QString messageRole(const Message& message);

// Plain text of a message's content, whether it is a string or content parts.
QString messageText(const Message& message);

Message userMessage(const QString& text);

// A picture sent along with a user message.
struct Image {
  QString name;
  // "image/png" and the like.
  QString type;
  QByteArray data;
  // The file it came from, named to the model with the picture.
  QString path;
};
// A user message with pictures: content parts, the text first, then each
// picture after a line naming its file, so the model knows it already has the
// file the text names and doesn't go and open it.
Message userMessage(const QString& text, const QList<Image>& images);
// Whether a message carries pictures, and the same message without them.
bool hasImages(const Message& message);
Message withoutImages(const Message& message);
Message toolMessage(const QString& callId, const QString& content);

// Gives every assistant tool call a matching tool result. A session saved
// mid-turn (crash, kill) can end with unanswered calls, which servers reject.
Messages repairHistory(const Messages& messages);

} // namespace Smith
