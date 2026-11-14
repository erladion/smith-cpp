#include "core/Glob.h"

namespace Smith {

namespace {

QString toSource(const QString& pattern) {
  QString out;
  for (int i = 0; i < pattern.size(); i++) {
    const QChar c = pattern[i];
    const bool atSegmentStart = i == 0 || pattern[i - 1] == '/';
    auto at = [&](int index) { return index < pattern.size() ? pattern[index] : QChar(); };

    if (c == '*' && at(i + 1) == '*' && atSegmentStart) {
      if (at(i + 2) == '/') {
        // "**/": zero or more directories
        out += "(?:[^/]*/)*";
        i += 2;
        continue;
      }
      if (i + 2 == pattern.size()) {
        // trailing "**": everything below
        out += ".*";
        i += 1;
        continue;
      }
    }
    if (c == '*') {
      out += "[^/]*";
      while (at(i + 1) == '*') {
        i++;
      }
    } else if (c == '?') {
      out += "[^/]";
    } else if (c == '{') {
      const int end = pattern.indexOf('}', i);
      if (end == -1) {
        out += "\\{";
        continue;
      }
      QStringList alternatives;
      for (const QString& alternative : pattern.mid(i + 1, end - i - 1).split(',')) {
        alternatives << toSource(alternative);
      }
      out += "(?:" + alternatives.join('|') + ")";
      i = end;
    } else if (c == '[') {
      const int end = pattern.indexOf(']', i + 2);
      if (end == -1) {
        out += "\\[";
        continue;
      }
      QString set = pattern.mid(i + 1, end - i - 1).replace("\\", "\\\\");
      if (set.startsWith('!')) {
        set = "^" + set.mid(1);
      }
      out += "[" + set + "]";
      i = end;
    } else if (QString(".+^$()|\\]").contains(c)) {
      out += QString("\\") + c;
    } else {
      out += c;
    }
  }
  return out;
}

} // namespace

QRegularExpression globToRegExp(const QString& pattern) {
  return QRegularExpression("^" + toSource(pattern) + "$");
}

} // namespace Smith
