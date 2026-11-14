#pragma once

#include <QString>
#include <QStringList>

namespace Smith::Ui {

struct FolderListing {
  QStringList names;
  QString error;
};

// Subfolders of `dir`, by name, case-insensitively sorted; hidden ones only if asked for.
FolderListing listFolders(const QString& dir, bool includeHidden = false);

bool isGitRepo(const QString& dir);

// A path shortened to `max` characters by dropping folders from the left: "…/work/webapp".
QString shortPath(const QString& path, int max);

} // namespace Smith::Ui
