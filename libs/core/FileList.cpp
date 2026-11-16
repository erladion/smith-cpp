#include "core/FileList.h"
#include "compat/Utf8.h"

#include "core/Process.h"
#include "core/Sandbox.h"
#include "core/Toolchain.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace Smith {

namespace {

constexpr int MAX_WALK_ENTRIES = 100000;
constexpr qint64 MAX_LISTING_CHARS = 32000000;

} // namespace

const QStringList SKIP_DIRS = {".git", "node_modules", "dist", "build", ".venv", "__pycache__"};

QList<FileEntry> walk(const QString& base, const CancelPtr& cancel) {
  QList<FileEntry> files;
  QStringList stack{""};
  int seen = 0;
  while (!stack.isEmpty()) {
    const QString relDir = stack.takeLast();
    const QFileInfoList entries =
        QDir(relDir.isEmpty() ? base : base + "/" + relDir).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo& entry : entries) {
      if (cancelled(cancel) || ++seen > MAX_WALK_ENTRIES) {
        return files;
      }
      const QString rel = relDir.isEmpty() ? entry.fileName() : relDir + "/" + entry.fileName();
      if (entry.isDir() && !entry.isSymLink()) {
        if (!SKIP_DIRS.contains(entry.fileName())) {
          stack << rel;
        }
        continue;
      }
      files.append({rel, base + "/" + rel});
    }
  }
  return files;
}

QStringList ripgrepListArgs() {
  QStringList args{"--files", "--hidden", "--color", "never"};
  for (const QString& dir : SKIP_DIRS) {
    args << "--glob" << "!" + dir;
  }
  return args;
}

QStringList findArgs() {
  QStringList skip;
  for (int i = 0; i < SKIP_DIRS.size(); i++) {
    if (i) {
      skip << "-o";
    }
    skip << "-name" << SKIP_DIRS[i];
  }
  return QStringList{".", "-mindepth", "1", "(", "-type", "d", "("} + skip + QStringList{")", "-prune", ")", "-o", "(", "!", "-type", "d", "-print", ")"};
}

QList<FileEntry> listFiles(const SpawnSpec& spec, const QString& base, const CancelPtr& cancel) {
  RunOptions options;
  options.cancel = cancel;
  options.maxCapture = MAX_LISTING_CHARS;
  options.stopAtCapture = true;
  const ProcessResult result = runProcess(spec, options);
  QList<FileEntry> files;
  for (const QByteArray& raw : result.out.split('\n')) {
    if (raw.isEmpty()) {
      continue;
    }
    QString rel = utf8(raw);
    if (rel.startsWith("./")) {
      rel = rel.mid(2);
    }
    files.append({rel, base + "/" + rel});
  }
  return files;
}

QStringList projectFiles(const QString& cwd, const CancelPtr& cancel, int limit) {
  const QString rg = fastTools().rg;
  QList<FileEntry> entries;
  if (!rg.isEmpty()) {
    entries = listFiles(Sandbox::none()->programSpec(rg, ripgrepListArgs(), cwd, cwd), cwd, cancel);
  } else {
    entries = walk(cwd, cancel);
  }
  QStringList files;
  for (const FileEntry& entry : entries) {
    files << entry.rel;
  }
  std::sort(files.begin(), files.end());
  return files.mid(0, limit);
}

} // namespace Smith
