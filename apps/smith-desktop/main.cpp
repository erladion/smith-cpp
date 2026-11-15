#include "frontend/Runtime.h"
#include "core/Dirs.h"
#include "core/Errors.h"
#include "core/Http.h"
#include "core/Session.h"
#include "Backend.h"
#include "Window.h"

#include <QCommandLineParser>
#include <QtGlobal>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QApplication>
#endif
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>

#include <cstdio>

using namespace Smith;

int main(int argc, char** argv) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  QGuiApplication app(argc, argv);
#else
  // Qt 5's folder dialog may be the Qt Widgets one, which needs a QApplication.
  QApplication app(argc, argv);
#endif
  QGuiApplication::setApplicationName("smith");
  QGuiApplication::setApplicationDisplayName("Smith");
  QGuiApplication::setApplicationVersion(SMITH_VERSION);
  QGuiApplication::setDesktopFileName("smith-desktop");
  QGuiApplication::setWindowIcon(QIcon(":/qt/qml/Smith/Desktop/icons/smith.svg"));

  QCommandLineParser parser;
  parser.setApplicationDescription("Smith, the coding agent, in a desktop window. Takes the same server, model, sandbox and session flags as smith.");
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument("folder", "The project to work in (default: the current directory).");
  addLaunchOptions(parser);
  parser.process(app);

  LaunchOptions launch;
  try {
    launch = readLaunchOptions(parser);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", qPrintable(errorMessage(error)));
    return 1;
  }

  Http::useSystemNetworkSettings();
  for (const QString& note : moveOldFolders()) {
    std::fprintf(stderr, "%s\n", qPrintable(note));
  }
  secureSessions();

  Desktop::Backend backend(launch);
  Desktop::Backend::setInstance(&backend);

  QQmlApplicationEngine engine;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, []() { QCoreApplication::exit(1); }, Qt::QueuedConnection);
  Desktop::loadWindow(engine);
#else
  Desktop::loadWindow(engine);
  if (engine.rootObjects().isEmpty()) {
    return 1;
  }
#endif
  backend.start();
  return app.exec();
}
