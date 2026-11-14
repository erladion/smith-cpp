#pragma once

#include <QList>
#include <QString>

#include <optional>

namespace Smith::Tui {

enum class KeyName { Text, Paste, Char, Return, Backspace, Delete, Tab, Escape, Up, Down, Left, Right, Home, End, PageUp, PageDown };

struct Key {
  KeyName name = KeyName::Text;
  // For Text (typed characters), Paste, and Char (a letter with ctrl or alt).
  QString text;
  bool ctrl = false;
  bool meta = false;
  bool shift = false;

  bool isCtrl(QChar letter) const { return name == KeyName::Char && ctrl && text == QString(letter); }
};

// Turns raw terminal input into keys. Handles arrow and editing keys in the
// common xterm encodings, ctrl and alt combinations, bracketed paste (which
// may arrive split across several reads), and the kitty keyboard protocol's
// `CSI code;modifiers u` form, which is how terminals report Shift+Enter.
class KeyReader {
public:
  QList<Key> feed(const QString& data);

private:
  std::optional<QString> m_paste;
};

} // namespace Smith::Tui
