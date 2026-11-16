#pragma once

#include "core/Cancel.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace Smith {

struct SpawnSpec;

// Folders no listing goes into: version control, dependencies and build output.
extern const QStringList SKIP_DIRS;

struct FileEntry {
  // Slash-separated, relative to the folder listed.
  QString rel;
  QString abs;
};

// Every file under `base`, skipping SKIP_DIRS. Doesn't follow symlinked
// directories, so it can't leave `base`.
QList<FileEntry> walk(const QString& base, const CancelPtr& cancel);

// `rg --files`: much faster than walking the tree on big repositories, and it
// skips what .gitignore ignores. Hidden files are included.
QStringList ripgrepListArgs();

// `find` listing what `walk` would.
QStringList findArgs();

// Runs a file lister in `base` and returns the paths it prints, one per line.
QList<FileEntry> listFiles(const SpawnSpec& spec, const QString& base, const CancelPtr& cancel);

// The project's files for you to pick from (@ mentions): with ripgrep when
// it's installed, so .gitignored ones are left out, else walked. Relative,
// sorted, and at most `limit` of them.
QStringList projectFiles(const QString& cwd, const CancelPtr& cancel = {}, int limit = 50000);

} // namespace Smith
