#include "core/LoopDetector.h"

namespace Smith {

namespace {

// Checked every so often rather than on every token: a search of the window each time adds up.
constexpr int CHECK_EVERY = 256;

} // namespace

bool LoopDetector::push(const QString& text) {
  m_text += text;
  if (m_text.size() > 2 * Window) {
    m_text = m_text.right(Window);
    m_checkedAt = 0;
  }
  if (m_text.size() < Stretch * Repeats || m_text.size() - m_checkedAt < CHECK_EVERY) {
    return false;
  }
  m_checkedAt = int(m_text.size());
  const QString window = m_text.right(Window);
  const QString tail = window.right(Stretch);
  if (tail.trimmed().isEmpty()) {
    return false;
  }
  // Non-overlapping, so a short phrase said over and over counts once per stretch it fills.
  int seen = 0;
  for (int at = window.indexOf(tail); at != -1; at = window.indexOf(tail, at + Stretch)) {
    seen++;
  }
  return seen >= Repeats;
}

} // namespace Smith
