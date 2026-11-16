#include "Palette.h"

#include <QFontDatabase>

namespace Smith::Desktop {

QString pickMonospaceFamily() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  const QStringList installed = QFontDatabase::families();
#else
  const QStringList installed = QFontDatabase().families();
#endif
  for (const char* family : {"JetBrains Mono", "Fira Code", "Cascadia Code", "Source Code Pro", "Hack", "DejaVu Sans Mono", "Noto Sans Mono",
                                "Liberation Mono", "Ubuntu Mono"}) {
    if (installed.contains(family)) {
      return family;
    }
  }
  return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
}

Palette Palette::forTheme(Theme theme) {
  Palette palette;
  palette.monoFamily = pickMonospaceFamily();
  if (theme == Theme::Matrix) {
    palette.window = QColor("#040805");
    palette.surface = QColor("#08100a");
    palette.surfaceRaised = QColor("#0c170f");
    palette.border = QColor("#163020");
    palette.borderStrong = QColor("#23573a");
    palette.text = QColor("#d4d4d4");
    palette.textDim = QColor("#767676");
    palette.textFaint = QColor("#505050");
    palette.accent = QColor("#00ff41");
    palette.accentSoft = QColor("#19c95a");
    palette.accentWash = QColor("#0a2a14");
    palette.code = QColor("#7dffb6");
    palette.codeWash = QColor("#0b1f13");
    palette.info = QColor("#5fd7c8");
    palette.warn = QColor("#e8c46a");
    palette.error = QColor("#ff5c5c");
    palette.added = QColor("#46e48a");
    palette.addedWash = QColor("#0d2a17");
    palette.removed = QColor("#ff6b6b");
    palette.removedWash = QColor("#2c0f12");
    palette.userWash = QColor("#0b1d11");
    palette.selection = QColor("#1d5a33");
    palette.textFamily = palette.monoFamily;
    return palette;
  }
  palette.textFamily = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
  if (theme == Theme::Light) {
    palette.window = QColor("#f6f7f9");
    palette.surface = QColor("#ffffff");
    palette.surfaceRaised = QColor("#eef0f3");
    palette.border = QColor("#dde1e6");
    palette.borderStrong = QColor("#c2c8d0");
    palette.text = QColor("#1f2328");
    palette.textDim = QColor("#57606a");
    palette.textFaint = QColor("#8c959f");
    palette.accent = QColor("#0969da");
    palette.accentSoft = QColor("#3b82d6");
    palette.accentWash = QColor("#ddeeff");
    palette.code = QColor("#0b7285");
    palette.codeWash = QColor("#f0f3f6");
    palette.info = QColor("#0969da");
    palette.warn = QColor("#9a6700");
    palette.error = QColor("#cf222e");
    palette.added = QColor("#1a7f37");
    palette.addedWash = QColor("#dafbe1");
    palette.removed = QColor("#cf222e");
    palette.removedWash = QColor("#ffebe9");
    palette.userWash = QColor("#eaf1fb");
    palette.selection = QColor("#b6d7ff");
    return palette;
  }
  palette.window = QColor("#17181b");
  palette.surface = QColor("#1e1f23");
  palette.surfaceRaised = QColor("#26282d");
  palette.border = QColor("#2f3238");
  palette.borderStrong = QColor("#454a52");
  palette.text = QColor("#dcdfe4");
  palette.textDim = QColor("#9aa0a8");
  palette.textFaint = QColor("#636870");
  palette.accent = QColor("#61afef");
  palette.accentSoft = QColor("#4d8fc7");
  palette.accentWash = QColor("#1b2a3a");
  palette.code = QColor("#56b6c2");
  palette.codeWash = QColor("#1a2427");
  palette.info = QColor("#61afef");
  palette.warn = QColor("#e5c07b");
  palette.error = QColor("#f14c4c");
  palette.added = QColor("#7ec699");
  palette.addedWash = QColor("#1a2b20");
  palette.removed = QColor("#f07178");
  palette.removedWash = QColor("#2e1c1f");
  palette.userWash = QColor("#232a33");
  palette.selection = QColor("#2c4a6b");
  return palette;
}

namespace {

// The colors by the names theme files and QML use (DESKTOP_COLORS).
const QList<QPair<QString, QColor Palette::*>>& colorMembers() {
  static const QList<QPair<QString, QColor Palette::*>> members = {
      {"window", &Palette::window},         {"surface", &Palette::surface},       {"surfaceRaised", &Palette::surfaceRaised},
      {"border", &Palette::border},         {"borderStrong", &Palette::borderStrong}, {"text", &Palette::text},
      {"textDim", &Palette::textDim},       {"textFaint", &Palette::textFaint},   {"accent", &Palette::accent},
      {"accentSoft", &Palette::accentSoft}, {"accentWash", &Palette::accentWash}, {"code", &Palette::code},
      {"codeWash", &Palette::codeWash},     {"info", &Palette::info},             {"warn", &Palette::warn},
      {"error", &Palette::error},           {"added", &Palette::added},           {"addedWash", &Palette::addedWash},
      {"removed", &Palette::removed},       {"removedWash", &Palette::removedWash}, {"userWash", &Palette::userWash},
      {"selection", &Palette::selection},
  };
  return members;
}

} // namespace

Palette Palette::forTheme(const ThemeSpec& theme) {
  Palette palette = forTheme(theme.base);
  for (const auto& [name, member] : colorMembers()) {
    if (theme.colors.contains(name)) {
      palette.*member = QColor(QRgb(theme.colors.value(name)));
    }
  }
  return palette;
}

QVariantMap Palette::toVariant() const {
  QVariantMap out{{"monoFamily", monoFamily}, {"textFamily", textFamily}};
  for (const auto& [name, member] : colorMembers()) {
    out.insert(name, this->*member);
  }
  return out;
}

} // namespace Smith::Desktop
