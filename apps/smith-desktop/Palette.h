#pragma once

#include "core/Config.h"

#include <QColor>
#include <QString>
#include <QVariantMap>

namespace Smith::Desktop {

// The desktop app's colors and fonts, for the theme. "matrix" is green on
// near-black with monospace throughout, like the TUI's matrix theme; "plain"
// is a neutral dark theme and "light" a neutral light one.
struct Palette {
  QColor window;
  QColor surface;
  QColor surfaceRaised;
  QColor border;
  QColor borderStrong;
  QColor text;
  QColor textDim;
  QColor textFaint;
  QColor accent;
  QColor accentSoft;
  QColor accentWash;
  // Inline code and headings, the TUI's cyan.
  QColor code;
  QColor codeWash;
  QColor info;
  QColor warn;
  QColor error;
  QColor added;
  QColor addedWash;
  QColor removed;
  QColor removedWash;
  QColor userWash;
  QColor selection;
  QString monoFamily;
  // Prose and controls: the monospace font with the matrix theme, a sans one otherwise.
  QString textFamily;

  static Palette forTheme(Theme theme);
  QVariantMap toVariant() const;
};

// The first installed monospace font of the ones that suit a terminal-like UI.
QString pickMonospaceFamily();

} // namespace Smith::Desktop
