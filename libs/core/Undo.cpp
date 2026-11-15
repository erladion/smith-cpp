#include "core/Undo.h"

#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>

namespace Smith {

std::optional<QByteArray> readOrNull(const QString& file) {
  QFile handle(file);
  if (!handle.exists() || !handle.open(QIODevice::ReadOnly)) {
    return std::nullopt;
  }
  return handle.readAll();
}

void Checkpoint::beforeWrite(const QString& file) {
  QMutexLocker lock(&m_mutex);
  if (!m_files.contains(file)) {
    m_files.insert(file, FileChange{readOrNull(file), std::nullopt});
  }
}

void Checkpoint::afterWrite(const QString& file, const QByteArray& content) {
  QMutexLocker lock(&m_mutex);
  if (m_files.contains(file)) {
    m_files[file].after = content;
  }
}

void Checkpoint::addUntracked(const QString& tool) {
  QMutexLocker lock(&m_mutex);
  if (!m_untracked.contains(tool)) {
    m_untracked << tool;
  }
}

QMap<QString, Checkpoint::FileChange> Checkpoint::files() const {
  QMutexLocker lock(&m_mutex);
  return m_files;
}

QStringList Checkpoint::untracked() const {
  QMutexLocker lock(&m_mutex);
  return m_untracked;
}

RestoreResult restore(const Checkpoint& checkpoint) {
  RestoreResult result;
  const auto files = checkpoint.files();
  for (auto it = files.begin(); it != files.end(); ++it) {
    const QString& file = it.key();
    if (readOrNull(file) != it.value().after) {
      result.skipped << file;
      continue;
    }
    bool done = false;
    if (!it.value().before) {
      done = QFile::remove(file);
    } else {
      QFile handle(file);
      done = handle.open(QIODevice::WriteOnly | QIODevice::Truncate) && handle.write(*it.value().before) >= 0;
    }
    // One that couldn't be put back is left as it is, and said so.
    (done ? result.restored : result.skipped) << file;
  }
  return result;
}

} // namespace Smith
