#include "core/SessionProcess.h"

#include <signal.h>
#include <unistd.h>

namespace Smith {

SessionProcess::SessionProcess(QObject* pParent) : QProcess(pParent) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
  setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession | QProcess::UnixProcessFlag::ResetSignalHandlers |
                           QProcess::UnixProcessFlag::IgnoreSigPipe);
#endif
}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
// Runs in the child between fork and exec: only async-signal-safe calls.
void SessionProcess::setupChildProcess() {
  ::setsid();
  for (int sig = 1; sig < NSIG; sig++) {
    if (sig != SIGKILL && sig != SIGSTOP) {
      ::signal(sig, SIG_DFL);
    }
  }
  ::signal(SIGPIPE, SIG_IGN);
  sigset_t none;
  sigemptyset(&none);
  ::sigprocmask(SIG_SETMASK, &none, nullptr);
}
#endif

} // namespace Smith
