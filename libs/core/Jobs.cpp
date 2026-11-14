#include "core/Jobs.h"

#include "core/Errors.h"
#include "core/SessionProcess.h"
#include "compat/Utf8Decoder.h"

#include <QDeadlineTimer>
#include <QMutexLocker>

#include <signal.h>

namespace Smith {

namespace {

// Keep at most this much of a job's output; older output is dropped.
constexpr qint64 MAX_BUFFER_CHARS = 1000000;

QString signalName(int number) {
  switch (number) {
  case SIGKILL:
    return "SIGKILL";
  case SIGTERM:
    return "SIGTERM";
  case SIGINT:
    return "SIGINT";
  case SIGHUP:
    return "SIGHUP";
  case SIGSEGV:
    return "SIGSEGV";
  case SIGABRT:
    return "SIGABRT";
  case SIGPIPE:
    return "SIGPIPE";
  default:
    return QString("signal %1").arg(number);
  }
}

} // namespace

struct BackgroundJobs::Job {
  QString id;
  QString command;
  bool running = true;
  std::optional<int> exitCode;
  QString signal;
  QDateTime started;
  SpawnSpec spec;
  QProcess* process = nullptr;
  qint64 pid = 0;
  Utf8Decoder decoder;
  QString output;
  // Characters dropped from the start of `output` to keep it small.
  qint64 dropped = 0;
  // How far the model has read, counted from the very start of the output.
  qint64 read = 0;
};

QString describeJob(const JobStatus& job) {
  if (job.running) {
    return "running";
  }
  if (!job.signal.isEmpty()) {
    return QString("stopped (%1)").arg(job.signal);
  }
  return QString("exited with code %1").arg(job.exitCode.value_or(-1));
}

BackgroundJobs::BackgroundJobs() {
  m_thread.setObjectName("smith-jobs");
  m_pHome = new QObject;
  m_pHome->moveToThread(&m_thread);
  m_thread.start();
}

BackgroundJobs::~BackgroundJobs() {
  stopAll();
  QObject* home = m_pHome;
  QMetaObject::invokeMethod(home, [home]() { delete home; }, Qt::BlockingQueuedConnection);
  m_thread.quit();
  m_thread.wait();
}

JobStatus BackgroundJobs::start(const QString& command, const SpawnSpec& spec, const QString& cwd) {
  auto job = std::make_shared<Job>();
  job->command = command;
  job->spec = spec;
  job->started = QDateTime::currentDateTime();
  {
    QMutexLocker lock(&m_mutex);
    job->id = QString("bg%1").arg(m_next++);
    m_jobs.insert(job->id, job);
    m_order << job->id;
  }
  if (QThread::currentThread() == &m_thread) {
    startOnThread(job, cwd);
  } else {
    QMetaObject::invokeMethod(m_pHome, [this, job, cwd]() { startOnThread(job, cwd); }, Qt::BlockingQueuedConnection);
  }
  Q_EMIT changed();
  QMutexLocker lock(&m_mutex);
  return statusOf(*job);
}

void BackgroundJobs::startOnThread(const std::shared_ptr<Job>& job, const QString& cwd) {
  // Its own process group, so a stop ends everything it spawned.
  auto* process = new SessionProcess(m_pHome);
  process->setProgram(job->spec.file);
  process->setArguments(job->spec.args);
  process->setProcessEnvironment(job->spec.env.isEmpty() ? QProcessEnvironment::systemEnvironment() : job->spec.env);
  process->setWorkingDirectory(job->spec.cwd.isEmpty() ? cwd : job->spec.cwd);
  process->setStandardInputFile(QProcess::nullDevice());
  process->setProcessChannelMode(QProcess::MergedChannels);
  job->process = process;

  auto collect = [this, job](const QString& text) {
    QMutexLocker lock(&m_mutex);
    job->output += text;
    const qint64 excess = job->output.size() - MAX_BUFFER_CHARS;
    if (excess > 0) {
      job->output.remove(0, excess);
      job->dropped += excess;
    }
    m_wake.wakeAll();
  };
  auto end = [this, job](std::optional<int> code, const QString& signal) {
    {
      QMutexLocker lock(&m_mutex);
      if (!job->running) {
        return;
      }
      job->running = false;
      job->exitCode = code;
      job->signal = signal;
      m_wake.wakeAll();
    }
    Q_EMIT changed();
  };

  QObject::connect(process, &QProcess::readyReadStandardOutput, process, [process, job, collect]() {
    collect(job->decoder.decode(process->readAllStandardOutput()));
  });
  QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process, [process, job, collect, end](int code, QProcess::ExitStatus status) {
    collect(job->decoder.decode(process->readAllStandardOutput()));
    if (status == QProcess::CrashExit) {
      end(std::nullopt, signalName(code));
    } else {
      end(code, {});
    }
  });
  QObject::connect(process, &QProcess::errorOccurred, process, [job, collect, end](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      collect(QString("Failed to start %1: %2\n").arg(job->spec.file, job->process->errorString()));
      end(127, {});
    }
  });
  process->start();
  if (process->waitForStarted(30000)) {
    QMutexLocker lock(&m_mutex);
    job->pid = process->processId();
  }
}

