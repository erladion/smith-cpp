#include "frontend/Commands.h"

#include "core/Dirs.h"
#include "core/Reasoning.h"
#include "frontend/Folders.h"

#include <QDir>

namespace Smith {

namespace {

std::function<QList<Completion>(const QString&, const CompletionContext&)> fixed(const QList<Completion>& items) {
  return [items](const QString& typed, const CompletionContext&) {
    QList<Completion> out;
    for (const Completion& item : items) {
      if (item.value.startsWith(typed)) {
        out.append(item);
      }
    }
    return out;
  };
}

// Folder completions for a partly typed path, relative to `cwd`, absolute, or with ~/.
QList<Completion> completeFolder(const QString& typed, const QString& cwd) {
  const int slash = typed.lastIndexOf('/');
  const QString dirPart = slash == -1 ? QString() : typed.left(slash + 1);
  const QString prefix = slash == -1 ? typed : typed.mid(slash + 1);
  const QString expanded = expandHome(dirPart);
  const QString base = QDir::cleanPath(QDir(cwd).absoluteFilePath(expanded.isEmpty() ? QString(".") : expanded));
  QList<Completion> out;
  for (const QString& name : listFolders(base, prefix.startsWith('.')).names) {
    if (name.startsWith(prefix)) {
      out.append(Completion{dirPart + name + "/", {}});
    }
    if (out.size() >= 50) {
      break;
    }
  }
  return out;
}

} // namespace

const QList<CommandSpec>& commands() {
  static const QList<CommandSpec> list = []() {
    QList<Completion> reasoning;
    for (const QString& level : REASONING_LEVELS) {
      reasoning.append(Completion{level, reasoningHint(level)});
    }
    return QList<CommandSpec>{
        {"/help", {}, "show the commands and keys", {}},
        {"/endpoint", "[name]", "pick a configured endpoint, or switch to one by name",
         [](const QString& typed, const CompletionContext& ctx) {
           QList<Completion> out;
           for (const QString& name : ctx.endpoints) {
             if (name.startsWith(typed)) {
               out.append(Completion{name, {}});
             }
           }
           return out;
         }},
        {"/model", "[name]", "pick a model from the server, or switch to one by name",
         [](const QString& typed, const CompletionContext& ctx) {
           QList<Completion> out;
           for (const QString& model : ctx.models) {
             if (model.contains(typed, Qt::CaseInsensitive)) {
               out.append(Completion{model, {}});
             }
           }
           return out;
         }},
        {"/reasoning", "[level]", "pick how much the model thinks: " + REASONING_LEVELS.join(", "), fixed(reasoning)},
        {"/sessions", "[all]", "switch to another session from this folder (all: from every folder)",
         fixed({Completion{"all", "sessions from every folder"}})},
        {"/name", "[name]", "show or set the current session's name", {}},
        {"/open", {}, "pick a project folder: a recent one, or browse to one", {}},
        {"/new", "[folder]", "start a new session, here or in another folder",
         [](const QString& typed, const CompletionContext& ctx) { return completeFolder(typed, ctx.cwd); }},
        {"/clear", {}, "start a new session (same as /new)", {}},
        {"/undo", {}, "undo the last turn: restore files it changed and edit your message again", {}},
        {"/skills", {}, "list the available skills", {}},
        {"/network", "on|off", "allow or block network access for sandboxed commands",
         fixed({Completion{"on", "allow network access"}, Completion{"off", "block network access"}})},
        {"/jobs", "[stop id|all]", "list the commands the agent runs in the background, or stop one or all",
         fixed({Completion{"stop all", "stop every background job"}})},
        {"/compact", {}, "summarize the conversation now to free context", {}},
        {"/exit", {}, "quit", {}},
    };
  }();
  return list;
}

QString commandHelp() {
  int width = 0;
  for (const CommandSpec& command : commands()) {
    width = std::max(width, int(QString("%1 %2").arg(command.name, command.args).size()));
  }
  width += 2;
  QStringList lines;
  for (const CommandSpec& command : commands()) {
    lines << "  " + QString("%1 %2").arg(command.name, command.args).leftJustified(width) + command.description;
  }
  return "Commands (type / to see them as you type; tab completes, enter runs):\n" + lines.join('\n') +
         "\nKeys: enter sends · shift+enter, \\ then enter, or ctrl+j adds a new line · esc interrupts the agent\n"
         "      ↑/↓ recall earlier input · ctrl+c quits";
}

QList<Suggestion> suggest(const QString& input, const CompletionContext& ctx) {
  if (!input.startsWith('/') || input.contains('\n')) {
    return {};
  }
  const int space = input.indexOf(' ');
  QList<Suggestion> out;
  if (space == -1) {
    const QString typed = input.toLower();
    auto add = [&](const CommandSpec& command) {
      out.append(Suggestion{command.name, command.args.isEmpty() ? command.name : command.name + " " + command.args, command.description, false});
    };
    for (const CommandSpec& command : commands()) {
      if (command.name.startsWith(typed)) {
        add(command);
      }
    }
    for (const CommandSpec& command : commands()) {
      if (!command.name.startsWith(typed) && command.name.contains(typed.mid(1))) {
        add(command);
      }
    }
    return out;
  }

  const QString name = input.left(space);
  for (const CommandSpec& command : commands()) {
    if (command.name != name || !command.complete) {
      continue;
    }
    for (const Completion& completion : command.complete(input.mid(space + 1), ctx)) {
      // A folder can be gone into further; other arguments are complete.
      out.append(Suggestion{command.name + " " + completion.value, completion.value, completion.hint, completion.value.endsWith('/')});
    }
  }
  return out;
}

} // namespace Smith
