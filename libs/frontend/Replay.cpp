#include "frontend/Replay.h"

#include "core/Agent.h"
#include "core/Context.h"

#include <QJsonArray>
#include <QMap>

namespace Smith {

QList<Entry> historyToEntries(const Messages& messages, const ToolList& tools) {
  QMap<QString, QString> results;
  for (const Message& message : messages) {
    if (messageRole(message) == "tool") {
      results.insert(message.value("tool_call_id").toString(), messageText(message));
    }
  }

  QList<Entry> entries;
  for (const Message& message : messages) {
    const QString role = messageRole(message);
    if (role == "user") {
      const QString text = messageText(message);
      if (text.startsWith(SUMMARY_PREFIX)) {
        entries.append(Entry::notice(NoticeLevel::Info, "(Earlier conversation was summarized to save context.)"));
        const int separator = text.indexOf("\n\n---\n\n");
        if (separator != -1) {
          entries.append(Entry::simple(Entry::Kind::User, text.mid(separator + 7)));
        }
      } else if (text.startsWith(TODO_REMINDER)) {
        entries.append(Entry::notice(NoticeLevel::Info, "Reminded the agent of its unfinished todos."));
      } else if (text.startsWith(SHOWN_IMAGES)) {
        // The read_file calls that opened them are there already.
      } else {
        entries.append(Entry::simple(Entry::Kind::User, text));
      }
    } else if (role == "assistant") {
      const QString text = messageText(message).trimmed();
      if (!text.isEmpty()) {
        entries.append(Entry::simple(Entry::Kind::Text, text));
      }
      for (const QJsonValue& value : message.value("tool_calls").toArray()) {
        const QJsonObject call = value.toObject();
        const QJsonObject function = call.value("function").toObject();
        const QString name = function.value("name").toString();
        const QString args = function.value("arguments").toString();
        const QString output = results.value(call.value("id").toString());
        const bool isError = output.startsWith("Error: ");
        if (name == TODO_TOOL && !isError) {
          if (const auto todos = parseTodos(args)) {
            Entry entry;
            entry.kind = Entry::Kind::Todos;
            entry.todos = *todos;
            entries.append(entry);
            continue;
          }
        }
        Entry entry;
        entry.kind = Entry::Kind::Tool;
        entry.name = name;
        entry.summary = describeCall(tools, name, args);
        entry.output = isError ? output.mid(7) : output;
        entry.isError = isError;
        entries.append(entry);
      }
    }
  }
  return entries;
}

} // namespace Smith
