#include "tui/Text.h"

#include <QRegularExpression>
#include <QTextBoundaryFinder>

#include <algorithm>
#include <utility>

namespace Smith::Tui {

namespace {

// East Asian wide and fullwidth ranges, plus the emoji blocks terminals draw two columns wide.
constexpr std::pair<char32_t, char32_t> WIDE[] = {
    {0x1100, 0x115f},   {0x231a, 0x231b},   {0x2329, 0x232a},   {0x23e9, 0x23ec},   {0x23f0, 0x23f0},   {0x23f3, 0x23f3},
    {0x25fd, 0x25fe},   {0x2614, 0x2615},   {0x2648, 0x2653},   {0x267f, 0x267f},   {0x2693, 0x2693},   {0x26a1, 0x26a1},
    {0x26aa, 0x26ab},   {0x26bd, 0x26be},   {0x26c4, 0x26c5},   {0x26ce, 0x26ce},   {0x26d4, 0x26d4},   {0x26ea, 0x26ea},
    {0x26f2, 0x26f5},   {0x26fa, 0x26fd},   {0x2705, 0x2705},   {0x270a, 0x270b},   {0x2728, 0x2728},   {0x274c, 0x274c},
    {0x2753, 0x2757},   {0x2795, 0x2797},   {0x27b0, 0x27b0},   {0x27bf, 0x27bf},   {0x2b1b, 0x2b1c},   {0x2b50, 0x2b55},
    {0x2e80, 0x303e},   {0x3041, 0x33ff},   {0x3400, 0x4dbf},   {0x4e00, 0x9fff},   {0xa000, 0xa4cf},   {0xa960, 0xa97f},
    {0xac00, 0xd7a3},   {0xf900, 0xfaff},   {0xfe10, 0xfe19},   {0xfe30, 0xfe6f},   {0xff00, 0xff60},   {0xffe0, 0xffe6},
    {0x1f004, 0x1f004}, {0x1f0cf, 0x1f0cf}, {0x1f18e, 0x1f18e}, {0x1f191, 0x1f19a}, {0x1f200, 0x1f251}, {0x1f300, 0x1f64f},
    {0x1f680, 0x1f6ff}, {0x1f7e0, 0x1f7eb}, {0x1f90c, 0x1f9ff}, {0x1fa70, 0x1faff}, {0x20000, 0x3fffd},
};

bool isWide(char32_t cp) {
  int lo = 0;
  int hi = int(std::size(WIDE)) - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (cp < WIDE[mid].first) {
      hi = mid - 1;
    } else if (cp > WIDE[mid].second) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

bool isPlainAscii(const QString& text) {
  for (const QChar c : text) {
    if (c.unicode() < 0x20 || c.unicode() > 0x7e) {
      return false;
    }
  }
  return true;
}

bool needsSanitizing(const QString& text) {
  for (const QChar c : text) {
    const ushort u = c.unicode();
    if (u < 0x20 || (u >= 0x7f && u <= 0x9f)) {
      return true;
    }
  }
  return false;
}

// Splits into runs of spaces and runs of everything else.
QStringList tokens(const QString& text) {
  QStringList out;
  int start = 0;
  while (start < text.size()) {
    const bool space = text[start] == ' ';
    int end = start;
    while (end < text.size() && (text[end] == ' ') == space) {
      end++;
    }
    out << text.mid(start, end - start);
    start = end;
  }
  return out;
}

Line trimEnd(const Line& line) {
  Line out = line;
  while (!out.isEmpty()) {
    Span& last = out.last();
    if (last.style.inverse) {
      break;
    }
    while (last.text.endsWith(' ')) {
      last.text.chop(1);
    }
    if (!last.text.isEmpty()) {
      break;
    }
    out.removeLast();
  }
  return out;
}

QString flat(const QString& text) {
  return sanitize(text).replace('\n', ' ');
}

} // namespace

QString sanitize(const QString& text) {
  if (!needsSanitizing(text)) {
    return text;
  }
  static const QRegularExpression crlf(R"(\r\n?)");
  static const QRegularExpression csi(R"(\x1b\[[0-?]*[ -/]*[@-~])");
  static const QRegularExpression osc(R"(\x1b[\]PX^_][\s\S]*?(?:\x07|\x1b\\|\z))");
  static const QRegularExpression controls(R"([\x00-\x09\x0b-\x1f\x7f-\x9f])");
  QString out = text;
  out.replace(crlf, "\n");
  out.remove(csi);
  out.remove(osc);
  out.replace('\t', "    ");
  out.remove(controls);
  return out;
}

QStringList graphemes(const QString& text) {
  QStringList out;
  if (isPlainAscii(text)) {
    out.reserve(text.size());
    for (const QChar c : text) {
      out << QString(c);
    }
    return out;
  }
  QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
  int previous = 0;
  while (finder.toNextBoundary() != -1) {
    const int position = finder.position();
    if (position > previous) {
      out << text.mid(previous, position - previous);
    }
    previous = position;
  }
  return out;
}

int graphemeWidth(const QString& grapheme) {
  if (grapheme.isEmpty()) {
    return 0;
  }
  const QList<uint> codePoints = grapheme.toUcs4();
  const char32_t cp = codePoints.first();
  if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) {
    return 0;
  }
  if (cp < 0x300) {
    return 1;
  }
  bool zeroWidth = true;
  for (const uint point : codePoints) {
    const QChar::Category category = QChar::category(char32_t(point));
    if (category != QChar::Mark_NonSpacing && category != QChar::Mark_Enclosing && category != QChar::Other_Format) {
      zeroWidth = false;
      break;
    }
  }
  if (zeroWidth) {
    return 0;
  }
  // An emoji presentation selector makes even a narrow symbol two columns wide.
  if (codePoints.contains(0xfe0f)) {
    return 2;
  }
  return isWide(cp) ? 2 : 1;
}

int textWidth(const QString& text) {
  if (isPlainAscii(text)) {
    return text.size();
  }
  int width = 0;
  for (const QString& grapheme : graphemes(text)) {
    width += graphemeWidth(grapheme);
  }
  return width;
}

int lineWidth(const Line& line) {
  int width = 0;
  for (const Span& span : line) {
    width += textWidth(flat(span.text));
  }
  return width;
}

QList<Line> wrap(const Line& line, int width) {
  width = std::max(1, width);
  QList<Line> lines;
  Line current;
  int used = 0;
  bool afterSoftBreak = false;

  auto add = [&](const QString& text, const Style& style) {
    if (!current.isEmpty() && current.last().style == style) {
      current.last().text += text;
    } else {
      current.append(Span{text, style});
    }
    used += textWidth(text);
  };
  auto newLine = [&](bool soft) {
    lines.append(trimEnd(current));
    current.clear();
    used = 0;
    afterSoftBreak = soft;
  };

  for (const Span& span : line) {
    const QStringList parts = sanitize(span.text).split('\n');
    for (int i = 0; i < parts.size(); i++) {
      if (i > 0) {
        newLine(false);
      }
      for (const QString& token : tokens(parts[i])) {
        const int w = textWidth(token);
        // An inverse space is the input cursor: it's content, not a place to break.
        if (token.startsWith(' ') && !span.style.inverse) {
          if (afterSoftBreak && used == 0) {
            continue;
          }
          if (used + w <= width) {
            add(token, span.style);
          } else {
            newLine(true);
          }
          continue;
        }
        if (used + w > width && used > 0 && w <= width) {
          newLine(true);
        }
        if (used + w <= width) {
          add(token, span.style);
        } else {
          for (const QString& grapheme : graphemes(token)) {
            if (used + graphemeWidth(grapheme) > width && used > 0) {
              newLine(true);
            }
            add(grapheme, span.style);
          }
        }
        afterSoftBreak = false;
      }
    }
  }
  lines.append(trimEnd(current));
  return lines;
}

QList<Line> prefixed(const Line& first, const Line& rest, const Line& content, int width) {
  const int indent = std::max(lineWidth(first), lineWidth(rest));
  QList<Line> out;
  const QList<Line> wrapped = wrap(content, width - indent);
  for (int i = 0; i < wrapped.size(); i++) {
    out.append((i == 0 ? first : rest) + wrapped[i]);
  }
  return out;
}

Line truncate(const Line& line, int width) {
  Line clean;
  for (const Span& span : line) {
    clean.append(Span{flat(span.text), span.style});
  }
  if (lineWidth(clean) <= width) {
    return clean;
  }
  Line out;
  int used = 0;
  for (const Span& span : clean) {
    QString text;
    for (const QString& grapheme : graphemes(span.text)) {
      const int w = graphemeWidth(grapheme);
      if (used + w > width - 1) {
        if (!text.isEmpty()) {
          out.append(Span{text, span.style});
        }
        out.append(Span{"…", span.style});
        return out;
      }
      text += grapheme;
      used += w;
    }
    if (!text.isEmpty()) {
      out.append(Span{text, span.style});
    }
  }
  return out;
}

Line fit(const Line& line, int width) {
  Line cut = truncate(line, width);
  const int pad = width - lineWidth(cut);
  if (pad > 0) {
    cut.append(Span{QString(pad, ' '), {}});
  }
  return cut;
}

} // namespace Smith::Tui
