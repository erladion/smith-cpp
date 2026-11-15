#include "core/Process.h"

#include "core/SessionProcess.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>

#include <signal.h>

namespace Smith {

void killProcessGroup(qint64 pid) {
  if (pid > 0) {
    ::kill(-pid_t(pid), SIGKILL);
  }
}

QString which(const QString& name) {
  return QStandardPaths::findExecutable(name);
}

ProcessResult runProcess(const SpawnSpec& spec, const RunOptions& options) {
  ProcessResult result;
  SessionProcess process;
  process.setProgram(spec.file);
  process.setArguments(spec.args);
  process.setProcessEnvironment(spec.env.isEmpty() ? QProcessEnvironment::systemEnvironment() : spec.env);
  const QString cwd = spec.cwd.isEmpty() ? options.cwd : spec.cwd;
  if (!cwd.isEmpty()) {
    process.setWorkingDirectory(cwd);
  }
  process.setProcessChannelMode(options.mergeChannels ? QProcess::MergedChannels : QProcess::SeparateChannels);
  if (!options.input) {
    process.setStandardInputFile(QProcess::nullDevice());
  }
  process.start();
  if (!process.waitForStarted(30000)) {
    result.started = false;
    result.startError = process.errorString();
    const bool resolvable = spec.file.contains('/') ? QFileInfo(spec.file).isExecutable() : !which(spec.file).isEmpty();
    result.missing = !resolvable;
    return result;
  }
  result.pid = process.processId();

  if (options.input) {
    process.write(*options.input);
    process.closeWriteChannel();
  }

  QElapsedTimer clock;
  clock.start();
  const qint64 half = options.maxCapture / 2;
  QByteArray tail;
  auto collect = [&]() {
    const QByteArray out = process.readAllStandardOutput();
    const QByteArray err = options.mergeChannels ? QByteArray() : process.readAllStandardError();
    if (options.keepEnd && options.maxCapture >= 0) {
      // The first half fills up once; the rest goes through `tail`, cut back to the last half as it grows.
      const qint64 room = std::max<qint64>(0, half - result.out.size());
      result.out += out.left(room);
      tail += out.mid(room);
      if (tail.size() > 2 * half) {
        result.omitted += tail.size() - half;
        tail = tail.right(half);
      }
    } else if (options.maxCapture < 0 || result.out.size() < options.maxCapture) {
      result.out += out;
    }
    if (options.maxCapture < 0 || result.err.size() < options.maxCapture) {
      result.err += err;
    }
  };
  auto stop = [&](ProcessResult::Stop reason) {
    if (result.stoppedBy == ProcessResult::Stop::None) {
      result.stoppedBy = reason;
    }
    killProcessGroup(result.pid);
    if (spec.kill) {
      spec.kill();
    }
  };

  while (process.state() != QProcess::NotRunning) {
    process.waitForReadyRead(50);
    collect();
    if (result.stoppedBy != ProcessResult::Stop::None) {
      // Already killed; wait for it to go.
      process.waitForFinished(100);
      continue;
    }
    if (cancelled(options.cancel)) {
      stop(ProcessResult::Stop::Interrupt);
    } else if (options.timeoutMs > 0 && clock.elapsed() > options.timeoutMs) {
      stop(ProcessResult::Stop::Timeout);
    } else if (options.stopAtCapture && options.maxCapture >= 0 && result.out.size() >= options.maxCapture) {
      stop(ProcessResult::Stop::Capture);
    }
  }
  process.waitForFinished(5000);
  collect();
  if (tail.size() > half) {
    result.omitted += tail.size() - half;
    tail = tail.right(half);
  }
  result.omittedAt = result.out.size();
  result.out += tail;
  result.crashed = process.exitStatus() == QProcess::CrashExit;
  result.exitCode = result.crashed ? -1 : process.exitCode();
  return result;
}

} // namespace Smith
