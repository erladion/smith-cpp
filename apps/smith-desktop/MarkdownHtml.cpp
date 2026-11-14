#include "MarkdownHtml.h"

#include "tui/Markdown.h"

namespace Smith::Desktop {

namespace {

// Wide enough that the TUI renderer never wraps; the QML Text wraps instead.
constexpr int NO_WRAP = 100000;

QString styledSpan(const Tui::Span& span, const Palette& palette) {
  QString text = span.text.toHtmlEscaped();
  if (text.isEmpty()) {
    return {};
  }
  QStringList css;
  const Tui::Style& style = span.style;
  if (style.color == Tui::Color::Cyan) {
    css << "color:" + palette.code.name();
  } else if (style.dim) {
    css << "color:" + palette.textDim.name();
  }
  if (style.bold) {
    css << "font-weight:bold";
  }
  if (style.italic) {
    css << "font-style:italic";
  }
  if (style.underline) {
    css << "text-decoration:underline";
  }
  return css.isEmpty() ? text : QString("<span style=\"%1\">%2</span>").arg(css.join(';'), text);
}

} // namespace

QString markdownToHtml(const QString& markdown, const Palette& palette) {
  const QList<Tui::Line> lines = Tui::markdownLines(markdown, NO_WRAP);
  QString html;
  bool inBlock = false;
  for (const Tui::Line& line : lines) {
    // A code block line starts with the gutter; gather them into one block with a wash behind it.
    // Quotes have the same gutter, but italic text.
    const bool gutter = !line.isEmpty() && line.first().text == "│ " && line.first().style.dim;
    const bool blockLine = gutter && !(line.size() > 1 && line[1].style.italic);
    if (blockLine != inBlock) {
      html += blockLine ? QString("<table width=\"100%\" cellpadding=\"8\" style=\"background-color:%1; margin-top:4px; margin-bottom:4px\"><tr><td>"
                                  "<pre style=\"font-family:'%2'; color:%3; margin:0\">")
                              .arg(palette.codeWash.name(), palette.monoFamily, palette.text.name())
                        : QString("</pre></td></tr></table>");
      inBlock = blockLine;
    }
    if (blockLine) {
      QString code;
      for (int i = 1; i < line.size(); i++) {
        code += line[i].text;
      }
      html += code.toHtmlEscaped() + "\n";
      continue;
    }
    QString row;
    for (const Tui::Span& span : line) {
      row += styledSpan(span, palette);
    }
    html += QString("<p style=\"margin:0; white-space:pre-wrap\">%1</p>").arg(row.isEmpty() ? QString("&nbsp;") : row);
  }
  if (inBlock) {
    html += "</pre></td></tr></table>";
  }
  return html;
}

QString plainToHtml(const QString& text) {
  return QString("<p style=\"margin:0; white-space:pre-wrap\">%1</p>").arg(text.toHtmlEscaped());
}

} // namespace Smith::Desktop
