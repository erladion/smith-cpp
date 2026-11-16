#pragma once

#include <QByteArray>
#include <QMap>
#include <QMutex>
#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>

namespace Smith {

// What one turn changed through the file tools, so it can be put back.
// Changes made by shell commands can't be seen here; `untracked` records
// which such tools ran, so the user can be told. Tools may run in parallel,
// so it locks.
class Checkpoint {
public:
  struct FileChange {
    // Content before the turn's first change; nullopt if the file didn't exist.
    std::optional<QByteArray> before;
    // Content after the turn's last change.
    std::optional<QByteArray> after;
  };

  // Call before writing `file`; remembers its content the first time.
  void beforeWrite(const QString& file);
  void afterWrite(const QString& file, const QByteArray& content);
  void addUntracked(const QString& tool);

  QMap<QString, FileChange> files() const;
  QStringList untracked() const;

private:
  mutable QMutex m_mutex;
  QMap<QString, FileChange> m_files;
  QStringList m_untracked;
};

struct RestoreResult {
  QStringList restored;
  // Files left alone because they changed after the agent wrote them.
  QStringList skipped;
};

// A file as it was before the agent first changed it, and as it is now; nullopt where it doesn't exist.
struct ChangedFile {
  QString file;
  std::optional<QByteArray> before;
  std::optional<QByteArray> now;
};

// Puts files back as they were before the checkpoint, leaving alone any file changed since.
RestoreResult restore(const Checkpoint& checkpoint);

// A file's bytes, or nullopt if it doesn't exist.
std::optional<QByteArray> readOrNull(const QString& file);

} // namespace Smith
