#pragma once

// Helpers shared by the test suites, mirroring what the original tests do
// with node:fs and process.env.

#include "core/Cancel.h"
#include "core/Errors.h"
#include "core/Sandbox.h"
#include "core/Tool.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

#include <sys/stat.h>

#include <functional>
#include <optional>

namespace TestUtil {

// A folder that is removed with everything in it when this goes out of scope.
class TempDir {
public:
  explicit TempDir(const QString& base = QDir::tempPath(), const QString& prefix = "smith-test-") {
    QByteArray pattern = QFile::encodeName(base + "/" + prefix + "XXXXXX");
    m_path = QFile::decodeName(::mkdtemp(pattern.data()));
  }
  ~TempDir() { Smith::removeTree(m_path); }

  const QString& path() const { return m_path; }
  QString file(const QString& rel) const { return m_path + "/" + rel; }

private:
  QString m_path;
};

inline void write(const QString& file, const QString& content) {
  QDir().mkpath(QFileInfo(file).path());
  QFile handle(file);
  if (handle.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    handle.write(content.toUtf8());
  }
}

inline QString read(const QString& file) {
  QFile handle(file);
  if (!handle.open(QIODevice::ReadOnly)) {
    return {};
  }
  return QString::fromUtf8(handle.readAll());
}

inline bool exists(const QString& path) {
  return QFileInfo::exists(path) || QFileInfo(path).isSymLink();
}

// Permission bits as octal text, e.g. "600".
inline QString mode(const QString& path) {
  struct stat info;
  if (::stat(QFile::encodeName(path).constData(), &info) != 0) {
    return "missing";
  }
  return QString::number(info.st_mode & 0777, 8);
}

inline void chmod(const QString& path, int mode) {
  ::chmod(QFile::encodeName(path).constData(), mode_t(mode));
}

// Sets (or, with nullopt, removes) an environment variable for the scope.
class EnvGuard {
public:
  EnvGuard(const char* name, const std::optional<QString>& value) : m_name(name) {
    if (qEnvironmentVariableIsSet(name)) {
      m_saved = qEnvironmentVariable(name);
    }
    set(value);
  }
  ~EnvGuard() { set(m_saved); }

private:
  void set(const std::optional<QString>& value) {
    if (value) {
      qputenv(m_name.constData(), value->toUtf8());
    } else {
      qunsetenv(m_name.constData());
    }
  }

  QByteArray m_name;
  std::optional<QString> m_saved;
};

inline bool matches(const QString& text, const QString& pattern) {
  return QRegularExpression(pattern).match(text).hasMatch();
}

// The message of whatever `work` throws; nullopt if it doesn't throw.
inline std::optional<QString> thrown(const std::function<void()>& work) {
  try {
    work();
  } catch (const std::exception& error) {
    return Smith::errorMessage(error);
  }
  return std::nullopt;
}

inline Smith::ToolContext context(const QString& cwd, const std::shared_ptr<Smith::Sandbox>& sandbox = {}) {
  Smith::ToolContext ctx;
  ctx.cwd = cwd;
  ctx.cancel = Smith::makeCancel();
  ctx.sandbox = sandbox;
  return ctx;
}

// Runs a tool after validating its input, as the agent does; throws if the input is invalid.
inline Smith::ToolResult run(const Smith::ToolDef& tool, const QJsonObject& input, const Smith::ToolContext& ctx) {
  if (const auto problems = Smith::validateSchema(input, tool.schema)) {
    throw Smith::SmithError("invalid input: " + *problems);
  }
  return tool.run(input, ctx);
}

inline std::optional<QString> reason(const Smith::ToolDef& tool, const QJsonObject& input, const Smith::ToolContext& ctx) {
  return tool.approvalReason ? tool.approvalReason(input, ctx) : std::nullopt;
}

} // namespace TestUtil

// Asserts that `text` matches the regular expression `pattern`, showing the text when it doesn't.
#define QVERIFY_MATCH(text, pattern) QVERIFY2(TestUtil::matches((text), (pattern)), qPrintable(QString("/%1/ didn't match:\n%2").arg(pattern, text)))
#define QVERIFY_NO_MATCH(text, pattern) QVERIFY2(!TestUtil::matches((text), (pattern)), qPrintable(QString("/%1/ matched:\n%2").arg(pattern, text)))

// Asserts that `work` throws with a message matching `pattern`.
#define QVERIFY_THROWS_MATCH(work, pattern)                                                                                \
  do {                                                                                                                     \
    const std::optional<QString> message_ = TestUtil::thrown([&]() { work; });                                             \
    QVERIFY2(message_.has_value(), "expected an exception");                                                               \
    QVERIFY_MATCH(*message_, pattern);                                                                                     \
  } while (false)
