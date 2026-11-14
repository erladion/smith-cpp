#include "tui/Editor.h"

#include "tui/Text.h"

#include <QRegularExpression>

namespace Smith::Tui {

bool LineEditor::handle(const Key& key) {
  const bool ctrl = key.ctrl;
  const bool meta = key.meta;
  const QString letter = key.name == KeyName::Char ? key.text : QString();

  if (key.name == KeyName::Return) {
    if (key.shift || key.meta || key.ctrl) {
      insert("\n");
    } else if (m_cursor > 0 && m_value[m_cursor - 1] == '\\') {
      // Line continuation, as in a shell: the backslash becomes the line break.
      deleteTo(m_cursor - 1);
      insert("\n");
    } else {
      return true;
    }
    return false;
  }
  if (key.name == KeyName::Text || key.name == KeyName::Paste) {
    insert(key.text);
  } else if (key.name == KeyName::Left && (ctrl || meta)) {
    m_cursor = wordStart();
  } else if (key.name == KeyName::Right && (ctrl || meta)) {
    m_cursor = wordEnd();
  } else if (key.name == KeyName::Left) {
    m_cursor = prevBoundary();
  } else if (key.name == KeyName::Right) {
    m_cursor = nextBoundary();
  } else if (key.name == KeyName::Home || (ctrl && letter == "a")) {
    m_cursor = 0;
  } else if (key.name == KeyName::End || (ctrl && letter == "e")) {
    m_cursor = m_value.size();
  } else if (meta && letter == "b") {
    m_cursor = wordStart();
  } else if (meta && letter == "f") {
    m_cursor = wordEnd();
  } else if (key.name == KeyName::Backspace && meta) {
    deleteTo(wordStart());
  } else if (key.name == KeyName::Backspace) {
    deleteTo(prevBoundary());
  } else if (key.name == KeyName::Delete) {
    deleteTo(nextBoundary());
  } else if (ctrl && letter == "w") {
    deleteTo(wordStart());
  } else if (ctrl && letter == "u") {
    deleteTo(0);
  } else if (ctrl && letter == "k") {
    deleteTo(m_value.size());
  } else if (key.name == KeyName::Up) {
    recall(-1);
  } else if (key.name == KeyName::Down) {
    recall(1);
  }
  return false;
}

QString LineEditor::commit() {
  const QString text = m_value;
  if (!text.trimmed().isEmpty() && (m_history.isEmpty() || m_history.last() != text)) {
    m_history << text;
  }
  m_browsing = m_history.size();
  m_draft.clear();
  set({});
  return text;
}

Line LineEditor::render(const QString& placeholder) const {
  const Style cursorStyle{.inverse = true};
  if (m_value.isEmpty()) {
    return {Span{" ", cursorStyle}, Span{placeholder, DIM}};
  }
  const QString at = m_value.mid(m_cursor, nextBoundary() - m_cursor);
  const QString under = at.isEmpty() || at == "\n" ? QString(" ") : at;
  return {
      Span{m_value.left(m_cursor), {}},
      Span{under, cursorStyle},
      Span{(at == "\n" ? QString("\n") : QString()) + m_value.mid(m_cursor + at.size()), {}},
  };
}

void LineEditor::setValue(const QString& value) {
  set(value);
}

void LineEditor::set(const QString& value) {
  m_value = value;
  m_cursor = value.size();
}

void LineEditor::insert(const QString& text) {
  static const QRegularExpression crlf(R"(\r\n?)");
  QString clean = text;
  clean.replace(crlf, "\n");
  m_value = m_value.left(m_cursor) + clean + m_value.mid(m_cursor);
  m_cursor += clean.size();
}

void LineEditor::deleteTo(int target) {
  const int from = std::min(target, m_cursor);
  const int to = std::max(target, m_cursor);
  m_value = m_value.left(from) + m_value.mid(to);
  m_cursor = from;
}

void LineEditor::recall(int direction) {
  const int next = m_browsing + direction;
  if (next < 0 || next > m_history.size()) {
    return;
  }
  if (m_browsing == m_history.size()) {
    m_draft = m_value;
  }
  m_browsing = next;
  set(next == m_history.size() ? m_draft : m_history[next]);
}

int LineEditor::prevBoundary() const {
  const QStringList before = graphemes(m_value.left(m_cursor));
  return m_cursor - (before.isEmpty() ? 0 : before.last().size());
}

int LineEditor::nextBoundary() const {
  const QStringList after = graphemes(m_value.mid(m_cursor, 64));
  return m_cursor + (after.isEmpty() ? 0 : after.first().size());
}

int LineEditor::wordStart() const {
  static const QRegularExpression lastWord(R"(\S+\s*$|\s+$)");
  QString before = m_value.left(m_cursor);
  before.remove(lastWord);
  return before.size();
}

int LineEditor::wordEnd() const {
  static const QRegularExpression nextWord(R"(^\s*\S+|^\s+)");
  const QRegularExpressionMatch match = nextWord.match(m_value.mid(m_cursor));
  return m_cursor + (match.hasMatch() ? match.capturedLength(0) : 0);
}

} // namespace Smith::Tui
