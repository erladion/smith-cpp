#pragma once

#include <QJsonObject>
#include <QString>

#include <exception>
#include <stdexcept>

namespace Smith {

// An error with a message for the user or the model, and an optional
// errno-style code (ENOENT, EISDIR, ...) like Node's fs errors.
class SmithError : public std::runtime_error {
public:
  explicit SmithError(const QString& message, const QString& code = {})
      : std::runtime_error(message.toStdString()), m_message(message), m_code(code) {}

  const QString& message() const { return m_message; }
  const QString& code() const { return m_code; }

private:
  QString m_message;
  QString m_code;
};

// The server answered with an HTTP error.
class ApiError : public SmithError {
public:
  ApiError(int status, const QString& message, const QJsonObject& body, const QString& code = {})
      : SmithError(message, code), m_status(status), m_body(body) {}

  int status() const { return m_status; }
  // The "error" object the server sent, if any.
  const QJsonObject& body() const { return m_body; }

private:
  int m_status;
  QJsonObject m_body;
};

// The server couldn't be reached; `code` says why (ECONNREFUSED, ENOTFOUND, CERT, ...).
class ConnectionError : public SmithError {
public:
  using SmithError::SmithError;
};

// The message of any exception.
QString errorMessage(const std::exception& error);

// Turns API and network errors into readable text for the transcript.
QString describeError(const std::exception& error, const QString& baseUrl);

} // namespace Smith
