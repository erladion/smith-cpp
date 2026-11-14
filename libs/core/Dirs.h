#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace Smith {

// The name the app's own folders go by.
inline const QString APP = QStringLiteral("smith");

QString homeDir();

// ~/.config/smith: the config file, preferences, personal AGENTS.md and skills.
QString configDir();
// ~/.local/share/smith: sessions and tool logs.
QString dataDir();

// Smith used to be called agent. Moves ~/.config/agent and ~/.local/share/agent
// over once; returns what was done, to tell the user.
QStringList moveOldFolders();

// A path with the home folder shown as ~.
QString tildify(const QString& path);
// "~" or "~/x" expanded to the home folder; other paths unchanged.
QString expandHome(const QString& path);

// Whether `path` is `dir` or inside it (plain string comparison).
bool isWithin(const QString& dir, const QString& path);

// The agent's own data is for your eyes only: folders 700, files 600.
void privateDir(const QString& dir);
void privateFile(const QString& file);
void privateTree(const QString& dir, const QRegularExpression& filePattern);

// realpath(3); empty when the path doesn't exist.
QString realPath(const QString& path);
// realpath for a path whose last parts may not exist yet.
QString realPathAllowingMissing(const QString& path);
bool isDirectory(const QString& path);
bool pathExists(const QString& path);

} // namespace Smith
