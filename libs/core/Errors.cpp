#include "core/Errors.h"

#include <QRegularExpression>

namespace Smith {

namespace {

QString hintFor(const QString& code) {
  if (code.isEmpty()) {
    return {};
  }
  if (code == "CERT") {
    return "The server's certificate isn't trusted. The agent trusts the system certificate store, so if it's "
           "signed by an internal CA, add that CA there (RHEL: copy it to /etc/pki/ca-trust/source/anchors/ and "
           "run update-ca-trust), or point SSL_CERT_FILE at a bundle that includes it.";
  }
  if (code == "ECONNREFUSED") {
    return "Nothing is listening at that address and port. Check the host, port and that the server is up.";
  }
  if (code == "ENOTFOUND") {
    return "The host name doesn't resolve. Check its spelling and your DNS.";
  }
  if (code == "ETIMEDOUT" || code == "EHOSTUNREACH") {
    return "The server can't be reached from here. If your network needs a proxy, set HTTPS_PROXY (or HTTP_PROXY); "
           "the agent uses it, and NO_PROXY for exceptions.";
  }
  if (code == "ECONNRESET") {
    return "The connection was dropped. A proxy or firewall in between may be refusing it.";
  }
  return {};
}

} // namespace

QString errorMessage(const std::exception& error) {
  if (const auto* smith = dynamic_cast<const SmithError*>(&error)) {
    return smith->message();
  }
  return QString::fromStdString(error.what());
}

QString describeError(const std::exception& error, const QString& baseUrl) {
  if (const auto* connection = dynamic_cast<const ConnectionError*>(&error)) {
    if (connection->message().isEmpty()) {
      return QString("Could not reach %1. Is the server running?").arg(baseUrl);
    }
    const QString code = connection->code();
    const QString hint = hintFor(code);
    QString text = QString("Could not reach %1: %2").arg(baseUrl, connection->message());
    if (!code.isEmpty() && !connection->message().contains(code) && code != "CERT") {
      text += QString(" (%1)").arg(code);
    }
    text += ".";
    if (!hint.isEmpty()) {
      text += "\n" + hint;
    }
    return text;
  }
  if (const auto* api = dynamic_cast<const ApiError*>(&error)) {
    const QString status = api->status() ? QString("HTTP %1: ").arg(api->status()) : QString();
    return QString("Server error. %1%2").arg(status, api->message());
  }
  return errorMessage(error);
}

} // namespace Smith
