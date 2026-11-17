#pragma once

#include "core/History.h"

#include <QList>
#include <QString>

#include <functional>
#include <optional>

namespace Smith {

struct ToolDef;

// Waiting: on the user, for something only they can do (a manual test, a decision).
enum class TodoStatus { Pending, InProgress, Waiting, Completed };

struct Todo {
  QString content;
  TodoStatus status = TodoStatus::Pending;

  bool operator==(const Todo& other) const = default;
};

using TodoList = QList<Todo>;

inline const QString TODO_TOOL = QStringLiteral("todo_write");

// The status as todo_write writes it: "pending", "in_progress", "waiting", "completed".
QString todoStatusName(TodoStatus status);
// "2/5 done", for summaries.
QString todoProgress(const TodoList& todos);
TodoList unfinished(const TodoList& todos);
// The unfinished steps the agent can do itself: not waiting on the user.
TodoList actionable(const TodoList& todos);
// The list as plain text, for the model.
QString formatTodos(const TodoList& todos);

// The todo_write tool: the model keeps a checklist; `onUpdate` gets each new list.
ToolDef createTodoTool(std::function<void(const TodoList&)> onUpdate);

// The list from the last successful todo_write in a conversation, e.g. a resumed one.
TodoList todosFromHistory(const Messages& messages);
// The todos in a todo_write call's arguments, or nullopt if they aren't valid.
std::optional<TodoList> parseTodos(const QString& args);

} // namespace Smith
