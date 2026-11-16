#include "core/Themes.h"

#include "core/Dirs.h"
#include "core/Preferences.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

namespace Smith {

const QStringList DESKTOP_COLORS = {
    "window", "surface", "surfaceRaised", "border",  "borderStrong", "text",    "textDim",     "textFaint",
    "accent", "accentSoft", "accentWash", "code",    "codeWash",     "info",    "warn",        "error",
    "added",  "addedWash",  "removed",    "removedWash", "userWash", "selection",
};

const QStringList TERMINAL_COLORS = {"text", "red", "green", "yellow", "blue", "magenta", "cyan", "white", "gray"};

const QMap<QString, QString> DERIVED_TERMINAL_COLORS = {
    {"red", "error"}, {"green", "added"}, {"yellow", "warn"}, {"blue", "accent"}, {"cyan", "code"}, {"white", "text"}, {"gray", "textDim"},
};

namespace {

// Like a tool's name: what a file can be called and still be typed after /theme.
const QRegularExpression THEME_NAME("^[A-Za-z][A-Za-z0-9_-]{0,47}$");

ThemeSpec builtIn(Theme theme) {
  ThemeSpec spec;
  spec.name = themeName(theme);
  spec.base = theme;
  return spec;
}

// The colors under `key`, which may only be `allowed` ones; false with `problem` set if any isn't right.
bool readColors(const QJsonObject& file, const QString& key, const QStringList& allowed, QMap<QString, quint32>& out, QString& problem) {
  if (!file.contains(key)) {
    return true;
  }
  if (!file.value(key).isObject()) {
    problem = QString("\"%1\" isn't an object of colors").arg(key);
    return false;
  }
  const QJsonObject colors = file.value(key).toObject();
  for (auto it = colors.begin(); it != colors.end(); ++it) {
    if (!allowed.contains(it.key())) {
      problem = QString("\"%1\" has no color \"%2\"; it has %3").arg(key, it.key(), allowed.join(", "));
      return false;
    }
    const std::optional<quint32> color = parseHexColor(it.value().toString());
    if (!color) {
      problem = QString("%1.%2 isn't a color like \"#1e1e2e\"").arg(key, it.key());
      return false;
    }
    out.insert(it.key(), *color);
  }
  return true;
}

// A theme file, or nullopt with why in `problem`.
std::optional<ThemeSpec> readTheme(const QString& name, const QString& file, QString& problem) {
  const QString shown = tildify(file);
  auto fail = [&](const QString& why) {
    problem = QString("The theme %1 %2.").arg(shown, why);
    return std::nullopt;
  };
  if (!THEME_NAME.match(name).hasMatch()) {
    return fail("has a name that isn't letters, digits, _ and - (starting with a letter)");
  }
  if (themeNamed(name)) {
    return fail("has the name of a built-in theme");
  }
  QFile handle(file);
  if (!handle.open(QIODevice::ReadOnly)) {
    return fail("can't be read");
  }
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(handle.readAll(), &error);
  if (!document.isObject()) {
    return fail("isn't a JSON object" + (error.error == QJsonParseError::NoError ? QString() : ": " + error.errorString()));
  }
  const QJsonObject json = document.object();
  for (const QString& key : json.keys()) {
    if (!QStringList{"description", "base", "colors", "terminal"}.contains(key)) {
      return fail(QString("has \"%1\", which a theme doesn't take (it takes description, base, colors and terminal)").arg(key));
    }
  }
  ThemeSpec spec;
  spec.name = name;
  spec.description = json.value("description").toString().trimmed();
  const QString base = json.value("base").toString("plain");
  if (!themeNamed(base)) {
    return fail(QString("has the base \"%1\"; it can be %2").arg(base, THEME_NAMES.join(", ")));
  }
  spec.base = *themeNamed(base);
  QString why;
  if (!readColors(json, "colors", DESKTOP_COLORS, spec.colors, why) || !readColors(json, "terminal", TERMINAL_COLORS, spec.terminal, why)) {
    return fail(why);
  }
  return spec;
}

} // namespace

QString themesDir() {
  return configDir() + "/themes";
}

std::optional<quint32> parseHexColor(const QString& text) {
  static const QRegularExpression hex("^#([0-9a-fA-F]{3}|[0-9a-fA-F]{6})$");
  const QRegularExpressionMatch match = hex.match(text.trimmed());
  if (!match.hasMatch()) {
    return std::nullopt;
  }
  QString digits = match.captured(1);
  if (digits.size() == 3) {
    digits = QString(digits[0]) + digits[0] + digits[1] + digits[1] + digits[2] + digits[2];
  }
  return digits.toUInt(nullptr, 16);
}

ThemeList listThemes() {
  ThemeList list;
  for (const QString& name : THEME_NAMES) {
    list.themes.append(builtIn(*themeNamed(name)));
  }
  const QDir dir(themesDir());
  for (const QString& entry : dir.entryList({"*.json"}, QDir::Files | QDir::Readable, QDir::Name)) {
    QString problem;
    if (const auto spec = readTheme(QFileInfo(entry).completeBaseName(), dir.filePath(entry), problem)) {
      list.themes.append(*spec);
    } else {
      list.problems << problem;
    }
  }
  return list;
}

std::optional<ThemeSpec> findTheme(const QString& name, QString* problem) {
  if (const auto theme = themeNamed(name)) {
    return builtIn(*theme);
  }
  QString why;
  const QString file = themesDir() + "/" + name + ".json";
  std::optional<ThemeSpec> spec;
  if (QFile::exists(file)) {
    spec = readTheme(name, file, why);
  } else {
    why = QString("There's no theme \"%1\": the built-in ones are %2, and yours go in %3/<name>.json.").arg(name, THEME_NAMES.join(", "), tildify(themesDir()));
  }
  if (problem) {
    *problem = why;
  }
  return spec;
}

QString themeHint(const ThemeSpec& theme) {
  if (!theme.description.isEmpty()) {
    return theme.description;
  }
  if (themeNamed(theme.name)) {
    return themeHint(theme.base);
  }
  return "yours, from " + themeName(theme.base);
}

ThemeSpec startingTheme(const QString& configured, QStringList* warnings) {
  const QString picked = loadPreferences().theme;
  if (!picked.isEmpty()) {
    QString problem;
    if (const auto spec = findTheme(picked, &problem)) {
      return *spec;
    }
    if (warnings) {
      *warnings << problem;
    }
  }
  QString problem;
  if (const auto spec = findTheme(configured, &problem)) {
    return *spec;
  }
  if (warnings) {
    *warnings << problem + " Using " + themeName(Theme::Matrix) + ".";
  }
  return builtIn(Theme::Matrix);
}

} // namespace Smith
