#include "tui/Rain.h"

#include <QtGlobal>

#include <cmath>

namespace Smith::Tui {

namespace {

// The 16 basic colors, so the terminal's theme picks the shades.
const QString HEAD = "\x1b[1;97m";
const QString BRIGHT = "\x1b[92m";
const QString GREEN = "\x1b[32m";
const QString FADED = "\x1b[2;32m";
const QString TITLE = "\x1b[1;92m";
const QString RESET = "\x1b[0m";

} // namespace

const QStringList& rainGlyphs() {
  static const QStringList glyphs = []() {
    const QString all = QString::fromUtf8("ｦｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗﾘﾙﾚﾛﾜﾝ0123456789:.=*+-<>¦");
    QStringList out;
    for (const QChar c : all) {
      out << QString(c);
    }
    return out;
  }();
  return glyphs;
}

Rain::Rain(int cols, int rows, const Options& options)
    : m_cols(cols), m_rows(rows), m_random(options.seed ? QRandomGenerator(*options.seed) : QRandomGenerator::securelySeeded()),
      m_frames(options.frames), m_holdFrames(options.holdFrames) {
  for (const QChar c : options.title.left(cols)) {
    m_title << QString(c);
  }
  m_titleRow = rows / 2;
  m_titleCol = (cols - int(m_title.size())) / 2;
  m_locked.assign(m_title.size(), false);
  m_glyphs.assign(rows, std::vector<QString>(cols));
  for (auto& row : m_glyphs) {
    for (QString& cell : row) {
      cell = glyph();
    }
  }
  // Drops start at staggered heights above the screen, so they don't arrive as one line.
  m_drops.resize(cols);
  for (auto& drop : m_drops) {
    if (random() < 0.85) {
      drop = makeDrop(rows * 0.9);
    }
  }
}

bool Rain::done() const {
  return m_frame >= m_frames + m_holdFrames;
}

QString Rain::shownTitle() const {
  QString out;
  for (int i = 0; i < m_title.size(); i++) {
    out += m_locked[i] ? m_title[i] : QString(" ");
  }
  return out;
}

void Rain::step() {
  m_frame++;
  const bool spawning = m_frame < m_frames * 0.6;
  const bool revealing = m_frame >= m_frames * 0.3;
  for (int c = 0; c < m_cols; c++) {
    std::optional<Drop>& drop = m_drops[c];
    if (!drop) {
      if (spawning && random() < 0.04) {
        drop = makeDrop(m_rows * 0.3);
      }
      continue;
    }
    const double before = drop->head;
    drop->head += drop->speed;
    // A drop passing through the title row leaves its letter behind.
    const int t = c - m_titleCol;
    if (revealing && t >= 0 && t < m_title.size() && before <= m_titleRow && drop->head >= m_titleRow) {
      m_locked[t] = true;
    }
    if (drop->head - drop->length > m_rows) {
      if (spawning) {
        drop = makeDrop(m_rows * 0.3);
      } else {
        drop.reset();
      }
    }
  }
  if (m_frame >= m_frames) {
    std::fill(m_locked.begin(), m_locked.end(), true);
  }
  // Glyphs flicker.
  const int changes = int(std::ceil(m_rows * m_cols * 0.03));
  for (int i = 0; i < changes; i++) {
    m_glyphs[int(random() * m_rows)][int(random() * m_cols)] = glyph();
  }
}

QString Rain::render() const {
  QString out;
  for (int r = 0; r < m_rows; r++) {
    out += QString("\x1b[%1;1H").arg(r + 1);
    QString current;
    for (int c = 0; c < m_cols; c++) {
      const auto [shade, ch] = cell(r, c);
      QString style;
      switch (shade) {
      case Shade::Head:
        style = HEAD;
        break;
      case Shade::Bright:
        style = BRIGHT;
        break;
      case Shade::Green:
        style = GREEN;
        break;
      case Shade::Faded:
        style = FADED;
        break;
      case Shade::Title:
        style = TITLE;
        break;
      case Shade::None:
        break;
      }
      if (style != current) {
        out += RESET + style;
        current = style;
      }
      out += ch;
    }
    out += RESET;
  }
  return out;
}

QList<Line> Rain::lines() const {
  QList<Line> out;
  for (int r = 0; r < m_rows; r++) {
    Line line;
    std::optional<Shade> previous;
    for (int c = 0; c < m_cols; c++) {
      const auto [shade, ch] = cell(r, c);
      if (previous == shade && !line.isEmpty()) {
        line.last().text += ch;
        continue;
      }
      Style style;
      switch (shade) {
      case Shade::Head:
        style = Style{.color = Color::White, .bold = true};
        break;
      case Shade::Bright:
      case Shade::Title:
        style = Style{.color = Color::Green, .bold = true};
        break;
      case Shade::Green:
        style = fg(Color::Green);
        break;
      case Shade::Faded:
        style = Style{.color = Color::Green, .dim = true};
        break;
      case Shade::None:
        break;
      }
      line.append(Span{ch, style});
      previous = shade;
    }
    out.append(line);
  }
  return out;
}

QPair<Rain::Shade, QString> Rain::cell(int r, int c) const {
  const int t = c - m_titleCol;
  if (r == m_titleRow && t >= 0 && t < m_title.size() && m_locked[t]) {
    return {Shade::Title, m_title[t]};
  }
  const std::optional<Drop>& drop = m_drops[c];
  const double d = drop ? drop->head - r : -1;
  if (!drop || d < 0 || d >= drop->length) {
    return {Shade::None, " "};
  }
  const QString& g = m_glyphs[r][c];
  if (d < 1) {
    return {Shade::Head, g};
  }
  if (d < 3) {
    return {Shade::Bright, g};
  }
  return {d < drop->length * 0.5 ? Shade::Green : Shade::Faded, g};
}

Rain::Drop Rain::makeDrop(double spread) {
  Drop drop;
  // At least a few rows of gap above, so short strips aren't solid green.
  drop.head = -random() * std::max(spread, 4.0);
  drop.speed = m_rows / (10 + random() * 16);
  drop.length = std::max(4, int(std::lround(m_rows * (0.3 + random() * 0.5))));
  return drop;
}

QString Rain::glyph() {
  const QStringList& glyphs = rainGlyphs();
  return glyphs[int(random() * glyphs.size())];
}

double Rain::random() {
  return m_random.generateDouble();
}

QString codeStream(int offset, int width) {
  const QStringList& glyphs = rainGlyphs();
  QString out;
  for (int i = 0; i < width; i++) {
    quint32 h = quint32(offset + i) * 0x9e3779b1u;
    h = (h ^ (h >> 15)) * 0x85ebca6bu;
    h = h ^ (h >> 13);
    out += h % 5 == 0 ? QString(" ") : glyphs[h % glyphs.size()];
  }
  return out;
}

} // namespace Smith::Tui
