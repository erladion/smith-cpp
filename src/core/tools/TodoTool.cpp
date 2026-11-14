#include "core/tools/TodoTool.h"

#include "core/Json.h"
#include "core/Tool.h"

#include <QJsonArray>
#include <QSet>

namespace Smith {

namespace {

QJsonObject todoSchema() {
  return Schema::object(
      {
          {"todos", Schema::array(Schema::object(
                                      {
                                          {"content", Schema::string("The step, as a short imperative sentence", 1)},
                                          {"status", Schema::enumeration({"pending", "in_progress", "completed"})},
                                      },
                                      {"content", "status"}),
                                  "The whole list, every time: steps left out are removed")},
      },
      {"todos"});
}

TodoStatus statusFrom(const QString& name) {
  if (name == "in_progress") {
    return TodoStatus::InProgress;
  }
  if (name == "completed") {
    return TodoStatus::Completed;
  }
  return TodoStatus::Pending;
}

TodoList todosFrom(const QJsonObject& input) {
  TodoList todos;
  for (const QJsonValue& value : input.value("todos").toArray()) {
    const QJsonObject item = value.toObject();
    todos.append(Todo{item.value("content").toString(), statusFrom(item.value("status").toString())});
  }
  return todos;
}

} // namespace

QString todoProgress(const TodoList& todos) {
  int done = 0;
  for (const Todo& todo : todos) {
    if (todo.status == TodoStatus::Completed) {
      done++;
    }
  }
  return QString("%1/%2 done").arg(done).arg(todos.size());
}

TodoList unfinished(const TodoList& todos) {
  TodoList out;
  for (const Todo& todo : todos) {
    if (todo.status != TodoStatus::Completed) {
      out.append(todo);
    }
  }
  return out;
}

QString formatTodos(const TodoList& todos) {
  QStringList lines;
  for (const Todo& todo : todos) {
    const QString mark = todo.status == TodoStatus::Completed ? "[x]" : todo.status == TodoStatus::InProgress ? "[>]" : "[ ]";
    lines << mark + " " + todo.content;
  }
  return lines.join('\n');
}

ToolDef createTodoTool(std::function<void(const TodoList&)> onUpdate) {
  ToolDef tool;
  tool.name = TODO_TOOL;
  tool.description = "Keep a checklist for a task with several steps. Write it before you start (all pending, or the first in_progress), and write it "
                     "again whenever a step starts or finishes: mark a step completed as soon as it's done and the next one in_progress, one at a "
                     "time. Send the whole list every time. Skip it for a task you can do in one or two steps.";
  tool.schema = todoSchema();
  tool.readOnly = true;
  tool.summarize = [](const QJsonObject& input) { return todoProgress(todosFrom(input)); };
  tool.run = [onUpdate](const QJsonObject& input, const ToolContext&) {
    const TodoList todos = todosFrom(input);
    onUpdate(todos);
    if (todos.isEmpty()) {
      return ToolResult{"The todo list is now empty."};
    }
    const int left = unfinished(todos).size();
    const QString next = left ? plural(left, "step") + " left. Carry on with the one in progress." : QString("Every step is done.");
    return ToolResult{QString("Todo list updated (%1):\n").arg(todoProgress(todos)) + formatTodos(todos) + "\n\n" + next};
  };
  return tool;
}

std::optional<TodoList> parseTodos(const QString& args) {
  try {
    const QJsonValue parsed = Json::parse(args.isEmpty() ? "{}" : args);
    if (validateSchema(parsed, todoSchema())) {
      return std::nullopt;
    }
    return todosFrom(parsed.toObject());
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

TodoList todosFromHistory(const Messages& messages) {
  QSet<QString> failed;
  for (const Message& message : messages) {
    if (messageRole(message) == "tool" && message.value("content").isString() && message.value("content").toString().startsWith("Error: ")) {
      failed.insert(message.value("tool_call_id").toString());
    }
  }
  for (int i = messages.size() - 1; i >= 0; i--) {
    const Message& message = messages[i];
    if (messageRole(message) != "assistant") {
      continue;
    }
    const QJsonArray calls = message.value("tool_calls").toArray();
    for (int j = calls.size() - 1; j >= 0; j--) {
      const QJsonObject call = calls[j].toObject();
      const QJsonObject function = call.value("function").toObject();
      if (function.value("name").toString() != TODO_TOOL || failed.contains(call.value("id").toString())) {
        continue;
      }
      if (const auto parsed = parseTodos(function.value("arguments").toString())) {
        return *parsed;
      }
    }
  }
  return {};
}

} // namespace Smith
