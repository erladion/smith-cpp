#include "tui/Markdown.h"

#include "tui/Text.h"

#include <QRegularExpression>

namespace Smith::Tui {

QList<Line> markdownLines(const QString& text, int width) {
  static const QRegularExpression fenceRe(R"(^\s*(`{3,}|~{3,}))");
  static const QRegularExpression headingRe(R"(^(#{1,6})\s+(.*)$)");
  static const QRegularExpression ruleRe(R"(^\s*([-*_])(\s*\1){2,}\s*$)");
  static const QRegularExpression quoteRe(R"(^\s*>\s?(.*)$)");
  static const QRegularExpression bulletRe(R"(^(\s*)[-*+]\s+(.*)$)");
  static const QRegularExpression numberRe(R"(^(\s*)(\d+)[.)]\s+(.*)$)");

  QList<Line> out;
  QString fence;
  const Line gutter{Span{"│ ", DIM}};

  for (const QString& raw : text.split('\n')) {
    const QRegularExpressionMatch fenceMatch = fenceRe.match(raw);
    const QString fenceMark = fenceMatch.hasMatch() ? fenceMatch.captured(1) : QString();
    if (!fence.isEmpty()) {
      if (!fenceMark.isEmpty() && fenceMark[0] == fence[0] && fenceMark.size() >= fence.size()) {
        fence.clear();
      } else {
        out.append(prefixed(gutter, gutter, Line{Span{raw, {}}}, width));
      }
      continue;
    }
    if (!fenceMark.isEmpty()) {
      fence = fenceMark;
      continue;
    }
    if (raw.trimmed().isEmpty()) {
      out.append(Line{});
      continue;
    }

    QRegularExpressionMatch m;
    if ((m = headingRe.match(raw)).hasMatch()) {
      const Style style = m.captured(1).size() <= 2 ? Style{.color = Color::Cyan, .bold = true} : Style{.bold = true};
      out.append(wrap(inlineSpans(m.captured(2), style), width));
    } else if (ruleRe.match(raw).hasMatch()) {
      out.append(Line{Span{QString(std::min(width, 40), QChar(0x2500)), DIM}});
    } else if ((m = quoteRe.match(raw)).hasMatch()) {
      const Line bar{Span{"│ ", DIM}};
      out.append(prefixed(bar, bar, inlineSpans(m.captured(1), Style{.dim = true, .italic = true}), width));
    } else if ((m = bulletRe.match(raw)).hasMatch()) {
      const QString indent(m.captured(1).size() / 2 * 2, ' ');
      out.append(prefixed(Line{Span{indent + "• ", {}}}, Line{Span{indent + "  ", {}}}, inlineSpans(m.captured(2)), width));
    } else if ((m = numberRe.match(raw)).hasMatch()) {
      const QString indent(m.captured(1).size() / 2 * 2, ' ');
      const QString marker = indent + m.captured(2) + ". ";
      out.append(prefixed(Line{Span{marker, {}}}, Line{Span{QString(marker.size(), ' '), {}}}, inlineSpans(m.captured(3)), width));
    } else {
      out.append(wrap(inlineSpans(raw), width));
    }
  }
  return out;
}

Line inlineSpans(const QString& text, const Style& base) {
  static const QRegularExpression pattern(
      R"((`+)(.+?)\1|\*\*(.+?)\*\*|__(.+?)__|(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])|(?<!\w)_(?!\s)(.+?)(?<!\s)_(?!\w)|\[([^\]]+)\]\(([^)\s]+)\))");
  Line out;
  int last = 0;
  auto it = pattern.globalMatch(text);
  while (it.hasNext()) {
    const QRegularExpressionMatch m = it.next();
    if (m.capturedStart(0) > last) {
      out.append(Span{text.mid(last, m.capturedStart(0) - last), base});
    }
    if (!m.captured(1).isEmpty()) {
      Style style = base;
      style.color = Color::Cyan;
      out.append(Span{m.captured(2), style});
    } else if (!m.captured(3).isEmpty() || !m.captured(4).isEmpty()) {
      Style style = base;
      style.bold = true;
      out.append(Span{m.captured(3).isEmpty() ? m.captured(4) : m.captured(3), style});
    } else if (!m.captured(5).isEmpty() || !m.captured(6).isEmpty()) {
      Style style = base;
      style.italic = true;
      out.append(Span{m.captured(5).isEmpty() ? m.captured(6) : m.captured(5), style});
    } else if (!m.captured(7).isEmpty()) {
      Style underline = base;
      underline.underline = true;
      out.append(Span{m.captured(7), underline});
      if (m.captured(8) != m.captured(7)) {
        Style dim = base;
        dim.dim = true;
        out.append(Span{" (" + m.captured(8) + ")", dim});
      }
    }
    last = m.capturedEnd(0);
  }
  if (last < text.size()) {
    out.append(Span{text.mid(last), base});
  }
  return out;
}

int safeSplit(const QString& text) {
  static const QRegularExpression fences(R"(^\s*(`{3,}|~{3,}))", QRegularExpression::MultilineOption);
  int at = text.lastIndexOf("\n\n");
  while (at > 0) {
    int count = 0;
    auto it = fences.globalMatch(text.left(at));
    while (it.hasNext()) {
      it.next();
      count++;
    }
    if (count % 2 == 0) {
      return at;
    }
    at = text.lastIndexOf("\n\n", at - 1);
  }
  return -1;
}

} // namespace Smith::Tui
