#pragma once

#include "tui/Style.h"

#include <functional>

namespace Smith::Tui {

// Where the renderer draws: the terminal, or a test double.
struct Screen {
  std::function<void(const QString&)> write;
  std::function<int()> columns;
  std::function<int()> rows;
};

// Inline rendering: finished output is printed once and scrolls into the
// terminal's normal scrollback, and only a small "live" region at the bottom
// (input, spinner, prompts, status) is erased and redrawn.
//
// The cursor always rests at the end of the live region's last line, so
// erasing it means: go to column 0, up (rows - 1) lines, clear to the end.
class InlineRenderer {
public:
  explicit InlineRenderer(Screen screen);

  // Queues lines to print above the live region on the next draw.
  void print(const QList<Line>& lines);
  // Prints queued lines and replaces the live region with `live`.
  void draw(const QList<Line>& live);
  // Clears the screen and scrollback and starts over at the top (after a width change).
  void reset();
  // Erases the live region and prints anything queued, leaving the cursor on a fresh line.
  void finish();

private:
  QString eraseLive() const;

  Screen m_screen;
  int m_liveRows = 0;
  QString m_lastFrame;
  QList<Line> m_pending;
};

} // namespace Smith::Tui
