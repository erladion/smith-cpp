#pragma once

#include <QProcess>
#include <QtGlobal>

namespace Smith {

// A QProcess that starts in a session and process group of its own, so
// killing the group also stops everything it spawned. The child starts with
// default signal handlers, except SIGPIPE, which is ignored.
class SessionProcess : public QProcess {
public:
  explicit SessionProcess(QObject* pParent = nullptr);

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
protected:
  void setupChildProcess() override;
#endif
};

} // namespace Smith