QList<JobStatus> BackgroundJobs::list() const {
  QMutexLocker lock(&m_mutex);
  QList<JobStatus> out;
  for (const QString& id : m_order) {
    out << statusOf(*m_jobs.value(id));
  }
  return out;
}

int BackgroundJobs::running() const {
  QMutexLocker lock(&m_mutex);
  int count = 0;
  for (const auto& job : m_jobs) {
    if (job->running) {
      count++;
    }
  }
  return count;
}

std::shared_ptr<BackgroundJobs::Job> BackgroundJobs::find(const QString& id) const {
  const auto job = m_jobs.value(id);
  if (!job) {
    const QString known = m_order.isEmpty() ? QString("None have been started.") : QString("Jobs: %1.").arg(m_order.join(", "));
    throw SmithError(QString("No background job %1. %2").arg(id, known));
  }
  return job;
}

BackgroundJobs::ReadResult BackgroundJobs::read(const QString& id, int waitMs, const std::optional<QRegularExpression>& until, const CancelPtr& cancel,
                                                bool peek) {
  QMutexLocker lock(&m_mutex);
  const auto job = find(id);
  auto fresh = [&]() { return job->output.mid(std::max<qint64>(0, job->read - job->dropped)); };
  auto matches = [&]() { return until && until->match(fresh()).hasMatch(); };

  bool matched = matches();
  if (waitMs > 0 && job->running && !matched) {
    QDeadlineTimer deadline(waitMs);
    while (job->running && !deadline.hasExpired() && !cancelled(cancel)) {
      m_wake.wait(&m_mutex, QDeadlineTimer(std::min<qint64>(100, deadline.remainingTime())));
      if (matches()) {
        break;
      }
    }
    matched = matches();
  }
  ReadResult result;
  result.dropped = std::max<qint64>(0, job->dropped - job->read);
  result.output = fresh();
  result.matched = matched;
  if (!peek) {
    job->read = job->dropped + job->output.size();
  }
  result.status = statusOf(*job);
  return result;
}

JobStatus BackgroundJobs::stop(const QString& id) {
  QMutexLocker lock(&m_mutex);
  const auto job = find(id);
  if (job->running) {
    killJob(*job);
    QDeadlineTimer deadline(5000);
    while (job->running && !deadline.hasExpired()) {
      m_wake.wait(&m_mutex, deadline);
    }
  }
  return statusOf(*job);
}

int BackgroundJobs::stopAll() {
  QMutexLocker lock(&m_mutex);
  int stopped = 0;
  for (const auto& job : m_jobs) {
    if (!job->running) {
      continue;
    }
    killJob(*job);
    stopped++;
  }
  return stopped;
}

int BackgroundJobs::clear() {
  const int stopped = stopAll();
  {
    QMutexLocker lock(&m_mutex);
    m_jobs.clear();
    m_order.clear();
    m_next = 1;
  }
  Q_EMIT changed();
  return stopped;
}

JobStatus BackgroundJobs::statusOf(const Job& job) {
  return JobStatus{job.id, job.command, job.running, job.exitCode, job.signal, job.started};
}

void BackgroundJobs::killJob(const Job& job) {
  killProcessGroup(job.pid);
  if (job.spec.kill) {
    job.spec.kill();
  }
}

} // namespace Smith
