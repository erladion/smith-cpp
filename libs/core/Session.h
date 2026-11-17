#pragma once

#include "core/History.h"

#include <QDateTime>
#include <QJsonObject>
#include <QMutex>
#include <QString>

#include <memory>
#include <optional>

namespace Smith {

// Sessions are append-only JSONL logs, one file per conversation:
//
//   {"type":"meta", "id", "cwd", "model", "endpoint"?, "created"}
//   {"type":"append", "message": {...}}        one per message added
//   {"type":"replace", "messages": [...]}      after compaction or /clear
//   {"type":"model", "model", "endpoint"?}     when the user switches model or endpoint
//   {"type":"name", "name"}                    when the user names the session (/name)
//
// Replaying the log rebuilds the conversation (without the system prompt,
// which is regenerated on resume). Appending instead of rewriting means a
// crash can lose at most the line being written.

struct SessionInfo {
  QString id;
  QString cwd;
  QString model;
  QString endpoint;
  QString created;
  QDateTime updated;
  QString file;
  // First user message, for listings.
  QString preview;
  // Number of messages you sent, counting those compaction summarized.
  int turns = 0;
  // The session's name: set with /name, or else taken from the first message.
  QString name;
  // Whether the name was set with /name.
  bool named = false;
};

QString sessionsDir();

class Session {
public:
  // A new session. Its file is created on the first change, so empty sessions leave nothing behind.
  static std::shared_ptr<Session> create(const QString& cwd, const QString& model, const QString& endpoint);

  struct Opened {
    std::shared_ptr<Session> session;
    Messages messages;
  };

  // Opens an existing session for appending and returns its conversation so far.
  static Opened open(const QString& id);

  QString id() const { return m_id; }
  QString file() const { return m_file; }
  QString cwd() const;
  QString model() const;
  QString endpoint() const;
  // The name given with /name; empty if none.
  QString name() const;
  // For an opened session: when it was last written to, before this run.
  std::optional<QDateTime> lastUsed() const { return m_lastUsed; }

  void record(const HistoryChange& change);
  // Names the session. Unlike other changes this saves it even before its first message.
  void rename(const QString& name);
  void recordModel(const QString& model, const QString& endpoint);

private:
  Session(const QString& id, const QString& file, const QJsonObject& meta);
  void write(const QJsonObject& line);

  QString m_id;
  QString m_file;
  QJsonObject m_meta;
  mutable QMutex m_mutex;
  bool m_started = false;
  // Whether this run has checked the file's permissions.
  bool m_secured = false;
  QString m_model;
  QString m_endpoint;
  QString m_name;
  std::optional<QDateTime> m_lastUsed;
};

// Makes existing session files private; files from older versions were readable by others.
void secureSessions();

// Sessions, newest first, optionally only those started in `cwd`; stops after `limit` matches.
QList<SessionInfo> listSessions(const QString& cwd = {}, int limit = -1);

// A name from the first message, until the session is given one with /name.
QString defaultName(const QString& firstMessage);

// Finds a session by id, or by name (ignoring case); throws with the candidates when ambiguous.
QString findSession(const QString& idOrName);

// Deletes a session's log and its tool log; returns the files removed.
QStringList deleteSession(const QString& id);

} // namespace Smith
