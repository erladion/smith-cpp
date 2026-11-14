#pragma once

#include "tui/Style.h"

#include <QRandomGenerator>
#include <QStringList>

#include <limits>
#include <optional>
#include <vector>

namespace Smith::Tui {

// Digital rain: green glyphs falling down the screen, with a title that
// locks into place where the drops pass through it. One step per frame,
// drawn to a string (full screen) or to lines (inside other output).
class Rain {
public:
  struct Options {
    // Text that locks into the middle row as drops pass; empty for none.
    QString title;
    // Frames of falling rain before the title is complete; infinity rains until stopped.
    double frames = 36;
    // Frames to keep showing the finished title.
    int holdFrames = 12;
    // For tests: a fixed seed.
    std::optional<quint32> seed;
  };

  Rain(int cols, int rows, const Options& options);

  // Whether the animation is over: the title is complete and has been shown for a moment.
  bool done() const;
  // The title as shown so far, with spaces for letters not locked in yet.
  QString shownTitle() const;

  void step();
  // The whole screen, row by row from the top-left corner.
  QString render() const;
  // The rain as styled lines, for drawing it inside other output.
  QList<Line> lines() const;

  int cols() const { return m_cols; }
  int rows() const { return m_rows; }

private:
  enum class Shade { None, Head, Bright, Green, Faded, Title };

  struct Drop {
    // Row of the leading glyph; negative while it's still above the screen.
    double head = 0;
    // Rows per frame.
    double speed = 1;
    // Rows of trail behind the head.
    int length = 4;
  };

  QPair<Shade, QString> cell(int r, int c) const;
  Drop makeDrop(double spread);
  QString glyph();
  double random();

  int m_cols;
  int m_rows;
  QRandomGenerator m_random;
  std::vector<std::vector<QString>> m_glyphs;
  std::vector<std::optional<Drop>> m_drops;
  QStringList m_title;
  int m_titleRow;
  int m_titleCol;
  std::vector<bool> m_locked;
  double m_frames;
  int m_holdFrames;
  int m_frame = 0;
};

// `width` columns of a scrolling stream of glyphs with gaps, like code going
// by: each step of `offset` moves it one column to the left.
QString codeStream(int offset, int width);

// The glyphs: half-width katakana (one column wide), digits and a few symbols.
const QStringList& rainGlyphs();

} // namespace Smith::Tui
