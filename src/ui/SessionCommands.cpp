#include "ui/SessionCommands.h"

#include "core/Dirs.h"
#include "core/Errors.h"
#include "core/Tool.h"
#include "ui/App.h"
#include "ui/Connection.h"
#include "ui/Folders.h"
#include "ui/Replay.h"

#include <QFileInfo>
#include <QLocale>
#include <QMap>
#include <QRegularExpression>

#include <cmath>

namespace Smith::Ui {

QString ago(const QDateTime& date) {
  const qint64 minutes = std::llround(date.msecsTo(QDateTime::currentDateTime()) / 60000.0);
  if (minutes < 1) {
    return "just now";
  }
  if (minutes < 60) {
    return QString("%1 min ago").arg(minutes);
  }
  const qint64 hours = std::llround(minutes / 60.0);
  if (hours < 24) {
    return QString("%1 h ago").arg(hours);
  }
  if (hours < 48) {
    return "yesterday";
  }
  return QLocale(QLocale::English).toString(date.date(), "d MMM yyyy");
}

SessionCommands::SessionCommands(App& app, ConnectionCommands& connection) : m_app(app), m_connection(connection) {}

void SessionCommands::newSession(const QString& folder) {
  const QString previous = m_app.opts().sessions.currentId();
  QList<Entry> notes;
  if (!folder.isEmpty()) {
    const std::optional<QList<Entry>> entered = enterProject(folder);
    if (!entered) {
      return;
    }
    notes = *entered;
  } else {
    m_app.agent()->loadConversation({});
  }
  m_app.opts().sessions.startNew(m_app.agent()->model(), m_app.server()->name());
  const QString where = folder.isEmpty() ? QString() : " in " + tildify(m_app.project().cwd);
  notes.prepend(Entry::notice(NoticeLevel::Info,
                              QString("Started a new session%1. The previous one is %2; /sessions lists this folder's sessions.").arg(where, previous)));
  m_app.startOver(Start{}, notes);
}

std::optional<QList<Entry>> SessionCommands::enterProject(const QString& folder) {
  ProjectView next;
  try {
    next = m_app.opts().switchProject(folder);
  } catch (const std::exception& error) {
    m_app.print(Entry::notice(NoticeLevel::Error, errorMessage(error)));
    return std::nullopt;
  }
  const std::shared_ptr<Agent> agent = m_app.agent();
  const AgentSettings current{m_app.server(), agent->model(), agent->reasoning(), agent->contextLimit()};
  m_app.setProject(next);
  m_app.setAgent(m_app.opts().createAgent(m_app.hooks(), current));
  QList<Entry> notes;
  for (const QString& warning : next.warnings) {
    notes.append(Entry::notice(NoticeLevel::Warn, warning));
  }
  if (next.cwd == homeDir() || next.cwd == "/") {
    notes.append(Entry::notice(NoticeLevel::Warn, QString("The project is now %1: the agent can change anything under it.").arg(next.cwd)));
  }
  return notes;
}

void SessionCommands::nameSession(const QString& name) {
  if (name.isEmpty()) {
    const QString current = m_app.opts().sessions.currentName();
    m_app.print(Entry::notice(NoticeLevel::Info, current.isEmpty()
                                                     ? QString("This session has no name yet. /name <name> gives it one; until then, listings use its first message.")
                                                     : QString("This session is named \"%1\". /name <new name> renames it.").arg(current)));
    return;
  }
  m_app.opts().sessions.rename(name);
  static const QRegularExpression whitespace(R"(\s)");
  const QString resume = whitespace.match(name).hasMatch() ? QString("smith --resume \"%1\"").arg(name) : "smith --resume " + name;
  m_app.print(Entry::notice(NoticeLevel::Info, QString("Named this session \"%1\". Resume it from anywhere with: %2").arg(name, resume)));
}

void SessionCommands::openProjectPicker() {
  const QString cwd = m_app.project().cwd;
  QList<Tui::PickerItem> items{{"browse:" + cwd, "Browse folders…", "starting from the current project"}};
  for (const RecentProject& project : m_app.opts().sessions.recentProjects()) {
    if (project.cwd == cwd || !pathExists(project.cwd)) {
      continue;
    }
    // The folder's name stays readable even when its full path is long.
    const QString label = QFileInfo(project.cwd).fileName().isEmpty() ? project.cwd : QFileInfo(project.cwd).fileName();
    items.append({"use:" + project.cwd, label,
                  QString("%1 · %2 · %3").arg(plural(project.sessions, "session"), ago(project.updated), tildify(QFileInfo(project.cwd).path()))});
  }
  m_app.openPicker(std::make_unique<Tui::Picker>("Open a project", items), [this](const QString& value) { pickFolder(value); });
}

void SessionCommands::browseFolder(const QString& dir) {
  QMap<QString, int> sessionCounts;
  for (const RecentProject& project : m_app.opts().sessions.recentProjects()) {
    sessionCounts.insert(project.cwd, project.sessions);
  }
  auto describe = [&](const QString& folder) {
    QStringList parts;
    if (isGitRepo(folder)) {
      parts << "git";
    }
    if (sessionCounts.value(folder)) {
      parts << plural(sessionCounts.value(folder), "session");
    }
    return parts.join(" · ");
  };
  const QString parent = QFileInfo(dir).path();
  const FolderListing listing = listFolders(dir);
  QString useHint = describe(dir);
  if (useHint.isEmpty() && dir == m_app.project().cwd) {
    useHint = "the current project";
  }
  QList<Tui::PickerItem> items{{"use:" + dir, "✓ Use this folder", useHint}};
  if (parent != dir) {
    items.append({"browse:" + parent, "..", "up one level"});
  }
  for (const QString& name : listing.names) {
    const QString path = dir == "/" ? "/" + name : dir + "/" + name;
    items.append({"browse:" + path, name + "/", describe(path)});
  }
  if (!listing.error.isEmpty()) {
    items.append({"browse:" + dir, "(" + listing.error + ")", {}});
  }
  Tui::PickerOptions options;
  options.back = parent != dir ? "browse:" + parent : QString();
  options.help = "type to filter · enter opens · backspace goes up · esc cancels";
  m_app.openPicker(std::make_unique<Tui::Picker>(tildify(dir), items, QString(), options), [this](const QString& value) { pickFolder(value); });
}

void SessionCommands::pickFolder(const QString& value) {
  if (value.startsWith("browse:")) {
    browseFolder(value.mid(7));
  } else if (value.startsWith("use:")) {
    newSession(value.mid(4));
  }
}

void SessionCommands::openSessionPicker(bool everywhere) {
  const QString current = m_app.opts().sessions.currentId();
  const QList<SessionInfo> sessions = m_app.opts().sessions.list(everywhere);
  if (sessions.isEmpty() || (sessions.size() == 1 && sessions.first().id == current)) {
    const QString where = everywhere ? QString() : QString(" for this folder; /sessions all shows every folder's");
    m_app.print(Entry::notice(NoticeLevel::Info, QString("There are no other saved sessions%1.").arg(where)));
    return;
  }
  QMap<QString, SessionInfo> byId;
  QList<Tui::PickerItem> items;
  for (const SessionInfo& session : sessions) {
    byId.insert(session.id, session);
    QStringList hint;
    if (everywhere) {
      hint << shortPath(session.cwd, 32);
    }
    hint << ago(session.updated) << plural(session.turns, "message") << (session.endpoint.isEmpty() ? QString() : session.endpoint + "/") + session.model;
    items.append({session.id, session.name, hint.join(" · ")});
  }
  const QString title = everywhere ? "Switch session (all folders)" : "Switch session (this folder; /sessions all for every folder)";
  Tui::PickerOptions options;
  options.action = Tui::PickerAction{'d', "delete", [this, byId](const Tui::PickerItem& item) {
                                       if (item.value == m_app.opts().sessions.currentId()) {
                                         return Tui::PickerAction::Check{{}, "That's the session you're in; switch to another one first."};
                                       }
                                       const int turns = byId.value(item.value).turns;
                                       return Tui::PickerAction::Check{
                                           QString("Delete \"%1\" (%2) and its tool log?").arg(item.label, plural(turns, "message")), {}};
                                     }};
  m_app.openPicker(
      std::make_unique<Tui::Picker>(title, items, current, options),
      [this, byId](const QString& id) {
        if (id != m_app.opts().sessions.currentId()) {
          switchSession(id, byId.value(id).cwd);
        }
      },
      [this, byId, everywhere](const QString& id) { deleteSession(id, everywhere, byId.contains(id) ? byId.value(id).name : id); });
}

void SessionCommands::deleteSession(const QString& id, bool everywhere, const QString& name) {
  try {
    m_app.opts().sessions.remove(id);
  } catch (const std::exception& error) {
    m_app.print(Entry::notice(NoticeLevel::Error, "Could not delete the session: " + errorMessage(error)));
    return;
  }
  m_app.print(Entry::notice(NoticeLevel::Info, QString("Deleted the session \"%1\" (%2) and its tool log.").arg(name, id)));
  openSessionPicker(everywhere);
}

void SessionCommands::switchSession(const QString& id, const QString& folder) {
  QList<Entry> notes;
  if (!folder.isEmpty() && folder != m_app.project().cwd) {
    if (!pathExists(folder)) {
      m_app.print(Entry::notice(NoticeLevel::Error, QString("That session's folder, %1, doesn't exist anymore.").arg(folder)));
      return;
    }
    const std::optional<QList<Entry>> entered = enterProject(folder);
    if (!entered) {
      return;
    }
    notes.append(Entry::notice(NoticeLevel::Info, QString("Now working in %1.").arg(tildify(folder))));
    notes.append(*entered);
  }
  OpenedSession opened;
  try {
    opened = m_app.opts().sessions.open(id);
  } catch (const std::exception& error) {
    m_app.print(Entry::notice(NoticeLevel::Error, QString("Could not open session %1: %2").arg(id, errorMessage(error))));
    return;
  }
  m_app.agent()->loadConversation(opened.messages);
  m_app.startOver(Start{true, opened.lastUsed}, notes + historyToEntries(opened.messages, m_app.project().tools));

  // Continue on the endpoint and model the session last used, when they're available.
  bool configured = false;
  for (const Endpoint& endpoint : m_app.opts().endpoints) {
    configured = configured || endpoint.name == opened.endpoint;
  }
  if (!opened.endpoint.isEmpty() && opened.endpoint != m_app.server()->name() && configured) {
    m_connection.switchEndpoint(opened.endpoint, opened.model);
  } else if (opened.model != m_app.agent()->model()) {
    const ServerPtr server = m_app.server();
    const QString model = opened.model;
    m_app.withActivity(QString("Checking %1…").arg(model), [this, server, model]() -> std::function<void()> {
      bool served = false;
      try {
        for (const ModelInfo& info : server->listModels()) {
          served = served || info.id == model;
        }
      } catch (const std::exception&) {
        served = false;
      }
      return [this, served, model, server]() {
        if (served) {
          m_connection.switchModel(model);
        } else {
          m_app.print(Entry::notice(NoticeLevel::Warn, QString("This session used %1, which %2 doesn't serve; continuing with %3.")
                                                           .arg(model, server->label(), m_app.agent()->model())));
        }
      };
    });
  }
}

} // namespace Smith::Ui
