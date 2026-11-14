#include "core/Instructions.h"

#include "core/Dirs.h"

#include <QFile>
#include <QFileInfo>

namespace Smith {

namespace {

constexpr int MAX_CHARS = 20000;

} // namespace

QString findRepoRoot(const QString& cwd) {
  for (QString dir = cwd;; dir = QFileInfo(dir).path()) {
    if (pathExists(dir + "/.git")) {
      return dir;
    }
    if (QFileInfo(dir).path() == dir) {
      return {};
    }
  }
}

QList<InstructionFile> loadInstructions(const QString& cwd) {
  QList<InstructionFile> files;
  auto add = [&](const QString& file) {
    QFile handle(file);
    if (!QFileInfo(file).isFile() || !handle.open(QIODevice::ReadOnly)) {
      return false;
    }
    QString content = QString::fromUtf8(handle.readAll()).trimmed();
    if (content.size() > MAX_CHARS) {
      content = content.left(MAX_CHARS) + QString("\n\n[Truncated: the file is longer than %1 characters.]").arg(MAX_CHARS);
    }
    if (!content.isEmpty()) {
      files.append({file, content});
    }
    return true;
  };

  add(configDir() + "/AGENTS.md");
  QString root = findRepoRoot(cwd);
  if (root.isEmpty()) {
    root = cwd;
  }
  QStringList dirs;
  for (QString dir = cwd;; dir = QFileInfo(dir).path()) {
    dirs.prepend(dir);
    if (dir == root || QFileInfo(dir).path() == dir) {
      break;
    }
  }
  for (const QString& dir : dirs) {
    if (!add(dir + "/AGENTS.md")) {
      add(dir + "/CLAUDE.md");
    }
  }
  return files;
}

} // namespace Smith
