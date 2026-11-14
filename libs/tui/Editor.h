#pragma once

#include "tui/Keys.h"
#include "tui/Style.h"

#include <QStringList>

namespace Smith::Tui {

// The prompt's input. Enter submits; pasted text keeps its newlines.
//
//   shift+enter, alt+enter, ctrl+j  new line
//   \ then enter                    new line (the backslash is removed)
//   ←/→, ctrl+←/→ (or alt+b/f)      move by character / word
//   home/end (or ctrl+a/e)          start / end
//   backspace, delete               delete a character
//   ctrl+w (or alt+backspace)       delete the previous word
//   ctrl+u / ctrl+k                 delete to the start / end
//   ↑/↓                             earlier / later inputs
class LineEditor {
public:
  // Applies a key. Returns true when Enter was pressed (submit).
  bool handle(const Key& key);
  // Clears the line and remembers it for ↑.
  QString commit();
  // The line with a block cursor, or the placeholder when empty.
  Line render(const QString& placeholder) const;
  // Replaces the text, with the cursor at the end.
  void setValue(const QString& value);

  const QString& value() const { return m_value; }
  int cursor() const { return m_cursor; }

private:
  void set(const QString& value);
  void insert(const QString& text);
  void deleteTo(int target);
  void recall(int direction);
  int prevBoundary() const;
  int nextBoundary() const;
  int wordStart() const;
  int wordEnd() const;

  QString m_value;
  // Index into the value (UTF-16 units), always on a grapheme boundary.
  int m_cursor = 0;
  QStringList m_history;
  // Position while browsing history; history size means "the line being typed".
  int m_browsing = 0;
  QString m_draft;
};

} // namespace Smith::Tui
