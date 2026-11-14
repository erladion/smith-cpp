#pragma once

#include "compat/Utf8Decoder.h"

#include <QObject>
#include <QSocketNotifier>

#include <memory>
#include <termios.h>

namespace Smith::Tui {

// The real terminal: raw mode, keys as they're typed, its size and resize
// events, and SIGTERM/SIGHUP turned into a signal instead of a sudden exit.
class Terminal : public QObject {
  Q_OBJECT

public:
  explicit Terminal(QObject* parent = nullptr);
  ~Terminal() override;

  static bool isInteractive();

  // Raw mode: keys arrive one by one, unechoed, with ctrl+c as a key.
  void enableRaw();
  // Puts the terminal back as it was.
  void restore();

  int columns() const;
  int rows() const;
  void write(const QString& text);

  // Input is delivered through `input` while reading is on.
  void setReading(bool on);

Q_SIGNALS:
  void input(const QString& data);
  void resized();
  // SIGTERM or SIGHUP arrived; shut down cleanly.
  void terminate();

private:
  void onStdin();
  void onSignal();

  bool m_raw = false;
  termios m_saved{};
  std::unique_ptr<QSocketNotifier> m_pInput;
  std::unique_ptr<QSocketNotifier> m_pSignals;
  Utf8Decoder m_decoder;
};

} // namespace Smith::Tui
