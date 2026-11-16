#include "Backend.h"

#include "core/Dirs.h"
#include "core/Tool.h"

#include <QLocale>
#include "core/Jobs.h"
#include "core/Tool.h"
#include "tui/Rain.h"
#include "frontend/Commands.h"
#include "frontend/Duration.h"

#include <QDateTime>
#include <QFileInfo>

namespace Smith::Desktop {

// ---- properties ----

QVariantList Backend::todos() const {
  QVariantList out;
  if (!m_pAgent) {
    return out;
  }
  for (const Todo& todo : m_pAgent->todos()) {
    const QString status = todo.status == TodoStatus::Completed ? "completed" : todo.status == TodoStatus::InProgress ? "in_progress" : "pending";
    out.append(QVariantMap{{"content", todo.content}, {"status", status}});
  }
  return out;
}

QStringList Backend::queued() const {
  return m_pAgent ? m_pAgent->queuedMessages() : QStringList();
}

QVariantMap Backend::approval() const {
  if (!m_prompts.approval()) {
    return {};
  }
  const ApprovalRequest& request = *m_prompts.approval();
  QVariantMap map{
      {"tool", request.tool},
      {"summary", request.summary},
      {"note", request.note},
      {"warning", request.warning},
      {"hasSandbox", request.hasSandbox},
      {"sandboxActive", request.sandboxActive},
      {"sandboxDescription", request.sandboxDescription},
      {"canAllowAlways", request.canAllowAlways},
      {"similar", request.similar},
      {"remembered", request.remembered},
  };
  if (request.preview) {
    QVariantList rows;
    for (const DiffLine& line : *request.preview) {
      const QString kind = line.kind == DiffLine::Kind::Add      ? "add"
                           : line.kind == DiffLine::Kind::Remove ? "remove"
                           : line.kind == DiffLine::Kind::Gap    ? "gap"
                                                                 : "context";
      const int number = line.kind == DiffLine::Kind::Remove ? line.oldLine : line.newLine;
      rows.append(QVariantMap{{"kind", kind}, {"text", line.text}, {"number", number}, {"count", line.count}});
    }
    map["diff"] = rows;
  }
  return map;
}

QVariantMap Backend::question() const {
  const Question* asked = m_prompts.question();
  if (!asked) {
    return {};
  }
  return QVariantMap{{"question", asked->question}, {"options", asked->options}};
}

QString Backend::endpoint() const {
  return m_pServer ? m_pServer->name() : QString();
}

QString Backend::serverLabel() const {
  return m_pServer ? m_pServer->label() : QString();
}

QStringList Backend::endpoints() const {
  QStringList names;
  if (m_pRuntime) {
    for (const Endpoint& endpoint : m_pRuntime->config().endpoints) {
      names << endpoint.name;
    }
  }
  return names;
}

QString Backend::model() const {
  return m_pAgent ? m_pAgent->model() : QString();
}

QString Backend::reasoning() const {
  return m_pAgent ? m_pAgent->reasoning() : QString();
}

bool Backend::sandboxActive() const {
  return m_pRuntime && m_pRuntime->sandbox()->active();
}

QString Backend::sandboxKind() const {
  return m_pRuntime ? sandboxTypeName(m_pRuntime->sandbox()->type()) : QString();
}

QString Backend::sandboxDescription() const {
  return m_pRuntime ? m_pRuntime->sandbox()->describe(m_project.cwd) : QString();
}

bool Backend::network() const {
  return m_pRuntime && m_pRuntime->sandbox()->network();
}

int Backend::jobsRunning() const {
  return m_pRuntime ? m_pRuntime->jobs()->running() : 0;
}

QVariantList Backend::jobs() const {
  QVariantList out;
  if (!m_pRuntime) {
    return out;
  }
  for (const JobStatus& job : m_pRuntime->jobs()->list()) {
    out.append(QVariantMap{
        {"id", job.id},
        {"command", job.command},
        {"running", job.running},
        {"status", describeJob(job)},
        {"started", formatDuration(job.started.msecsTo(QDateTime::currentDateTime()))},
    });
  }
  return out;
}

QString Backend::projectName() const {
  const QString name = QFileInfo(m_project.cwd).fileName();
  return name.isEmpty() ? m_project.cwd : name;
}

QVariantList Backend::skills() const {
  QVariantList out;
  for (const Skill& skill : m_project.skills) {
    out.append(QVariantMap{{"name", skill.name}, {"description", QString(skill.description).simplified()}});
  }
  return out;
}

QString Backend::sessionId() const {
  return m_sessionControl.currentId ? m_sessionControl.currentId() : QString();
}

QString Backend::sessionName() const {
  return m_sessionControl.currentName ? m_sessionControl.currentName() : QString();
}

void Backend::setAllSessions(bool all) {
  if (all != m_allSessions) {
    m_allSessions = all;
    refreshSessions();
  }
}

QVariantList Backend::recentProjects() const {
  QVariantList out;
  if (!m_sessionControl.recentProjects) {
    return out;
  }
  for (const RecentProject& project : m_sessionControl.recentProjects()) {
    if (project.cwd == m_project.cwd || !pathExists(project.cwd)) {
      continue;
    }
    out.append(QVariantMap{
        {"path", project.cwd},
        {"name", QFileInfo(project.cwd).fileName().isEmpty() ? project.cwd : QFileInfo(project.cwd).fileName()},
        {"hint", QString("%1 · %2").arg(plural(project.sessions, "session"), ago(project.updated))},
        {"where", tildify(QFileInfo(project.cwd).path())},
    });
  }
  return out;
}

bool Backend::planning() const {
  return m_pAgent && m_pAgent->planning();
}

QString Backend::usageDetail() const {
  const TurnUsage& usage = m_turns.usage();
  if (usage.calls == 0) {
    return {};
  }
  const QLocale english(QLocale::English);
  QString text = QString("%1: %2, %3 prompt tokens sent (the whole conversation, again with each call) and %4 generated")
                     .arg(m_turns.busy() ? "This turn so far" : "The last turn", plural(usage.calls, "model call"),
                          english.toString(usage.promptTokens), english.toString(usage.completionTokens));
  if (const std::optional<double> speed = usage.tokensPerSecond()) {
    text += QString(", at %1 tokens a second").arg(english.toString(*speed, 'f', 1));
  }
  return text + ".";
}

bool Backend::canUndo() const {
  return m_pAgent && !m_turns.busy() && m_pAgent->canUndo();
}

QVariantList Backend::suggest(const QString& input, int) {
  QStringList models;
  for (const QVariant& model : m_models) {
    models << model.toMap().value("id").toString();
  }
  QVariantList out;
  QStringList tasks;
  for (const Worktree& worktree : m_worktreeList) {
    if (!worktree.task.isEmpty()) {
      tasks << worktree.task;
    }
  }
  const CompletionContext context{m_project.cwd, endpoints(), models, tasks, [this]() { return m_fileIndex.files(m_project.cwd); }};
  for (const Suggestion& suggestion : Smith::suggest(input, context)) {
    out.append(QVariantMap{{"text", suggestion.text},
                           {"label", suggestion.label},
                           {"hint", suggestion.hint},
                           {"takesArgument", suggestion.takesArgument},
                           {"insert", suggestion.insert}});
  }
  return out;
}

QString Backend::codeStreamText(int offset, int width) const {
  return Tui::codeStream(offset, width);
}

} // namespace Smith::Desktop
