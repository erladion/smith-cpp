#pragma once

#include "core/Cancel.h"

#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace Smith {

// How to start a program: directly, or wrapped in a sandbox.
struct SpawnSpec {
  QString file;
  QStringList args;
  QProcessEnvironment env;
  // Where to start it on this machine, when that matters (unsandboxed); a sandbox changes folder itself.
  QString cwd;
  // Stops the command where killing `file`'s process group isn't enough (inside a container).
  std::function<void()> kill;
};

struct RunOptions {
  // Written to stdin, which is then closed; without it stdin is /dev/null.
  std::optional<QByteArray> input;
  // stdout and stderr together, in the order they were written.
  bool mergeChannels = false;
  // Kill the process group after this long; 0 for no limit.
  int timeoutMs = 0;
  CancelPtr cancel;
  // Stop keeping output beyond this many bytes; -1 for no limit.
  qint64 maxCapture = -1;
  // Past maxCapture, keep the first and the last half of stdout instead of only the start: the end of a long build is where it says what failed.
  bool keepEnd = false;
  // Kill the process once maxCapture is reached (for listings that can be cut short).
  bool stopAtCapture = false;
  // Used when the spec has no cwd of its own.
  QString cwd;
};

struct ProcessResult {
  enum class Stop { None, Timeout, Interrupt, Capture };
  bool started = true;
  // The program doesn't exist.
  bool missing = false;
  QString startError;
  // Exit code; -1 when a signal ended it.
  int exitCode = -1;
  bool crashed = false;
  QByteArray out;
  QByteArray err;
  // With keepEnd: how many bytes were left out of `out`, and where in it.
  qint64 omitted = 0;
  qint64 omittedAt = 0;
  Stop stoppedBy = Stop::None;
  // The process id it ran with, for reports; 0 if it never started.
  qint64 pid = 0;
};

// Runs a program to completion in its own process group, so killing it on
// timeout or interrupt also stops everything it started.
ProcessResult runProcess(const SpawnSpec& spec, const RunOptions& options = {});

// Kills a process group started by runProcess or BackgroundJobs.
void killProcessGroup(qint64 pid);

// Finds an executable on PATH without running it; empty if not found.
QString which(const QString& name);

} // namespace Smith
