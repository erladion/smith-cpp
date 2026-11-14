#pragma once

#include "tui/Rain.h"

#include <QColor>
#include <QQuickPaintedItem>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <memory>

namespace Smith::Desktop {

// Digital rain as a QML item, painted from the TUI's Rain simulation: the
// startup splash (with a title that locks in) and the strip shown while
// the conversation is compacted (endless).
class RainItem : public QQuickPaintedItem {
  Q_OBJECT
  QML_ELEMENT
  Q_PROPERTY(QString title READ title WRITE setTitle NOTIFY settingsChanged)
  Q_PROPERTY(bool endless READ endless WRITE setEndless NOTIFY settingsChanged)
  Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)
  Q_PROPERTY(int glyphSize READ glyphSize WRITE setGlyphSize NOTIFY settingsChanged)
  Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY settingsChanged)
  Q_PROPERTY(QColor headColor MEMBER m_headColor NOTIFY settingsChanged)
  Q_PROPERTY(QColor brightColor MEMBER m_brightColor NOTIFY settingsChanged)
  Q_PROPERTY(QColor color MEMBER m_color NOTIFY settingsChanged)
  Q_PROPERTY(QColor fadedColor MEMBER m_fadedColor NOTIFY settingsChanged)

public:
  explicit RainItem(QQuickItem* parent = nullptr);

  QString title() const { return m_title; }
  void setTitle(const QString& title);
  bool endless() const { return m_endless; }
  void setEndless(bool endless);
  bool running() const { return m_timer.isActive(); }
  void setRunning(bool running);
  int glyphSize() const { return m_glyphSize; }
  void setGlyphSize(int size);
  QString fontFamily() const { return m_fontFamily; }
  void setFontFamily(const QString& family);

  void paint(QPainter* painter) override;

Q_SIGNALS:
  void settingsChanged();
  void runningChanged();
  // The title is complete and has been shown for a moment.
  void finished();

protected:
  void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
  void rebuild();
  void step();
  QSizeF cellSize() const;

  std::unique_ptr<Tui::Rain> m_pRain;
  QTimer m_timer;
  QString m_title;
  bool m_endless = false;
  int m_glyphSize = 16;
  QString m_fontFamily = "monospace";
  QColor m_headColor = QColor("#e8ffe8");
  QColor m_brightColor = QColor("#5af78e");
  QColor m_color = QColor("#00c732");
  QColor m_fadedColor = QColor("#0d5a22");
  bool m_finished = false;
};

} // namespace Smith::Desktop
