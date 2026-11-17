#include "Backend.h"

#include "core/Dirs.h"
#include "core/Errors.h"
#include "core/Tool.h"
#include "frontend/Duration.h"
#include "frontend/Folders.h"
#include "frontend/Replay.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QUrl>

namespace Smith::Desktop {

// ---- sessions and projects ----

void Backend::refreshSessions() {
  m_sessions.clear();
  if (m_controller.options().sessions.list) {
    const QString current = m_controller.options().sessions.currentId();
    for (const SessionInfo& info : m_controller.options().sessions.list(m_allSessions)) {
      QStringList hint{ago(info.updated), plural(info.turns, "message")};
      m_sessions.append(QVariantMap{
          {"id", info.id},
          {"name", info.name},
          {"hint", hint.join(" · ")},
          {"model", (info.endpoint.isEmpty() ? QString() : info.endpoint + "/") + info.model},
          {"folder", shortPath(info.cwd, 40)},
          {"cwd", info.cwd},
          {"current", info.id == current},
      });
    }
  }
  Q_EMIT sessionsChanged();
}

} // namespace Smith::Desktop
