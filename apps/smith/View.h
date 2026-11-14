#pragma once

#include "core/Agent.h"
#include "core/Config.h"
#include "core/tools/Tools.h"
#include "frontend/Activity.h"
#include "tui/Style.h"

#include <functional>
#include <optional>

namespace Smith::Ui {

// Parts of the live region that are drawn from plain data.

struct Spinner {
  QStringList frames;
  Tui::Color color;
};

const Spinner& spinnerFor(Theme theme);

// The "Allow bash?" box, with the change previewed in as many of the screen's `rows` as fit.
QList<Tui::Line> approvalBox(const ApprovalRequest& request, int width, int rows);

// The agent's question, its suggested answers (the `selected` one highlighted), and the keys.
QList<Tui::Line> questionBox(const Question& question, int selected, int width);

struct Status {
  QString endpoint;
  QString model;
  QString reasoning;
  std::optional<qint64> tokens;
  std::optional<int> limit;
  bool sandboxActive = false;
  SandboxType sandboxType = SandboxType::Off;
  bool network = false;
  // Working: esc interrupts.
  bool busy = false;
  // An agent turn is running, so enter queues a message.
  bool queueing = false;
  // Commands running in the background.
  int jobs = 0;
  // The branch, when working in a task worktree.
  QString worktreeBranch;
  // Fills spare room at the right end with this many columns of something, e.g. a code stream.
  std::function<QString(int)> filler;
};

// The bottom line: endpoint, model, reasoning, context use, sandbox, and what keys do now.
Tui::Line statusLine(const Status& status, int width);

} // namespace Smith::Ui
