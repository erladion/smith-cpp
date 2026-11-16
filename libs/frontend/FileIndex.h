#pragma once

#include "core/Cancel.h"

#include <QElapsedTimer>
#include <QObject>
#include <QStringList>

class QThread;

namespace Smith {

// The project's files, for completing @ mentions. Listed on a worker thread,
// so typing never waits for a big repository, and kept for a few seconds.
// Lives on the UI thread.
class FileIndex : public QObject {
  Q_OBJECT

public:
  explicit FileIndex(QObject* pParent = nullptr);
  ~FileIndex() override;

  // The files under `cwd` as last listed: none until the first listing is
  // done. Lists them again when they're stale or `cwd` is another folder.
  QStringList files(const QString& cwd);

Q_SIGNALS:
  // A listing finished: what `files` gives has changed.
  void updated();

private:
  void list();

  QString m_cwd;
  QStringList m_files;
  QElapsedTimer m_age;
  QThread* m_pThread = nullptr;
  CancelPtr m_pCancel;
};

} // namespace Smith
