#pragma once

class QQmlApplicationEngine;

namespace Smith::Desktop {

// Loads the main window (Main.qml) into `engine`. Call after
// Backend::setInstance; the window is the engine's first root object, and
// there is none if loading failed.
void loadWindow(QQmlApplicationEngine& engine);

} // namespace Smith::Desktop
