#include "core/Paths.h"

#include "core/Dirs.h"
#include "core/Errors.h"
#include "core/Sandbox.h"

#include <QDir>
#include <QFileInfo>

namespace Smith {

namespace {

QString resolve(const QString& root, const QString& requested) {
  return QDir::cleanPath(QDir(root).absoluteFilePath(requested));
}

bool isInside(const QString& root, const QString& target) {
  const QString rel = QDir(root).relativeFilePath(target);
  return !(rel == ".." || rel.startsWith("../") || QDir::isAbsolutePath(rel));
}

void assertInside(const QString& root, const QString& target, const QString& requested) {
  if (!isInside(root, target)) {
    throw SmithError(QString("Path is outside the working directory: %1").arg(requested));
  }
}

} // namespace

QString resolveInRoot(const QString& root, const QString& requested) {
  const QString target = resolve(root, requested);
  assertInside(root, target, requested);

  // Resolve symlinks on the deepest part of the path that exists, so a link
  // inside the root cannot redirect a read or write outside it.
  const QString realRoot = realPath(root).isEmpty() ? root : realPath(root);
  QString existing = target;
  for (;;) {
    const QString real = realPath(existing);
    if (!real.isEmpty()) {
      assertInside(realRoot, real, requested);
      break;
    }
    const QString parent = QFileInfo(existing).path();
    if (parent == existing) {
      break;
    }
    existing = parent;
  }
  return target;
}

void assertWritable(const QString& root, const QString& target) {
  const QString rel = QDir(root).relativeFilePath(target);
  if (rel.section('/', 0, 0) == ".git") {
    throw SmithError("Refusing to modify .git: git runs its config and hooks outside the sandbox. Ask the user to make git changes.");
  }
}

ReadTarget resolveForRead(const QString& root, const QString& requested, const std::shared_ptr<Sandbox>& sandbox) {
  const QString target = resolve(root, expandHome(requested));
  const QString real = realPathAllowingMissing(target);
  const QString realRoot = realPath(root).isEmpty() ? root : realPath(root);
  if (isInside(realRoot, real)) {
    return ReadTarget{target, false, {}};
  }
  const std::optional<SandboxPath> temp = sandbox ? sandbox->sandboxPath(target, root) : std::nullopt;
  if (temp && (temp->host.isEmpty() || temp->root.isEmpty())) {
    return ReadTarget{target, false, {}};
  }
  if (temp) {
    const QString realTemp = realPathAllowingMissing(temp->host);
    const QString realTempRoot = realPath(temp->root).isEmpty() ? temp->root : realPath(temp->root);
    if (isInside(realTempRoot, realTemp)) {
      return ReadTarget{target, false, temp->host};
    }
    if (isHiddenFromAgent(realTemp)) {
      throw SmithError(QString("%1 is off limits: it leads to a place hidden from the agent.").arg(requested));
    }
    return ReadTarget{target, true, temp->host};
  }
  if (isHiddenFromAgent(target) || isHiddenFromAgent(real)) {
    throw SmithError(QString("%1 is off limits: credentials, system files and the agent's own data are hidden from the agent.").arg(requested));
  }
  return ReadTarget{target, true, {}};
}

} // namespace Smith
