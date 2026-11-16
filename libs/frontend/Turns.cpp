#include "frontend/Turns.h"

#include "core/Errors.h"
#include "core/tools/TodoTool.h"
#include "frontend/Activity.h"

#include <QDateTime>
#include <QPointer>

#include <cmath>
#include <memory>
#include <utility>

namespace Smith {

TurnRunner::TurnRunner(QObject* pParent) : QObject(pParent) {}

void TurnRunner::forgetContext() {
  m_tokens.reset();
  m_limit.reset();
}

void TurnRunner::queueNext(const QString& task) {
  m_next << task;
  Q_EMIT changed();
}

QString TurnRunner::takeNext(int index) {
  if (m_next.isEmpty()) {
    return {};
  }
  const QString task = m_next.takeAt(index < 0 || index >= m_next.size() ? m_next.size() - 1 : index);
  Q_EMIT changed();
  return task;
}

std::optional<QString> TurnRunner::afterTurn(QStringList& putBack) {
  if (m_next.isEmpty()) {
    return std::nullopt;
  }
  if (m_last.reason == "stop") {
    const QString task = m_next.takeFirst();
    Q_EMIT changed();
    return task;
  }
  putBack = m_next;
  m_next.clear();
  Q_EMIT changed();
  return std::nullopt;
}

std::optional<qint64> TurnRunner::elapsed() const {
  return m_timed ? std::optional<qint64>(m_clock.elapsed()) : std::nullopt;
}

QString TurnRunner::runningLabel() const {
  QStringList labels;
  for (const QString& id : m_runningOrder) {
    labels << m_running.value(id);
  }
  return Smith::runningLabel(labels);
}

std::optional<Entry> TurnRunner::apply(const AgentEvent& event) {
  using Added = std::optional<Entry>;
  return std::visit(Overloaded{
                        [](const Event::UserMessage& message) -> Added { return Entry::simple(Entry::Kind::User, message.text); },
                        [this](const Event::Delta&) -> Added {
                          m_compacting = false;
                          m_activity.clear();
                          return std::nullopt;
                        },
                        [](const Event::ResponseEnd&) -> Added { return std::nullopt; },
                        [this](const Event::ToolPrepare& tool) -> Added {
                          m_compacting = false;
                          m_activity = "Preparing " + tool.name + "…";
                          return std::nullopt;
                        },
                        [this](const Event::ToolStart& tool) -> Added {
                          m_compacting = false;
                          m_running.insert(tool.id, tool.name + " " + tool.summary);
                          m_runningOrder.removeAll(tool.id);
                          m_runningOrder << tool.id;
                          m_activity = runningLabel();
                          return std::nullopt;
                        },
                        [this](const Event::ToolEnd& tool) -> Added {
                          m_compacting = false;
                          m_toolCalls++;
                          m_running.remove(tool.id);
                          m_runningOrder.removeAll(tool.id);
                          m_activity = runningLabel();
                          // The checklist itself is shown when it changes.
                          if (tool.name == TODO_TOOL && !tool.isError) {
                            return std::nullopt;
                          }
                          Entry entry;
                          entry.kind = Entry::Kind::Tool;
                          entry.name = tool.name;
                          entry.summary = tool.summary;
                          entry.output = tool.output;
                          entry.isError = tool.isError;
                          entry.diff = tool.diff;
                          return entry;
                        },
                        [this](const Event::Context& context) -> Added {
                          m_tokens = context.tokens;
                          m_limit = context.limit;
                          return std::nullopt;
                        },
                        [this](const Event::Activity& activity) -> Added {
                          m_compacting = activity.compacting;
                          // Progress from inside a tool (a sub-agent); say so when several run at once.
                          m_activity = m_running.size() > 1 ? QString("%1 in parallel · %2").arg(m_running.size()).arg(activity.text) : activity.text;
                          return std::nullopt;
                        },
                        [](const Event::Notice& notice) -> Added { return Entry::notice(notice.level, notice.text); },
                        [this](const Event::Todos& changed) -> Added {
                          // Outside a turn the list was cleared or restored with the session, which shows it its own way.
                          if (!m_busy) {
                            return std::nullopt;
                          }
                          Entry entry;
                          entry.kind = Entry::Kind::Todos;
                          entry.todos = changed.todos;
                          return entry;
                        },
                        [this](const Event::Done& done) -> Added {
                          m_reason = doneReasonName(done.reason);
                          return std::nullopt;
                        },
                        [this](const Event::Usage& usage) -> Added {
                          if (m_timed) {
                            m_usage.calls++;
                            m_usage.promptTokens += usage.promptTokens;
                            m_usage.completionTokens += usage.completionTokens;
                            if (usage.generationMs > 0) {
                              m_usage.timedTokens += usage.completionTokens;
                              m_usage.generationMs += usage.generationMs;
                            }
                          }
                          return std::nullopt;
                        },
                    },
                    event);
}

void TurnRunner::runTurn(const QString& activity, const QString& baseUrl, std::function<void(const CancelPtr&)> work,
                         std::function<void(const QString& error)> finished, bool timed) {
  const CancelPtr cancel = makeCancel();
  m_pCancel = cancel;
  m_busy = true;
  m_activity = activity;
  m_timed = timed;
  if (timed) {
    m_clock.start();
    m_startedAt = QDateTime::currentMSecsSinceEpoch();
    m_toolCalls = 0;
    m_reason.clear();
    m_usage = {};
  }
  Q_EMIT changed();
  auto error = std::make_shared<QString>();
  launch(
      [work, cancel, error, baseUrl]() {
        try {
          work(cancel);
        } catch (const std::exception& failure) {
          *error = describeError(failure, baseUrl);
        } catch (...) {
          *error = "Unexpected error.";
        }
      },
      [this, error, finished]() {
        if (m_timed) {
          m_last = TurnStats{m_reason, m_clock.elapsed(), m_toolCalls, m_usage};
          m_timed = false;
          m_startedAt = 0;
        }
        m_pCancel.reset();
        m_activity.clear();
        m_running.clear();
        m_runningOrder.clear();
        m_compacting = false;
        m_busy = false;
        if (finished) {
          finished(*error);
        }
        Q_EMIT changed();
      });
}

void TurnRunner::runTask(const QString& activity, const QString& baseUrl, std::function<std::function<void()>()> work,
                         std::function<void(const QString& error)> failed) {
  m_busy = true;
  m_activity = activity;
  Q_EMIT changed();
  auto next = std::make_shared<std::function<void()>>();
  auto error = std::make_shared<QString>();
  launch(
      [work, next, error, baseUrl]() {
        try {
          *next = work();
        } catch (const std::exception& failure) {
          *error = describeError(failure, baseUrl);
        }
      },
      [this, next, error, failed]() {
        m_activity.clear();
        m_busy = false;
        if (!error->isEmpty()) {
          if (failed) {
            failed(*error);
          }
        } else if (*next) {
          (*next)();
        }
        Q_EMIT changed();
      });
}

void TurnRunner::launch(std::function<void()> work, std::function<void()> finished) {
  QThread* thread = QThread::create(std::move(work));
  m_workers << thread;
  connect(thread, &QThread::finished, this, [this, thread = QPointer<QThread>(thread), finished]() {
    // Already deleted by close.
    if (!thread) {
      return;
    }
    m_workers.removeAll(thread);
    thread->deleteLater();
    if (!m_closed && finished) {
      finished();
    }
  });
  thread->start();
}

void TurnRunner::interrupt() {
  if (m_pCancel) {
    m_pCancel->cancel();
  }
}

void TurnRunner::close() {
  if (m_closed) {
    return;
  }
  m_closed = true;
  interrupt();
  m_pClosing->cancel();
  // The threads stop at their next check of their cancel token.
  // Deleted here: what would delete them later is not called after close.
  for (QThread* thread : std::as_const(m_workers)) {
    thread->wait();
    delete thread;
  }
  m_workers.clear();
}

std::optional<double> TurnUsage::tokensPerSecond() const {
  // Too short to time well.
  if (generationMs < 250 || timedTokens < 8) {
    return std::nullopt;
  }
  return timedTokens * 1000.0 / generationMs;
}

QString usageText(const TurnUsage& usage) {
  if (usage.calls == 0) {
    return {};
  }
  auto count = [](qint64 n) {
    if (n < 1000) {
      return QString::number(n);
    }
    if (n < 9950) {
      return QString::number(n / 1000.0, 'f', 1) + "k";
    }
    return QString::number(std::llround(n / 1000.0)) + "k";
  };
  QString text = count(usage.promptTokens) + " in · " + count(usage.completionTokens) + " out";
  if (const std::optional<double> speed = usage.tokensPerSecond()) {
    text += QString(" · %1 tok/s").arg(*speed < 10 ? QString::number(*speed, 'f', 1) : QString::number(std::lround(*speed)));
  }
  return text;
}

QString putBackText(int count) {
  return count == 1 ? QString("Your queued task wasn't started, so it's back in the input box.")
                    : QString("Your %1 queued tasks weren't started, so they're back in the input box, one after another.").arg(count);
}

QList<Entry> changesEntries(const SessionChanges& changes, const QString& cwd) {
  auto relative = [&](const QString& file) { return file.startsWith(cwd + "/") ? file.mid(cwd.size() + 1) : file; };
  const QString untracked = changes.untracked.isEmpty()
                                ? QString()
                                : QString("\nNot shown: what %1 commands changed (git diff shows that).").arg(changes.untracked.join(", "));
  if (changes.files.isEmpty()) {
    return {Entry::notice(NoticeLevel::Info, "The agent hasn't changed any files in this session, or they're back as they were." + untracked)};
  }
  QList<Entry> files;
  DiffStats total;
  for (const ChangedFile& change : changes.files) {
    Entry entry;
    entry.kind = Entry::Kind::Tool;
    entry.name = !change.before ? "created" : !change.now ? "deleted" : "changed";
    entry.summary = relative(change.file);
    const QByteArray before = change.before.value_or(QByteArray());
    const QByteArray now = change.now.value_or(QByteArray());
    if (before.contains('\0') || now.contains('\0')) {
      entry.output = QString("A binary file: %1 → %2 bytes.").arg(before.size()).arg(now.size());
    } else {
      entry.diff = diffLines(QString::fromUtf8(before), QString::fromUtf8(now), 3);
      entry.fullDiff = true;
      const DiffStats stats = diffStats(*entry.diff);
      total.added += stats.added;
      total.removed += stats.removed;
    }
    files.append(entry);
  }
  const QString head = QString("Changed in this session: %1, +%2 −%3, compared with how each was before the agent first changed it.")
                           .arg(plural(changes.files.size(), "file"))
                           .arg(total.added)
                           .arg(total.removed);
  return QList<Entry>{Entry::notice(NoticeLevel::Info, head + untracked)} + files;
}

Entry undoNotice(const UndoResult& result, const QString& cwd) {
  auto relative = [&](const QStringList& files) {
    QStringList out;
    for (const QString& file : files) {
      out << (file.startsWith(cwd + "/") ? file.mid(cwd.size() + 1) : file);
    }
    return out.join(", ");
  };
  QStringList lines{"Undid the last turn and removed it from the conversation."};
  if (!result.restored.isEmpty()) {
    lines << "Restored: " + relative(result.restored);
  }
  if (!result.skipped.isEmpty()) {
    lines << "Left alone because they changed afterwards: " + relative(result.skipped);
  }
  if (!result.untracked.isEmpty()) {
    lines << QString("Not undone: anything changed by %1 commands.").arg(result.untracked.join(", "));
  }
  const bool warn = !result.skipped.isEmpty() || !result.untracked.isEmpty();
  return Entry::notice(warn ? NoticeLevel::Warn : NoticeLevel::Info, lines.join('\n'));
}

} // namespace Smith
