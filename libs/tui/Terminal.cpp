#include "tui/Terminal.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace Smith::Tui {

namespace {

// Signal handlers may only do async-signal-safe things: write the signal
// number to a pipe, which the event loop reads.
int g_signalPipe[2] = {-1, -1};

void onPosixSignal(int number) {
  const char byte = char(number);
  const int saved = errno;
  [[maybe_unused]] const ssize_t written = ::write(g_signalPipe[1], &byte, 1);
  errno = saved;
}

} // namespace

Terminal::Terminal(QObject* parent) : QObject(parent) {
  if (g_signalPipe[0] < 0 && ::pipe(g_signalPipe) == 0) {
    ::fcntl(g_signalPipe[0], F_SETFL, O_NONBLOCK);
    ::fcntl(g_signalPipe[1], F_SETFL, O_NONBLOCK);
  }
  struct sigaction action {};
  action.sa_handler = onPosixSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  ::sigaction(SIGWINCH, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);
  ::sigaction(SIGHUP, &action, nullptr);
  m_pSignals = std::make_unique<QSocketNotifier>(g_signalPipe[0], QSocketNotifier::Read);
  connect(m_pSignals.get(), &QSocketNotifier::activated, this, &Terminal::onSignal);

  m_pInput = std::make_unique<QSocketNotifier>(STDIN_FILENO, QSocketNotifier::Read);
  m_pInput->setEnabled(false);
  connect(m_pInput.get(), &QSocketNotifier::activated, this, &Terminal::onStdin);
}

Terminal::~Terminal() {
  restore();
  ::signal(SIGWINCH, SIG_DFL);
  ::signal(SIGTERM, SIG_DFL);
  ::signal(SIGHUP, SIG_DFL);
}

bool Terminal::isInteractive() {
  return ::isatty(STDIN_FILENO) && ::isatty(STDOUT_FILENO);
}

void Terminal::enableRaw() {
  if (m_raw || ::tcgetattr(STDIN_FILENO, &m_saved) != 0) {
    return;
  }
  termios raw = m_saved;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= CS8;
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
  m_raw = true;
}

void Terminal::restore() {
  if (m_raw) {
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &m_saved);
    m_raw = false;
  }
}

int Terminal::columns() const {
  winsize size{};
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
    return size.ws_col;
  }
  return 80;
}

int Terminal::rows() const {
  winsize size{};
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row > 0) {
    return size.ws_row;
  }
  return 24;
}

void Terminal::write(const QString& text) {
  const QByteArray data = text.toUtf8();
  qsizetype done = 0;
  while (done < data.size()) {
    const ssize_t n = ::write(STDOUT_FILENO, data.constData() + done, size_t(data.size() - done));
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      return;
    }
    done += n;
  }
}

void Terminal::setReading(bool on) {
  m_pInput->setEnabled(on);
}

void Terminal::onStdin() {
  char buffer[4096];
  const ssize_t n = ::read(STDIN_FILENO, buffer, sizeof(buffer));
  if (n <= 0) {
    if (n == 0) {
      // The terminal went away.
      m_pInput->setEnabled(false);
      Q_EMIT terminate();
    }
    return;
  }
  const QString text = m_decoder.decode(QByteArrayView(buffer, n));
  if (!text.isEmpty()) {
    Q_EMIT input(text);
  }
}

void Terminal::onSignal() {
  char bytes[64];
  const ssize_t n = ::read(g_signalPipe[0], bytes, sizeof(bytes));
  bool resize = false;
  bool stop = false;
  for (ssize_t i = 0; i < n; i++) {
    if (bytes[i] == SIGWINCH) {
      resize = true;
    } else {
      stop = true;
    }
  }
  if (resize) {
    Q_EMIT resized();
  }
  if (stop) {
    Q_EMIT terminate();
  }
}

} // namespace Smith::Tui
