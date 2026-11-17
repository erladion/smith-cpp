#pragma once

#include "core/Tool.h"

#include <QString>
#include <QStringList>

namespace Smith {

// Tools of your own: a folder with a tool.json that says what the tool is for,
// what arguments it takes, and the command that does it.
//
//   { "description": "Look up an issue in the tracker by its number.",
//     "parameters": { "type": "object", "properties": { "id": { "type": "integer" } }, "required": ["id"] },
//     "command": "python3 \"$SMITH_TOOL_DIR/issue.py\"",
//     "readOnly": true, "timeoutMs": 30000 }
//
// The folder's name is the tool's name. The command runs with bash in the
// project folder, always in the sandbox, with the call's arguments as JSON on
// stdin and SMITH_TOOL_DIR set to the tool's folder; what it prints is the
// result, and a non-zero exit is an error.
//
// Your own tools can also have "network": true (the network for this tool,
// while other commands stay offline) and "env": ["NAME", ...] (those variables
// from Smith's environment, for this tool alone), e.g. for an issue tracker
// on the local network and its token.
struct ToolRoot {
  QString dir;
  // Yours, not the project's: only these may say readOnly, which lets a tool
  // run without asking. A project's tools could have been written by the agent.
  bool personal = false;
};

// Folders searched for tools, highest priority first.
QList<ToolRoot> toolRoots(const QString& projectRoot);

struct LoadedTools {
  ToolList tools;
  // Their folders, which the sandbox has to show.
  QStringList dirs;
  QStringList warnings;
};

// Finds tools under `roots`. A name found earlier wins; one in `reserved`
// (the built-in tools) is skipped with a warning.
LoadedTools loadCustomTools(const QList<ToolRoot>& roots, const QStringList& reserved);

// How many tool folders there are under `roots`, loadable or not.
int countCustomTools(const QList<ToolRoot>& roots);

} // namespace Smith
