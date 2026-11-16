#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace Smith {

struct CompletionContext {
  QString cwd;
  QStringList endpoints;
  QStringList models;
  // The repository's task worktrees, for /worktree, /land and /drop.
  QStringList tasks;
  // The project's files, for @ mentions; asked for only when one is typed.
  std::function<QStringList()> files;
};

struct Completion {
  QString value;
  QString hint;
};

// A slash command, as listed by /help and offered while typing.
struct CommandSpec {
  QString name;
  // How its argument is written in help, e.g. "[name]" or "on|off".
  QString args;
  QString description;
  // Completions for the argument, given what's typed after the command.
  std::function<QList<Completion>(const QString& typed, const CompletionContext& ctx)> complete;
};

const QList<CommandSpec>& commands();

QString commandHelp();

// One entry of the suggestion list: `text` is what the input becomes when it's chosen.
struct Suggestion {
  QString text;
  QString label;
  QString hint;
  // Whether choosing it should leave the input open for an argument rather than run it.
  bool takesArgument = false;
  // A file for an @ mention: tab and enter both put it in the input, and nothing runs.
  bool insert = false;
};

// Suggestions for what's typed so far: command names while typing the
// command ("/h" → /help), then the argument's completions once a space
// follows a command that has them ("/reasoning h" → high). In a message,
// files for the @ mention at its end ("fix @Agent" → libs/core/Agent.cpp).
QList<Suggestion> suggest(const QString& input, const CompletionContext& ctx);

// What switching plan mode on or off did, for the transcript.
QString planModeText(bool on);
// Sent for you when you choose to carry out a plan.
extern const QString GO_AHEAD;

class AllowedTools;
// /permissions: what runs without asking, as text; with "reset", forgets what the user allowed.
QString managePermissions(AllowedTools& allowed, const QString& arg);

} // namespace Smith
