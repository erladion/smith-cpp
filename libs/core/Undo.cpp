#include "core/Undo.h"

#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>

namespace Smith {

std::optional<QString> readOrNull(const QString& file) {
  QFile handle(file);
  if (!handle.exists() || !handle.open(QIODevice::ReadOnly)) {
    return std::nullopt;
  }
  return QString::fromUtf8(handle.readAll());
}

void Checkpoint::beforeWrite(const QString& file) {
  QMutexLocker lock(&m_mutex);
  if (!m_files.contains(file)) {
    m_files.insert(file, FileChange{readOrNull(file), std::nullopt});
  }
}

void Checkpoint::afterWrite(const QString& file, const QString& content) {
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
    if (!it.value().before) {
      QFile::remove(file);
    } else {
      QFile handle(file);
      if (handle.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        handle.write(it.value().before->toUtf8());
      }
    }
    result.restored << file;
  }
  return result;
}

} // namespace Smith
