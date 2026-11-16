#include "frontend/FileIndex.h"

#include "core/FileList.h"

#include <QThread>

#include <memory>

namespace Smith {

namespace {

// Files come and go as the agent works; a listing older than this is redone.
constexpr qint64 FRESH_MS = 10000;

} // namespace

FileIndex::FileIndex(QObject* pParent) : QObject(pParent) {}

FileIndex::~FileIndex() {
  if (m_pThread) {
    m_pCancel->cancel();
    m_pThread->wait();
    delete m_pThread;
  }
}

QStringList FileIndex::files(const QString& cwd) {
  if (cwd != m_cwd) {
    m_cwd = cwd;
    m_files.clear();
    m_age.invalidate();
  }
  if (!m_pThread && (!m_age.isValid() || m_age.elapsed() > FRESH_MS)) {
    list();
  }
  return m_files;
}

void FileIndex::list() {
  const QString cwd = m_cwd;
  const CancelPtr cancel = makeCancel();
  auto listed = std::make_shared<QStringList>();
  m_pCancel = cancel;
  m_pThread = QThread::create([cwd, cancel, listed]() {
    try {
      *listed = projectFiles(cwd, cancel);
    } catch (const std::exception&) {
      // Nothing to suggest, then.
    }
  });
  connect(m_pThread, &QThread::finished, this, [this, cwd, listed]() {
    m_pThread->deleteLater();
    m_pThread = nullptr;
    if (cwd != m_cwd) {
      // The project changed meanwhile: list the new one.
      list();
      return;
    }
    m_files = *listed;
    m_age.start();
    Q_EMIT updated();
  });
  m_pThread->start();
}

} // namespace Smith
