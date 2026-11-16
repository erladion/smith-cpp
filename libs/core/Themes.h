#pragma once

#include "core/Config.h"

#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

namespace Smith {

// A theme: one of the built-in looks, or a JSON file of yours in themesDir()
// that starts from one and changes some of its colors:
//
//   { "description": "Easy on the eyes at night",
//     "base": "plain",
//     "colors": { "window": "#1e1e2e", "accent": "#89b4fa" },
//     "terminal": { "magenta": "#cba6f7" } }
//
// The file's name is the theme's name. Terminal colors follow the desktop
// ones they stand for (DERIVED_TERMINAL_COLORS) unless "terminal" sets them.
struct ThemeSpec {
  QString name;
  // The look it starts from: its colors and fonts, and with matrix the rain and the rest.
  Theme base = Theme::Matrix;
  QString description;
  // Desktop color name ("accent") → 0xRRGGBB.
  QMap<QString, quint32> colors;
  // Terminal color name ("red") → 0xRRGGBB.
  QMap<QString, quint32> terminal;
};

// The colors a theme file can set: the desktop app's, and the terminal's
// (in the terminal's order: "text" is text without a color of its own).
extern const QStringList DESKTOP_COLORS;
extern const QStringList TERMINAL_COLORS;
// Terminal color → the desktop color it follows when the file sets that one.
extern const QMap<QString, QString> DERIVED_TERMINAL_COLORS;

QString themesDir();

// "#rgb" or "#rrggbb" as 0xRRGGBB.
std::optional<quint32> parseHexColor(const QString& text);

struct ThemeList {
  QList<ThemeSpec> themes;
  // Theme files that couldn't be used, and why.
  QStringList problems;
};

// The built-in themes, then the files in themesDir() by name.
ThemeList listThemes();

// The theme with this name, its file read now. Without one, nullopt and why in `problem`.
std::optional<ThemeSpec> findTheme(const QString& name, QString* problem = nullptr);

// What a theme looks like, for pickers.
QString themeHint(const ThemeSpec& theme);

// The theme to start with: the one picked with /theme, else the config's;
// if that can't be used, the default, and why in `warnings`.
ThemeSpec startingTheme(const QString& configured, QStringList* warnings = nullptr);

} // namespace Smith
