#include "frontend/Commands.h"

#include "core/Themes.h"
#include "core/Dirs.h"
#include "core/Permissions.h"
#include "core/Tool.h"
#include "core/Reasoning.h"
#include "frontend/Folders.h"

#include <QDir>
#include <QRegularExpression>

#include <algorithm>

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

QList<Completion> completeTask(const QString& typed, const CompletionContext& ctx) {
  QList<Completion> out;
  for (const QString& task : ctx.tasks) {
    if (task.startsWith(typed)) {
      out.append(Completion{task, "smith/" + task});
    }
  }
  return out;
}

constexpr int MAX_MENTION_SUGGESTIONS = 50;

// Files for the @ mention being typed at the end of `input`: those whose path
// starts with what's typed, then those whose name does, then those that
// contain it anywhere; shorter paths first in each.
QList<Suggestion> mentionSuggestions(const QString& input, const CompletionContext& ctx) {
  static const QRegularExpression typing(R"re((?:^|(?<=\s))@"?([^\s"]*)$)re");
  const QRegularExpressionMatch match = typing.match(input);
  if (!match.hasMatch() || !ctx.files) {
    return {};
  }
  const QString typed = match.captured(1);
  const QString before = input.left(match.capturedStart());
  QList<QStringList> ranks(3);
  for (const QString& file : ctx.files()) {
    const QString name = file.mid(file.lastIndexOf('/') + 1);
    if (file.startsWith(typed, Qt::CaseInsensitive)) {
      ranks[0] << file;
    } else if (name.startsWith(typed, Qt::CaseInsensitive)) {
      ranks[1] << file;
    } else if (file.contains(typed, Qt::CaseInsensitive)) {
      ranks[2] << file;
    }
  }
  QList<Suggestion> out;
  for (QStringList& rank : ranks) {
    std::stable_sort(rank.begin(), rank.end(), [](const QString& a, const QString& b) { return a.size() < b.size(); });
    for (const QString& file : rank) {
      if (out.size() >= MAX_MENTION_SUGGESTIONS) {
        return out;
      }
      const QString written = file.contains(' ') ? "\"" + file + "\"" : file;
      Suggestion suggestion{before + "@" + written + " ", file, {}, false};
      suggestion.insert = true;
      out.append(suggestion);
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
        {"/theme", "[name]", "pick how Smith looks: " + THEME_NAMES.join(", ") + ", or a theme of yours",
         // Read each time, so a new theme file shows without a restart.
         [](const QString& typed, const CompletionContext&) {
           QList<Completion> out;
           for (const ThemeSpec& theme : listThemes().themes) {
             if (theme.name.startsWith(typed)) {
               out.append(Completion{theme.name, themeHint(theme)});
             }
           }
           return out;
         }},
        {"/sessions", "[all]", "switch to another session from this folder (all: from every folder)",
         fixed({Completion{"all", "sessions from every folder"}})},
        {"/name", "[name]", "show or set the current session's name", {}},
        {"/open", {}, "pick a project folder: a recent one, or browse to one", {}},
        {"/new", "[folder]", "start a new session, here or in another folder",
         [](const QString& typed, const CompletionContext& ctx) { return completeFolder(typed, ctx.cwd); }},
        {"/clear", {}, "start a new session (same as /new)", {}},
        {"/worktree", "<task>", "give a task its own worktree: the agent works on a smith/<task> branch beside your checkout", completeTask},
        {"/worktrees", {}, "list the task worktrees: switch to one, land it or drop it", {}},
        {"/land", "[task]", "commit a task's changes and bring them to your branch (cherry-pick or merge)", completeTask},
        {"/drop", "[task]", "remove a task's worktree and branch, discarding what isn't landed", completeTask},
        {"/plan", "[on|off]", "plan mode: the agent reads and plans but changes nothing until you switch it off",
         fixed({Completion{"on", "read and plan only"}, Completion{"off", "let the agent change files and run commands"}})},
        {"/diff", {}, "show what the agent changed in files this session, file by file", {}},
        {"/undo", {}, "undo the last turn: restore files it changed and edit your message again", {}},
        {"/skills", {}, "list the available skills", {}},
        {"/network", "on|off", "allow or block network access for sandboxed commands",
         fixed({Completion{"on", "allow network access"}, Completion{"off", "block network access"}})},
        {"/permissions", "[reset]", "show what runs here without asking, or forget what you allowed",
         fixed({Completion{"reset", "forget what you allowed in this project"}})},
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
         "      tab, while the agent works, queues the next task · shift+tab switches plan mode\n"
         "      ↑/↓ recall earlier input · ctrl+c quits";
}

QList<Suggestion> suggest(const QString& input, const CompletionContext& ctx) {
  if (!input.startsWith('/')) {
    return mentionSuggestions(input, ctx);
  }
  if (input.contains('\n')) {
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

const QString GO_AHEAD = "Go ahead with the plan.";

QString planModeText(bool on) {
  return on ? QString("Plan mode on: the agent reads and plans, but changes nothing. Switch it off (/plan or shift+tab) to let it carry out the plan.")
            : QString("Plan mode off: the agent can change files and run commands again.");
}

QString managePermissions(AllowedTools& allowed, const QString& arg) {
  if (arg == "reset") {
    const int count = allowed.remembered().size();
    allowed.forget();
    return count ? QString("Forgot %1 you allowed in this project. Those tools ask again.").arg(plural(count, "rule")) : QString("There was nothing to forget.");
  }
  auto shown = [](const QString& rule) {
    const int colon = rule.indexOf(':');
    return colon < 0 ? rule : QString("%1 commands starting with \"%2\"").arg(rule.left(colon), rule.mid(colon + 1));
  };
  auto list = [&](const QStringList& rules) {
    QStringList lines;
    for (const QString& rule : rules) {
      lines << "  " + shown(rule);
    }
    return lines.join('\n');
  };
  QStringList parts;
  if (!allowed.remembered().isEmpty()) {
    parts << QString("You allowed these %1 (/permissions reset forgets them):\n").arg(allowed.remembers() ? "in this project" : "for this session") +
                 list(allowed.remembered());
  }
  if (!allowed.configured().isEmpty()) {
    parts << "Allowed by the config file (permissions.allow):\n" + list(allowed.configured());
  }
  if (!allowed.denied().isEmpty()) {
    parts << "Refused by the config file (permissions.deny):\n" + list(allowed.denied());
  }
  if (parts.isEmpty()) {
    return "Nothing runs without asking, apart from tools that only read. Answer a prompt with \"always\" to allow a tool or a kind of command, "
           "or list rules under permissions.allow in the config file.";
  }
  return parts.join("\n\n");
}

} // namespace Smith
