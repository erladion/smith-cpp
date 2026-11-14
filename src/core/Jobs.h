#pragma once

#include "core/Cancel.h"
#include "core/Process.h"

#include <QDateTime>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QRegularExpression>
#include <QString>
#include <QThread>
#include <QWaitCondition>

#include <memory>
#include <optional>

namespace Smith {

struct JobStatus {
  QString id;
  QString command;
  bool running = true;
  // Exit code once it has ended; nullopt if a signal ended it.
  std::optional<int> exitCode;
  // The signal that ended it, e.g. SIGKILL after a stop.
  QString signal;
  QDateTime started;
};

// "running", "exited with code 0", "stopped (SIGKILL)".
QString describeJob(const JobStatus& job);

// Shell commands the agent started in the background (dev servers, watchers,
// long builds). They run in the same sandbox as other commands, outlive the
// tool call and the turn, and are stopped when the conversation changes or
// Smith exits. Their processes live on a thread of their own, so any thread
// can start, read and stop them.
class BackgroundJobs : public QObject {
  Q_OBJECT

public:
  BackgroundJobs();
  ~BackgroundJobs() override;

  // Starts a command from its spawn spec; its output collects until read.
  JobStatus start(const QString& command, const SpawnSpec& spec, const QString& cwd);
  QList<JobStatus> list() const;
  // How many are still running.
  int running() const;

  struct ReadResult {
    JobStatus status;
    QString output;
    qint64 dropped = 0;
    bool matched = false;
  };

  // Output since the last read, and the job's state. With `waitMs`, first
  // waits up to that long for the job to end, or, with `until`, for new
  // output matching it. With `peek`, the output stays unread. Throws for an unknown id.
  ReadResult read(const QString& id, int waitMs = 0, const std::optional<QRegularExpression>& until = std::nullopt, const CancelPtr& cancel = {},
                  bool peek = false);

  // Stops a job and everything it started; returns once it has ended (or after 5 s).
  JobStatus stop(const QString& id);
  // Stops every job at once, without waiting; returns how many were running.
  int stopAll();
  // Stops every job and forgets them all, e.g. for a new conversation.
  int clear();

Q_SIGNALS:
  // A job started or ended.
  void changed();

private:
  struct Job;

  void startOnThread(const std::shared_ptr<Job>& job, const QString& cwd);
  std::shared_ptr<Job> find(const QString& id) const;
  static JobStatus statusOf(const Job& job);
  static void killJob(const Job& job);

  QThread m_thread;
  QObject* m_pHome = nullptr;
  mutable QMutex m_mutex;
  QWaitCondition m_wake;
  QMap<QString, std::shared_ptr<Job>> m_jobs;
  QStringList m_order;
  int m_next = 1;
};

} // namespace Smith
