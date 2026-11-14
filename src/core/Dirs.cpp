#include "core/Dirs.h"

#include <QDir>
#include <QFileInfo>

#include <sys/stat.h>

#include <climits>
#include <cstdlib>

namespace Smith {

namespace {

// What it was called before, for moving old folders over.
const QString OLD_APP = "agent";

QString configBase() {
  const QString xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
  return xdg.isEmpty() ? homeDir() + "/.config" : xdg;
}

QString dataBase() {
  const QString xdg = qEnvironmentVariable("XDG_DATA_HOME");
  return xdg.isEmpty() ? homeDir() + "/.local/share" : xdg;
}

void tighten(const QString& path, mode_t mode) {
  struct stat info;
  const QByteArray native = QFile::encodeName(path);
  if (::stat(native.constData(), &info) != 0) {
    return;
  }
  if ((info.st_mode & 0777) != mode) {
    ::chmod(native.constData(), mode);
  }
}

} // namespace

QString homeDir() {
  return QDir::homePath();
}

QString configDir() {
  return configBase() + "/" + APP;
}

QString dataDir() {
  return dataBase() + "/" + APP;
}

QStringList moveOldFolders() {
  struct Move {
    QString from;
    QString to;
    QStringList marks;
  };
  const QList<Move> moves = {
      {configBase() + "/" + OLD_APP, configDir(), {"config.json", "preferences.json", "AGENTS.md", "skills"}},
      {dataBase() + "/" + OLD_APP, dataDir(), {"sessions", "logs"}},
  };
  QStringList notes;
  for (const Move& move : moves) {
    bool hasMark = false;
    for (const QString& mark : move.marks) {
      hasMark = hasMark || pathExists(move.from + "/" + mark);
    }
    if (pathExists(move.to) || !hasMark) {
      continue;
    }
    if (QDir().rename(move.from, move.to)) {
      notes << QString("Moved %1 to %2: agent is now called Smith.").arg(tildify(move.from), tildify(move.to));
    } else {
      notes << QString("Could not move %1 to %2; move it yourself to keep its contents.").arg(tildify(move.from), tildify(move.to));
    }
  }
  return notes;
}

QString tildify(const QString& path) {
  const QString home = homeDir();
  if (path == home) {
    return "~";
  }
  if (path.startsWith(home + "/")) {
    return "~" + path.mid(home.size());
  }
  return path;
}

QString expandHome(const QString& path) {
  if (path == "~" || path.startsWith("~/")) {
    return homeDir() + path.mid(1);
  }
  return path;
}

bool isWithin(const QString& dir, const QString& path) {
  return path == dir || path.startsWith(dir.endsWith('/') ? dir : dir + "/");
}

void privateDir(const QString& dir) {
  QDir().mkpath(QFileInfo(dir).path());
  QDir().mkpath(dir);
  tighten(dir, 0700);
}

void privateFile(const QString& file) {
  tighten(file, 0600);
}

void privateTree(const QString& dir, const QRegularExpression& filePattern) {
  if (!pathExists(dir)) {
    return;
  }
  tighten(dir, 0700);
  const QStringList names = QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
  for (const QString& name : names) {
    if (filePattern.match(name).hasMatch()) {
      tighten(dir + "/" + name, 0600);
    }
  }
}

QString realPath(const QString& path) {
  char buffer[PATH_MAX];
  const QByteArray native = QFile::encodeName(path);
  if (::realpath(native.constData(), buffer) == nullptr) {
    return {};
  }
  return QFile::decodeName(buffer);
}

QString realPathAllowingMissing(const QString& path) {
  QString existing = path;
  QString rest;
  for (;;) {
    const QString real = realPath(existing);
    if (!real.isEmpty()) {
      return rest.isEmpty() ? real : QDir::cleanPath(real + "/" + rest);
    }
    const QString parent = QFileInfo(existing).path();
    if (parent == existing || existing.isEmpty()) {
      return path;
    }
    const QString name = QFileInfo(existing).fileName();
    rest = rest.isEmpty() ? name : name + "/" + rest;
    existing = parent;
  }
}

bool isDirectory(const QString& path) {
  return QFileInfo(path).isDir();
}

bool pathExists(const QString& path) {
  return QFileInfo::exists(path);
}

} // namespace Smith
