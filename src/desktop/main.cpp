#include "app/Runtime.h"
#include "core/Dirs.h"
#include "core/Errors.h"
#include "core/Http.h"
#include "core/Session.h"
#include "desktop/Backend.h"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

#include <cstdio>

using namespace Smith;

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
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

  // Controls are styled from scratch; Basic is the style meant for that.
  QQuickStyle::setStyle("Basic");
  Desktop::Backend backend(launch);
  Desktop::Backend::setInstance(&backend);

  QQmlApplicationEngine engine;
  QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, []() { QCoreApplication::exit(1); }, Qt::QueuedConnection);
  engine.loadFromModule("Smith.Desktop", "Main");
  backend.start();
  return app.exec();
}
