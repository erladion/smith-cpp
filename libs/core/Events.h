#pragma once

#include "core/Diff.h"
#include "core/tools/TodoTool.h"

#include <QString>

#include <optional>
#include <variant>

namespace Smith {

// Everything the agent loop reports to the outside world. The core never
// includes UI code: a frontend (TUI, headless printer, tests) subscribes to
// these events and renders them however it likes.

enum class BlockKind { Text, Reasoning };
enum class NoticeLevel { Info, Warn, Error };
enum class DoneReason { Stop, Interrupted, Length, MaxSteps };

// One struct per kind of event, each with only what that event carries.
namespace Event {

// Messages you typed while the agent worked, now handed to the model.
struct UserMessage {
  QString text;
};

// A streamed chunk of assistant text or reasoning.
struct Delta {
  BlockKind kind = BlockKind::Text;
  QString text;
};

// The model finished streaming one response; flush any live text.
struct ResponseEnd {};

// The model has started emitting a call to this tool (arguments still streaming).
struct ToolPrepare {
  QString name;
};

struct ToolStart {
  QString id;
  QString name;
  QString summary;
};

struct ToolEnd {
  QString id;
  QString name;
  QString summary;
  QString output;
  bool isError = false;
  std::optional<Diff> diff;
};

// How full the context window is; `limit` is unknown for servers that don't report it.
struct Context {
  qint64 tokens = 0;
  std::optional<int> limit;
};

// Background work worth showing in a status line; `compacting` while summarizing.
struct Activity {
  QString text;
  bool compacting = false;
};

struct Notice {
  NoticeLevel level = NoticeLevel::Info;
  QString text;
};

// The model rewrote its checklist (todo_write), or it was cleared or restored.
struct Todos {
  TodoList todos;
};

struct Done {
  DoneReason reason = DoneReason::Stop;
};

// Tokens one model call used, as the server reported them. `generationMs` is
// how long the reply took to stream, from its first token; 0 when unknown.
struct Usage {
  qint64 promptTokens = 0;
  qint64 completionTokens = 0;
  qint64 generationMs = 0;
};

} // namespace Event

using AgentEvent = std::variant<Event::UserMessage, Event::Delta, Event::ResponseEnd, Event::ToolPrepare, Event::ToolStart, Event::ToolEnd,
                                Event::Context, Event::Activity, Event::Notice, Event::Todos, Event::Done, Event::Usage>;

// For std::visit over an AgentEvent: one lambda per event, e.g.
//   std::visit(Overloaded{[](const Event::Delta& delta) { ... }, [](const auto&) {}}, event);
template <typename... Handlers>
struct Overloaded : Handlers... {
  using Handlers::operator()...;
};
template <typename... Handlers>
Overloaded(Handlers...) -> Overloaded<Handlers...>;

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
