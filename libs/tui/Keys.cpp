#include "tui/Keys.h"

#include <QRegularExpression>

namespace Smith::Tui {

namespace {

const QString PASTE_START = "\x1b[200~";
const QString PASTE_END = "\x1b[201~";

std::optional<KeyName> finalKey(const QString& final) {
  if (final == "A") {
    return KeyName::Up;
  }
  if (final == "B") {
    return KeyName::Down;
  }
  if (final == "C") {
    return KeyName::Right;
  }
  if (final == "D") {
    return KeyName::Left;
  }
  if (final == "H") {
    return KeyName::Home;
  }
  if (final == "F") {
    return KeyName::End;
  }
  return std::nullopt;
}

std::optional<KeyName> tildeKey(const QString& first) {
  if (first == "1" || first == "7") {
    return KeyName::Home;
  }
  if (first == "4" || first == "8") {
    return KeyName::End;
  }
  if (first == "3") {
    return KeyName::Delete;
  }
  if (first == "5") {
    return KeyName::PageUp;
  }
  if (first == "6") {
    return KeyName::PageDown;
  }
  return std::nullopt;
}

// Unicode code points the kitty protocol uses for keys that aren't characters.
std::optional<KeyName> kittyKey(int code) {
  switch (code) {
  case 13:
  case 57414:
    return KeyName::Return;
  case 27:
    return KeyName::Escape;
  case 9:
    return KeyName::Tab;
  case 127:
  case 8:
    return KeyName::Backspace;
  default:
    return std::nullopt;
  }
}

std::optional<Key> escapeKey(const QString& params, const QString& final) {
  const QStringList fields = params.split(';');
  const QString first = fields.value(0);
  const QString modifier = fields.value(1);
  // Fields may carry ":"-separated extras (alternate keys, event types); only the first part matters here.
  bool ok = false;
  int value = modifier.section(':', 0, 0).toInt(&ok);
  if (!ok || value < 1) {
    value = 1;
  }
  const int bits = value - 1;
  Key key;
  key.shift = bits & 1;
  key.meta = bits & 2;
  key.ctrl = bits & 4;
  if (final == "u") {
    const int code = first.section(':', 0, 0).toInt(&ok);
    if (!ok) {
      return std::nullopt;
    }
    if (const auto named = kittyKey(code)) {
      key.name = *named;
      return key;
    }
    const char32_t point = char32_t(code);
    const QString text = QString::fromUcs4(&point, 1);
    if (key.ctrl || key.meta) {
      key.name = KeyName::Char;
      key.text = text;
      return key;
    }
    Key typed;
    typed.name = KeyName::Text;
    typed.text = key.shift ? text.toUpper() : text;
    return typed;
  }
  if (final == "Z") {
    Key tab;
    tab.name = KeyName::Tab;
    tab.shift = true;
    return tab;
  }
  const std::optional<KeyName> name = final == "~" ? tildeKey(first) : finalKey(final);
  if (!name) {
    return std::nullopt;
  }
  key.name = *name;
  return key;
}

} // namespace

QList<Key> KeyReader::feed(const QString& data) {
  static const QRegularExpression csi(R"(^\x1b\[([0-9;:]*)([A-Za-z~]))");
  static const QRegularExpression ss3(R"(^\x1bO()([A-Za-z]))");
  QList<Key> keys;
  int i = 0;
  while (i < data.size()) {
    if (m_paste) {
      const int end = data.indexOf(PASTE_END, i);
      if (end == -1) {
        *m_paste += data.mid(i);
        break;
      }
      Key paste;
      paste.name = KeyName::Paste;
      paste.text = *m_paste + data.mid(i, end - i);
      keys.append(paste);
      m_paste.reset();
      i = end + PASTE_END.size();
      continue;
    }
    if (QStringView(data).mid(i).startsWith(PASTE_START)) {
      m_paste = QString();
      i += PASTE_START.size();
      continue;
    }

    const QChar ch = data[i];
    Key key;
    if (ch == '\x1b') {
      const QString rest = data.mid(i);
      QRegularExpressionMatch match = csi.match(rest);
      if (!match.hasMatch()) {
        match = ss3.match(rest);
      }
      if (match.hasMatch()) {
        if (const auto parsed = escapeKey(match.captured(1), match.captured(2))) {
          keys.append(*parsed);
        }
        i += match.capturedLength(0);
      } else if (rest.size() == 1 || rest[1] == '\x1b') {
        key.name = KeyName::Escape;
        keys.append(key);
        i += 1;
      } else {
        const QChar next = rest[1];
        key.meta = true;
        if (next == '\x7f' || next == '\b') {
          key.name = KeyName::Backspace;
        } else if (next == '\r') {
          key.name = KeyName::Return;
        } else {
          key.name = KeyName::Char;
          key.text = QString(next);
        }
        keys.append(key);
        i += 2;
      }
      continue;
    }
    if (ch == '\r') {
      key.name = KeyName::Return;
    } else if (ch == '\n') {
      // In raw mode Enter sends CR; LF only comes from Ctrl+J.
      key.name = KeyName::Return;
      key.ctrl = true;
    } else if (ch == '\x7f' || ch == '\b') {
      key.name = KeyName::Backspace;
    } else if (ch == '\t') {
      key.name = KeyName::Tab;
    } else if (ch.unicode() < 0x20) {
      key.name = KeyName::Char;
      key.text = QString(QChar(ch.unicode() + 96));
      key.ctrl = true;
    } else {
      int j = i;
      while (j < data.size() && data[j].unicode() >= 0x20 && data[j] != '\x7f') {
        j++;
      }
      key.name = KeyName::Text;
      key.text = data.mid(i, j - i);
      keys.append(key);
      i = j;
      continue;
    }
    keys.append(key);
    i += 1;
  }
  return keys;
}

} // namespace Smith::Tui
