#pragma once

#include "core/Diff.h"
#include "core/tools/TodoTool.h"

#include <QString>

#include <optional>

namespace Smith {

// Everything the agent loop reports to the outside world. The core never
// includes UI code: a frontend (TUI, headless printer, tests) subscribes to
// these events and renders them however it likes.

enum class BlockKind { Text, Reasoning };
enum class NoticeLevel { Info, Warn, Error };
enum class DoneReason { Stop, Interrupted, Length, MaxSteps };

struct AgentEvent {
  enum class Type {
    // Messages you typed while the agent worked, now handed to the model.
    UserMessage,
    // A streamed chunk of assistant text or reasoning.
    Delta,
    // The model finished streaming one response; flush any live text.
    ResponseEnd,
    // The model has started emitting a call to this tool (arguments still streaming).
    ToolPrepare,
    ToolStart,
    ToolEnd,
    // How full the context window is; `limit` is unknown for servers that don't report it.
    Context,
    // Background work worth showing in a status line; `compacting` while summarizing.
    Activity,
    Notice,
    // The model rewrote its checklist (todo_write), or it was cleared or restored.
    Todos,
    Done,
  };

  Type type = Type::Notice;
  QString text;
  BlockKind kind = BlockKind::Text;
  QString id;
  QString name;
  QString summary;
  QString output;
  bool isError = false;
  std::optional<Diff> diff;
  qint64 tokens = 0;
  std::optional<int> limit;
  bool compacting = false;
  NoticeLevel level = NoticeLevel::Info;
  TodoList todos;
  DoneReason reason = DoneReason::Stop;

  static AgentEvent notice(NoticeLevel level, const QString& text) {
    AgentEvent event;
    event.type = Type::Notice;
    event.level = level;
    event.text = text;
    return event;
  }

  static AgentEvent done(DoneReason reason) {
    AgentEvent event;
    event.type = Type::Done;
    event.reason = reason;
    return event;
  }

  static AgentEvent simple(Type type) {
    AgentEvent event;
    event.type = type;
    return event;
  }

  static AgentEvent activity(const QString& text, bool compacting = false) {
    AgentEvent event;
    event.type = Type::Activity;
    event.text = text;
    event.compacting = compacting;
    return event;
  }
};

inline QString doneReasonName(DoneReason reason) {
  switch (reason) {
  case DoneReason::Stop:
    return "stop";
  case DoneReason::Interrupted:
    return "interrupted";
  case DoneReason::Length:
    return "length";
  case DoneReason::MaxSteps:
    return "max_steps";
  }
  return "stop";
}

} // namespace Smith
