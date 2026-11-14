#include "ui/Folders.h"

#include "core/Dirs.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace Smith::Ui {

namespace {

constexpr int MAX_FOLDERS = 500;

} // namespace

FolderListing listFolders(const QString& dir, bool includeHidden) {
  QFileInfo info(dir);
  if (!info.exists()) {
    return {{}, QString("ENOENT: no such file or directory, scandir '%1'").arg(dir)};
  }
  if (!info.isReadable() || !info.isExecutable()) {
    return {{}, "no permission to list this folder"};
  }
  QDir::Filters filters = QDir::Dirs | QDir::NoDotAndDotDot;
  if (includeHidden) {
    filters |= QDir::Hidden;
  }
  QStringList names = QDir(dir).entryList(filters);
  std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
  return {names.mid(0, MAX_FOLDERS), {}};
}

bool isGitRepo(const QString& dir) {
  return pathExists(dir + "/.git");
}

QString shortPath(const QString& path, int max) {
  const QString shown = tildify(path);
  if (shown.size() <= max) {
    return shown;
  }
  QStringList parts = shown.split('/');
  QString tail = parts.isEmpty() ? QString() : parts.takeLast();
  while (!parts.isEmpty() && tail.size() + parts.last().size() + 3 <= max) {
    tail = parts.takeLast() + "/" + tail;
  }
  return tail.size() + 2 <= max ? "…/" + tail : "…" + tail.right(max - 1);
}

} // namespace Smith::Ui
