#include "RainItem.h"

#include <QFontMetricsF>
#include <QPainter>

#include <limits>

namespace Smith::Desktop {

RainItem::RainItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
  setAntialiasing(true);
  m_timer.setInterval(40);
  connect(&m_timer, &QTimer::timeout, this, &RainItem::step);
}

void RainItem::setTitle(const QString& title) {
  if (title != m_title) {
    m_title = title;
    rebuild();
    Q_EMIT settingsChanged();
  }
}

void RainItem::setEndless(bool endless) {
  if (endless != m_endless) {
    m_endless = endless;
    rebuild();
    Q_EMIT settingsChanged();
  }
}

void RainItem::setRunning(bool running) {
  if (running == m_timer.isActive()) {
    return;
  }
  if (running) {
    m_timer.start();
  } else {
    m_timer.stop();
  }
  Q_EMIT runningChanged();
}

void RainItem::setGlyphSize(int size) {
  if (size != m_glyphSize && size > 4) {
    m_glyphSize = size;
    rebuild();
    Q_EMIT settingsChanged();
  }
}

void RainItem::setFontFamily(const QString& family) {
  if (family != m_fontFamily) {
    m_fontFamily = family;
    rebuild();
    Q_EMIT settingsChanged();
  }
}

QSizeF RainItem::cellSize() const {
  QFont font(m_fontFamily);
  font.setPixelSize(m_glyphSize);
  const QFontMetricsF metrics(font);
  return {metrics.horizontalAdvance('M'), metrics.height()};
}

void RainItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
  QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
  if (newGeometry.size() != oldGeometry.size()) {
    rebuild();
  }
}

void RainItem::rebuild() {
  const QSizeF cell = cellSize();
  const int cols = int(width() / cell.width());
  const int rows = int(height() / cell.height());
  if (cols < 1 || rows < 1) {
    m_pRain.reset();
    return;
  }
  Tui::Rain::Options options;
  options.title = m_title;
  if (m_endless) {
    options.frames = std::numeric_limits<double>::infinity();
  }
  m_pRain = std::make_unique<Tui::Rain>(cols, rows, options);
  m_finished = false;
  if (m_endless) {
    // Already raining when it appears.
    for (int i = 0; i < 12; i++) {
      m_pRain->step();
    }
  }
  update();
}

void RainItem::step() {
  if (!m_pRain) {
    return;
  }
  m_pRain->step();
  update();
  if (!m_finished && m_pRain->done()) {
    m_finished = true;
    m_timer.stop();
    Q_EMIT runningChanged();
    Q_EMIT finished();
  }
}

void RainItem::paint(QPainter* painter) {
  if (!m_pRain) {
    return;
  }
  QFont font(m_fontFamily);
  font.setPixelSize(m_glyphSize);
  QFont bold = font;
  bold.setBold(true);
  const QSizeF cell = cellSize();
  const QFontMetricsF metrics(font);
  const QList<Tui::Line> lines = m_pRain->lines();
  for (int r = 0; r < lines.size(); r++) {
    int col = 0;
    for (const Tui::Span& span : lines[r]) {
      const Tui::Style& style = span.style;
      QColor color;
      if (style.color == Tui::Color::White) {
        color = m_headColor;
      } else if (style.color == Tui::Color::Green && style.bold) {
        color = m_brightColor;
      } else if (style.color == Tui::Color::Green && style.dim) {
        color = m_fadedColor;
      } else if (style.color == Tui::Color::Green) {
        color = m_color;
      }
      if (!color.isValid()) {
        col += span.text.size();
        continue;
      }
      painter->setFont(style.bold ? bold : font);
      painter->setPen(color);
      for (const QChar ch : span.text) {
        if (ch != ' ') {
          painter->drawText(QPointF(col * cell.width(), r * cell.height() + metrics.ascent()), QString(ch));
        }
        col++;
      }
    }
  }
}

} // namespace Smith::Desktop
