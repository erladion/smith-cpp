#pragma once

#include <QString>

#include <memory>

namespace Smith {

class Sandbox;

// Resolves a model-supplied path against the working directory and rejects
// anything that lands outside it: `..`, absolute paths elsewhere, and
// symlinks that point out of the root. Throws SmithError.
QString resolveInRoot(const QString& root, const QString& requested);

// Resolves a path for write_file and edit_file: inside the working directory,
// or in the sandbox's private /tmp, which its commands see too. Throws SmithError.
QString resolveForWrite(const QString& root, const QString& requested, const std::shared_ptr<Sandbox>& sandbox);

// Rejects writes into .git: git config and hooks run code outside any sandbox.
void assertWritable(const QString& root, const QString& target);

struct ReadTarget {
  QString path;
  // Outside the project folder: reading it needs the user's approval.
  bool outside = false;
  // Where the file really is, when that differs from `path` (the sandbox's private /tmp).
  QString hostPath;
};

// Resolves a path for reading. Unlike writes, reads may leave the project
// (absolute paths, `..`, `~/`), but never into places hidden from the agent.
ReadTarget resolveForRead(const QString& root, const QString& requested, const std::shared_ptr<Sandbox>& sandbox);

} // namespace Smith
