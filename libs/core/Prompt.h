#pragma once

#include "core/Instructions.h"
#include "core/Skills.h"
#include "core/Toolchain.h"

#include <QString>
#include <QStringList>

namespace Smith {

struct PromptExtras {
  // AGENTS.md files, most general first.
  QList<InstructionFile> instructions;
  QList<Skill> skills;
  // Faster command-line tools found on this machine, to suggest for shell commands.
  FastTools fastTools;
  // The user can be asked questions (ask_user); not in non-interactive runs.
  bool canAsk = false;
  // Where commands run and what they can reach, from the sandbox.
  QStringList sandbox;
  // The branch, when the working directory is a task worktree.
  QString worktreeBranch;
  // The task tool can hand out work (can_write), not only investigations.
  bool workers = false;
};

QString buildSystemPrompt(const QString& cwd, const PromptExtras& extras = {});

// The system prompt for sub-agents run by the task tool.
QString buildSubagentPrompt(const QString& cwd, const PromptExtras& extras = {});
// The system prompt for sub-agents sent to change things (task with can_write).
QString buildWorkerPrompt(const QString& cwd, const PromptExtras& extras = {});

} // namespace Smith
