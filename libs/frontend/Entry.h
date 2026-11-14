#pragma once

#include "core/Diff.h"
#include "core/Events.h"
#include "core/tools/TodoTool.h"

#include <QStringList>

#include <optional>

namespace Smith {

// Something shown in the transcript, whichever frontend shows it.
struct Entry {
  enum class Kind {
    Banner,
    User,
    // Assistant text, rendered as Markdown.
    Text,
    Reasoning,
    Tool,
    Notice,
    // Quiet information, e.g. how long a turn took.
    Meta,
    // Agent Smith remarking on something, directly under the entry before it.
    Quote,
    // The agent's checklist, after it changed.
    Todos,
  };

  Kind kind = Kind::Notice;
  QString text;
  NoticeLevel level = NoticeLevel::Info;

  // Banner.
  QString model;
  QString server;
  QString cwd;
  QString session;
  bool resumed = false;
  bool sandboxActive = false;
  QString sandboxDescription;
  QStringList instructions;
  QStringList skills;
  QString greeting;

  // Tool.
  QString name;
  QString summary;
  QString output;
  bool isError = false;
  std::optional<Diff> diff;

  TodoList todos;

  static Entry notice(NoticeLevel level, const QString& text);
  static Entry simple(Kind kind, const QString& text);
};

} // namespace Smith
