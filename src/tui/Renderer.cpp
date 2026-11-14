#include "tui/Renderer.h"

#include <QStringList>

namespace Smith::Tui {

namespace {

// Terminals that support it show a frame only once it is fully drawn; others ignore these.
const QString BEGIN_FRAME = "\x1b[?2026h";
const QString END_FRAME = "\x1b[?2026l";

} // namespace

InlineRenderer::InlineRenderer(Screen screen) : m_screen(std::move(screen)) {}

void InlineRenderer::print(const QList<Line>& lines) {
  m_pending.append(lines);
}

void InlineRenderer::draw(const QList<Line>& live) {
  // A live region taller than the screen can't be erased (the cursor can't
  // move above the top), so keep only the bottom of it.
  const int rows = m_screen.rows ? m_screen.rows() : 24;
  const int maxRows = std::max(1, (rows > 0 ? rows : 24) - 1);
  const QList<Line> shown = live.size() > maxRows ? live.mid(live.size() - maxRows) : live;

  QStringList rendered;
  for (const Line& line : shown) {
    rendered << toAnsi(line);
  }
  const QString frame = rendered.join("\r\n");
  // Nothing new to print and the live region is unchanged: skip the redraw.
  if (m_pending.isEmpty() && frame == m_lastFrame && m_liveRows == shown.size()) {
    return;
  }

  QString out = BEGIN_FRAME + eraseLive();
  for (const Line& line : m_pending) {
    out += toAnsi(line) + "\r\n";
  }
  m_pending.clear();
  out += frame + END_FRAME;

  m_lastFrame = frame;
  m_liveRows = shown.size();
  m_screen.write(out);
}

void InlineRenderer::reset() {
  m_screen.write("\x1b[H\x1b[2J\x1b[3J");
  m_liveRows = 0;
  m_lastFrame.clear();
  m_pending.clear();
}

void InlineRenderer::finish() {
  draw({});
}

QString InlineRenderer::eraseLive() const {
  if (m_liveRows == 0) {
    return {};
  }
  return "\r" + (m_liveRows > 1 ? QString("\x1b[%1A").arg(m_liveRows - 1) : QString()) + "\x1b[J";
}

} // namespace Smith::Tui
