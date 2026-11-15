#pragma once

#include <QList>
#include <QMap>
#include <QString>

namespace Smith {

// Files in a project that run code later, outside the sandbox, without you
// deliberately running the project: when you enter the folder, open it in an
// editor, commit, push (CI), install dependencies, build, or start its
// containers. A change to one is how something in the sandbox could get out.

// Why a file (relative to the project, with / separators) runs outside the sandbox later; empty if it doesn't.
QString runLaterReason(const QString& rel);

// A fingerprint of every such file in the project: relative path → content hash.
// Git folders are in it too (".git", "vendor/lib/.git"), by their config and hooks.
QMap<QString, QString> snapshotRunLater(const QString& cwd);

// The fingerprint a snapshot has for a file with this content.
QString hashBytes(const QByteArray& content);

struct RunLaterChange {
  QString rel;
  // "changed", "created" or "deleted".
  QString kind;
  QString why;
};

// What changed between two snapshots, sorted by path.
QList<RunLaterChange> runLaterChanges(const QMap<QString, QString>& before, const QMap<QString, QString>& after);

} // namespace Smith
