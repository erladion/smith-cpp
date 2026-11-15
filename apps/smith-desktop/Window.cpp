#include "Window.h"

#include "Backend.h"
#include "RainItem.h"

#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QUrl>
#include <QtGlobal>

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
// The resources CMake lists for Qt 5 (Qt 6 makes its own with the QML module).
static void initDesktopResources() {
  Q_INIT_RESOURCE(smith_desktop);
}
#endif

namespace Smith::Desktop {

void loadWindow(QQmlApplicationEngine& engine) {
  // Controls are styled from scratch; this is the style meant for that.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  QQuickStyle::setStyle("Basic");
  engine.loadFromModule("Smith.Desktop", "Main");
#else
  QQuickStyle::setStyle("Default");
  initDesktopResources();
  // Qt 6 registers these from the QML_ELEMENT macros; Qt 5 is told here.
  qmlRegisterSingletonInstance("Smith.Desktop", 1, 0, "Backend", Backend::create(nullptr, nullptr));
  qmlRegisterType<RainItem>("Smith.Desktop", 1, 0, "RainItem");
  qmlRegisterAnonymousType<TranscriptModel>("Smith.Desktop", 1);
  engine.load(QUrl("qrc:/qt/qml/Smith/Desktop/Main.qml"));
#endif
}

} // namespace Smith::Desktop
