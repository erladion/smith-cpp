#pragma once

#include <QList>
#include <QString>

namespace Smith {

struct InstructionFile {
  QString path;
  QString content;
};

// The repository root containing `cwd` (the nearest folder with .git); empty if none.
QString findRepoRoot(const QString& cwd);

// Instruction files for the system prompt, most general first:
// ~/.config/smith/AGENTS.md, then AGENTS.md (or CLAUDE.md) in each folder
// from the repository root down to `cwd`.
QList<InstructionFile> loadInstructions(const QString& cwd);

} // namespace Smith
