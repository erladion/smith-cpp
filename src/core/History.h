#pragma once

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
Message toolMessage(const QString& callId, const QString& content);

// Gives every assistant tool call a matching tool result. A session saved
// mid-turn (crash, kill) can end with unanswered calls, which servers reject.
Messages repairHistory(const Messages& messages);

} // namespace Smith
