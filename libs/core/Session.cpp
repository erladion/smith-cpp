#include "core/Session.h"

#include "core/Agent.h"
#include "core/Context.h"
#include "core/Dirs.h"
#include "core/Errors.h"
#include "core/Json.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QRegularExpression>

#include <algorithm>

namespace Smith {

namespace {

struct Log {
  QJsonObject meta;
  Messages messages;
  QString model;
  QString endpoint;
  QString name;
  bool named = false;
  // The messages you sent over the whole session, compacted away or not, and the first of them.
  int sent = 0;
  QString first;
};

// One you sent, not a reminder the agent added.
bool isSent(const Message& message) {
  return messageRole(message) == "user" && !messageText(message).startsWith(TODO_REMINDER);
}

int sentIn(const Messages& messages) {
  return int(std::count_if(messages.begin(), messages.end(), isSent));
}

// Compaction folds your messages into a summary, which leaves them sent; any
// other replace (undo, mostly) takes away the ones it drops.
void replaced(Log& log, const Messages& next) {
  const bool compacted = !next.isEmpty() && messageText(next.first()).startsWith(SUMMARY_PREFIX) &&
                         (log.messages.isEmpty() || messageText(log.messages.first()) != messageText(next.first()));
  if (!compacted) {
    log.sent = std::max(0, log.sent - (sentIn(log.messages) - sentIn(next)));
    if (log.sent == 0) {
      log.first.clear();
    }
  }
  log.messages = next;
}

Log readLog(const QString& file) {
  QFile handle(file);
  if (!handle.open(QIODevice::ReadOnly)) {
    throw SmithError(QString("Could not read %1").arg(file));
  }
  const QList<QByteArray> raw = handle.readAll().split('\n');
  Log log;
  bool hasMeta = false;
  bool hasModel = false;
  for (int i = 0; i < raw.size(); i++) {
    if (raw[i].trimmed().isEmpty()) {
      continue;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(raw[i], &error);
    if (!document.isObject()) {
      // Only the last line can be half-written (crash mid-append); anything else is corruption.
      if (i >= raw.size() - 2) {
        break;
      }
      throw SmithError(QString("Corrupt session log %1 at line %2").arg(file).arg(i + 1));
    }
    const QJsonObject line = document.object();
    const QString type = line.value("type").toString();
    if (type == "meta") {
      log.meta = line;
      hasMeta = true;
    } else if (type == "append") {
      const Message message = line.value("message").toObject();
      if (isSent(message)) {
        if (log.sent++ == 0) {
          log.first = messageText(message);
        }
      }
      log.messages.append(message);
    } else if (type == "replace") {
      Messages next;
      for (const QJsonValue& message : line.value("messages").toArray()) {
        next.append(message.toObject());
      }
      replaced(log, next);
    } else if (type == "name") {
      log.name = line.value("name").toString();
      log.named = true;
    } else if (type == "model") {
      log.model = line.value("model").toString();
      log.endpoint = line.value("endpoint").toString();
      hasModel = true;
    }
  }
  if (!hasMeta) {
    throw SmithError(QString("Session log %1 has no header").arg(file));
  }
  if (!log.named && log.meta.contains("name")) {
    log.name = log.meta.value("name").toString();
    log.named = true;
  }
  if (!hasModel) {
    log.model = log.meta.value("model").toString();
    log.endpoint = log.meta.value("endpoint").toString();
  }
  return log;
}

// One line from a session's first message; after compaction, the message following the summary.
QString preview(const QString& first) {
  QString text = first;
  if (text.startsWith(SUMMARY_PREFIX)) {
    const int separator = text.indexOf("\n\n---\n\n");
    text = separator == -1 ? QString("(summarized conversation)") : text.mid(separator + 7);
  }
  static const QRegularExpression whitespace(R"(\s+)");
  return text.replace(whitespace, " ").trimmed().left(100);
}

} // namespace

QString sessionsDir() {
  return dataDir() + "/sessions";
}

Session::Session(const QString& id, const QString& file, const QJsonObject& meta)
    : m_id(id), m_file(file), m_meta(meta), m_model(meta.value("model").toString()), m_endpoint(meta.value("endpoint").toString()),
      m_name(meta.value("name").toString()) {}

std::shared_ptr<Session> Session::create(const QString& cwd, const QString& model, const QString& endpoint) {
  const QDateTime now = QDateTime::currentDateTimeUtc();
  const QString stamp = now.toString("yyyyMMdd-HHmmss");
  const QString id = QString("%1-%2").arg(stamp).arg(QRandomGenerator::global()->bounded(0x10000), 4, 16, QChar('0'));
  QJsonObject meta{{"type", "meta"}, {"id", id}, {"cwd", cwd}, {"model", model}, {"created", now.toString(Qt::ISODateWithMs)}};
  if (!endpoint.isEmpty()) {
    meta["endpoint"] = endpoint;
  }
  return std::shared_ptr<Session>(new Session(id, sessionsDir() + "/" + id + ".jsonl", meta));
}

Session::Opened Session::open(const QString& id) {
  const QString file = sessionsDir() + "/" + id + ".jsonl";
  if (!QFileInfo::exists(file)) {
    throw SmithError(QString("No session %1 in %2").arg(id, sessionsDir()));
  }
  const Log log = readLog(file);
  auto session = std::shared_ptr<Session>(new Session(log.meta.value("id").toString(), file, log.meta));
  session->m_lastUsed = QFileInfo(file).lastModified();
  session->m_model = log.model;
  session->m_endpoint = log.endpoint;
  session->m_name = log.name;
  session->m_started = true;
  return Opened{session, log.messages};
}

QString Session::cwd() const {
  QMutexLocker lock(&m_mutex);
  return m_meta.value("cwd").toString();
}

QString Session::model() const {
  QMutexLocker lock(&m_mutex);
  return m_model;
}

QString Session::endpoint() const {
  QMutexLocker lock(&m_mutex);
  return m_endpoint;
}

QString Session::name() const {
  QMutexLocker lock(&m_mutex);
  return m_name;
}

void Session::record(const HistoryChange& change) {
  if (change.type == HistoryChange::Type::Append) {
    write(QJsonObject{{"type", "append"}, {"message", change.message}});
    return;
  }
  QJsonArray messages;
  for (const Message& message : change.messages) {
    messages.append(message);
  }
  write(QJsonObject{{"type", "replace"}, {"messages", messages}});
}

void Session::rename(const QString& name) {
  {
    QMutexLocker lock(&m_mutex);
    m_name = name;
  }
  write(QJsonObject{{"type", "name"}, {"name", name}});
}

void Session::recordModel(const QString& model, const QString& endpoint) {
  QMutexLocker lock(&m_mutex);
  if (model == m_model && endpoint == m_endpoint) {
    return;
  }
  m_model = model;
  m_endpoint = endpoint;
  if (!m_started) {
    // Before the first message there's nothing to resume; the header will carry it.
    m_meta["model"] = model;
    if (endpoint.isEmpty()) {
      m_meta.remove("endpoint");
    } else {
      m_meta["endpoint"] = endpoint;
    }
    return;
  }
  lock.unlock();
  QJsonObject line{{"type", "model"}, {"model", model}};
  if (!endpoint.isEmpty()) {
    line["endpoint"] = endpoint;
  }
  write(line);
}

void Session::write(const QJsonObject& line) {
  QMutexLocker lock(&m_mutex);
  QByteArray data;
  if (!m_started) {
    privateDir(dataDir());
    privateDir(sessionsDir());
    data += QJsonDocument(m_meta).toJson(QJsonDocument::Compact) + "\n";
    m_started = true;
  }
  data += QJsonDocument(line).toJson(QJsonDocument::Compact) + "\n";
  QFile handle(m_file);
  // Sessions contain whole conversations: readable by you only.
  const bool created = !handle.exists();
  if (!handle.open(QIODevice::WriteOnly | QIODevice::Append)) {
    return;
  }
  if (created) {
    handle.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
  }
  handle.write(data);
  handle.close();
  if (!m_secured) {
    privateFile(m_file);
    m_secured = true;
  }
}

void secureSessions() {
  privateTree(dataDir(), QRegularExpression("^$"));
  privateTree(sessionsDir(), QRegularExpression(R"(\.jsonl$)"));
  privateTree(dataDir() + "/logs", QRegularExpression(R"(\.jsonl$)"));
}

QList<SessionInfo> listSessions(const QString& cwd, int limit) {
  QDir dir(sessionsDir());
  if (!dir.exists()) {
    return {};
  }
  QFileInfoList files = dir.entryInfoList({"*.jsonl"}, QDir::Files);
  std::sort(files.begin(), files.end(), [](const QFileInfo& a, const QFileInfo& b) { return a.lastModified() > b.lastModified(); });

  QList<SessionInfo> infos;
  for (const QFileInfo& file : files) {
    if (limit >= 0 && infos.size() >= limit) {
      break;
    }
    try {
      const Log log = readLog(file.filePath());
      const QString sessionCwd = log.meta.value("cwd").toString();
      if (!cwd.isEmpty() && sessionCwd != cwd) {
        continue;
      }
      SessionInfo info;
      info.id = log.meta.value("id").toString();
      info.cwd = sessionCwd;
      info.model = log.model;
      info.endpoint = log.endpoint;
      info.created = log.meta.value("created").toString();
      info.updated = file.lastModified();
      info.file = file.filePath();
      info.preview = preview(log.first);
      info.turns = log.sent;
      info.named = log.named;
      info.name = log.named ? log.name : defaultName(info.preview);
      infos.append(info);
    } catch (const std::exception&) {
      // Unreadable or foreign file; skip it.
    }
  }
  return infos;
}

QString defaultName(const QString& firstMessage) {
  static const QRegularExpression sentenceEnd(R"((?<=[.!?])\s)");
  const QString line = firstMessage.split(sentenceEnd).value(0);
  if (line.size() <= 48) {
    return line.isEmpty() ? QString("(no messages)") : line;
  }
  // Cut at 47 characters, dropping a word only if the cut splits it.
  QString cut = line.left(47);
  if (!line[47].isSpace()) {
    static const QRegularExpression partialWord(R"(\s+\S*$)");
    cut.remove(partialWord);
  }
  while (!cut.isEmpty() && cut.back().isSpace()) {
    cut.chop(1);
  }
  return cut + "…";
}

QString findSession(const QString& idOrName) {
  if (QFileInfo::exists(sessionsDir() + "/" + idOrName + ".jsonl")) {
    return idOrName;
  }
  const QString wanted = idOrName.toLower();
  QList<SessionInfo> matches;
  for (const SessionInfo& info : listSessions()) {
    if (info.name.toLower() == wanted) {
      matches.append(info);
    }
  }
  if (matches.size() == 1) {
    return matches.first().id;
  }
  if (matches.size() > 1) {
    QStringList lines;
    for (const SessionInfo& info : matches) {
      lines << QString("  %1  %2").arg(info.id, info.cwd);
    }
    throw SmithError(QString("Several sessions are named \"%1\"; resume one by id:\n%2").arg(idOrName, lines.join('\n')));
  }
  throw SmithError(QString("No session with the id or name \"%1\". smith --sessions lists them.").arg(idOrName));
}

QStringList deleteSession(const QString& id) {
  static const QRegularExpression valid(R"(^[\w.-]+$)");
  if (!valid.match(id).hasMatch()) {
    throw SmithError(QString("Not a session id: %1").arg(id));
  }
  const QStringList files = {sessionsDir() + "/" + id + ".jsonl", dataDir() + "/logs/" + id + ".jsonl"};
  if (!QFileInfo::exists(files.first())) {
    throw SmithError(QString("No session %1.").arg(id));
  }
  QStringList removed;
  for (const QString& file : files) {
    if (QFile::exists(file)) {
      if (!QFile::remove(file)) {
        throw SmithError(QString("Could not delete %1").arg(file));
      }
      removed << file;
    }
  }
  return removed;
}

} // namespace Smith
