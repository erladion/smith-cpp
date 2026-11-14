#include "core/Http.h"
#include "compat/Utf8.h"

#include "core/Errors.h"
#include "core/Json.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>
#include <QUrl>

namespace Smith::Http {

namespace {

// The OpenAI SDK's default request timeout.
constexpr int DEFAULT_TIMEOUT_MS = 600000;

// Turns server-sent events into JSON payloads.
class SseParser {
public:
  // Returns false once [DONE] arrived.
  bool feed(const QByteArray& data, const std::function<void(const QJsonObject&)>& onChunk) {
    m_buffer += data;
    for (;;) {
      const int newline = m_buffer.indexOf('\n');
      if (newline < 0) {
        return !m_done;
      }
      QByteArray line = m_buffer.left(newline);
      m_buffer.remove(0, newline + 1);
      if (line.endsWith('\r')) {
        line.chop(1);
      }
      if (line.isEmpty()) {
        dispatch(onChunk);
      } else if (line.startsWith("data:")) {
        QByteArray payload = line.mid(5);
        if (payload.startsWith(' ')) {
          payload.remove(0, 1);
        }
        if (!m_data.isEmpty()) {
          m_data += '\n';
        }
        m_data += payload;
      }
      if (m_done) {
        return false;
      }
    }
  }

  void finish(const std::function<void(const QJsonObject&)>& onChunk) {
    if (!m_buffer.isEmpty()) {
      feed("\n", onChunk);
    }
    dispatch(onChunk);
  }

private:
  void dispatch(const std::function<void(const QJsonObject&)>& onChunk) {
    if (m_data.isEmpty() || m_done) {
      m_data.clear();
      return;
    }
    const QByteArray data = m_data;
    m_data.clear();
    if (data.trimmed() == "[DONE]") {
      m_done = true;
      return;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(data, &error);
    if (!document.isObject()) {
      return;
    }
    const QJsonObject chunk = document.object();
    if (chunk.contains("error")) {
      const QJsonObject body = chunk.value("error").isObject() ? chunk.value("error").toObject() : QJsonObject{{"message", chunk.value("error").toString()}};
      throw ApiError(0, body.value("message").toString("The server reported an error mid-stream."), body, body.value("code").toString());
    }
    onChunk(chunk);
  }

  QByteArray m_buffer;
  QByteArray m_data;
  bool m_done = false;
};

QString connectionCode(QNetworkReply::NetworkError error) {
  switch (error) {
  case QNetworkReply::ConnectionRefusedError:
    return "ECONNREFUSED";
  case QNetworkReply::HostNotFoundError:
    return "ENOTFOUND";
  case QNetworkReply::TimeoutError:
  case QNetworkReply::OperationCanceledError:
    return "ETIMEDOUT";
  case QNetworkReply::RemoteHostClosedError:
    return "ECONNRESET";
  case QNetworkReply::SslHandshakeFailedError:
    return "CERT";
  case QNetworkReply::TemporaryNetworkFailureError:
  case QNetworkReply::NetworkSessionFailedError:
    return "EHOSTUNREACH";
  default:
    return {};
  }
}

[[noreturn]] void throwHttpError(int status, const QByteArray& body) {
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
  QJsonObject error;
  if (document.isObject()) {
    const QJsonValue inner = document.object().value("error");
    error = inner.isObject() ? inner.toObject() : document.object();
    if (inner.isString()) {
      error = QJsonObject{{"message", inner.toString()}};
    }
  }
  QString message = error.value("message").toString();
  if (message.isEmpty()) {
    message = utf8(body).trimmed();
  }
  if (message.isEmpty()) {
    message = QString("status code %1 (no body)").arg(status);
  }
  QString code = error.value("code").isString() ? error.value("code").toString() : QString();
  if (code.isEmpty() && error.value("type").isString()) {
    code = error.value("type").toString();
  }
  throw ApiError(status, message, error, code);
}

bool retryable(int status) {
  return status == 408 || status == 409 || status == 429 || status >= 500;
}

struct Outcome {
  int status = 0;
  QNetworkReply::NetworkError error = QNetworkReply::NoError;
  QString errorString;
  QByteArray body;
  bool aborted = false;
  // Whether any successful data went to the caller (no retry after that).
  bool delivered = false;
};

// One request, with an event loop of its own. `onData` gets the body as it
// arrives when the status is 2xx; otherwise the body is collected for the error.
Outcome request(const QByteArray& verb, const QString& url, const QString& apiKey, const QByteArray& payload, int timeoutMs,
                const CancelPtr& cancel, const std::function<void(const QByteArray&)>& onData) {
  QNetworkAccessManager manager;
  QNetworkRequest req{QUrl(url)};
  req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  req.setRawHeader("Accept", onData ? "text/event-stream, application/json" : "application/json");
  if (!apiKey.isEmpty()) {
    req.setRawHeader("Authorization", "Bearer " + apiKey.toUtf8());
  }
  req.setTransferTimeout(timeoutMs > 0 ? timeoutMs : DEFAULT_TIMEOUT_MS);
  req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
  // Qt 6's default; Qt 5 doesn't follow redirects otherwise.
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

  QNetworkReply* reply = verb == "GET" ? manager.get(req) : manager.sendCustomRequest(req, verb, payload);
  Outcome outcome;
  QEventLoop loop;
  std::exception_ptr failure;

  QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&]() {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray data = reply->readAll();
    if (status >= 200 && status < 300 && onData) {
      try {
        outcome.delivered = true;
        onData(data);
      } catch (...) {
        failure = std::current_exception();
        reply->abort();
      }
    } else {
      outcome.body += data;
    }
  });
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  QTimer poll;
  poll.setInterval(50);
  QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
    if (cancelled(cancel)) {
      outcome.aborted = true;
      reply->abort();
    }
  });
  poll.start();
  if (!reply->isFinished()) {
    loop.exec();
  }
  poll.stop();

  outcome.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const QByteArray rest = reply->readAll();
  if (outcome.status >= 200 && outcome.status < 300 && onData && !failure && !outcome.aborted && !rest.isEmpty()) {
    outcome.delivered = true;
    onData(rest);
  } else {
    outcome.body += rest;
  }
  outcome.error = reply->error();
  outcome.errorString = reply->errorString();
  delete reply;
  if (failure) {
    std::rethrow_exception(failure);
  }
  return outcome;
}

// Runs a request with one retry for connection errors and overloaded servers, like the OpenAI SDK.
void perform(const QByteArray& verb, const QString& url, const QString& apiKey, const QByteArray& payload, int timeoutMs, const CancelPtr& cancel,
             const std::function<void(const QByteArray&)>& onData) {
  for (int attempt = 0;; attempt++) {
    const Outcome outcome = request(verb, url, apiKey, payload, timeoutMs, cancel, onData);
    if (outcome.aborted || cancelled(cancel)) {
      throw AbortError();
    }
    const bool connectionFailed = outcome.status == 0 && outcome.error != QNetworkReply::NoError;
    const bool canRetry = attempt == 0 && !outcome.delivered && (connectionFailed || retryable(outcome.status));
    if (canRetry) {
      // A moment's pause, as the SDK does, while still noticing an interrupt.
      for (int waited = 0; waited < 500 && !cancelled(cancel); waited += 50) {
        QThread::msleep(50);
      }
      continue;
    }
    if (connectionFailed) {
      throw ConnectionError(outcome.errorString, connectionCode(outcome.error));
    }
    if (outcome.status >= 400) {
      throwHttpError(outcome.status, outcome.body);
    }
    if (outcome.status == 0) {
      throw ConnectionError(outcome.errorString.isEmpty() ? QString("no response") : outcome.errorString);
    }
    return;
  }
}

} // namespace

QJsonValue getJson(const QString& url, const QString& apiKey, int timeoutMs, const CancelPtr& cancel) {
  QByteArray body;
  perform("GET", url, apiKey, {}, timeoutMs, cancel, [&](const QByteArray& data) { body += data; });
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(body, &error);
  if (error.error != QJsonParseError::NoError) {
    throw SmithError(QString("%1 returned something other than JSON.").arg(url));
  }
  if (document.isArray()) {
    return document.array();
  }
  return document.object();
}

QJsonObject postJson(const QString& url, const QString& apiKey, const QJsonObject& body, const CancelPtr& cancel) {
  QByteArray response;
  perform("POST", url, apiKey, QJsonDocument(body).toJson(QJsonDocument::Compact), DEFAULT_TIMEOUT_MS, cancel,
          [&](const QByteArray& data) { response += data; });
  const QJsonDocument document = QJsonDocument::fromJson(response);
  if (!document.isObject()) {
    throw SmithError(QString("%1 returned something other than JSON.").arg(url));
  }
  return document.object();
}

void postStream(const QString& url, const QString& apiKey, const QJsonObject& body, const CancelPtr& cancel,
                const std::function<void(const QJsonObject&)>& onChunk) {
  SseParser parser;
  bool plainJson = false;
  QByteArray plain;
  perform("POST", url, apiKey, QJsonDocument(body).toJson(QJsonDocument::Compact), DEFAULT_TIMEOUT_MS, cancel, [&](const QByteArray& data) {
    // A server that ignores "stream" answers with one JSON object.
    if (!plainJson && plain.isEmpty() && data.trimmed().startsWith('{')) {
      plainJson = true;
    }
    if (plainJson) {
      plain += data;
      return;
    }
    parser.feed(data, onChunk);
  });
  if (!plainJson) {
    parser.finish(onChunk);
    return;
  }
  const QJsonDocument document = QJsonDocument::fromJson(plain);
  if (!document.isObject()) {
    throw SmithError("The server's response could not be read.");
  }
  const QJsonObject response = document.object();
  if (response.contains("error")) {
    throwHttpError(0, plain);
  }
  // Turn the whole response into one chunk shaped like a streamed one.
  QJsonArray choices;
  for (const QJsonValue& value : response.value("choices").toArray()) {
    QJsonObject choice = value.toObject();
    choice["delta"] = choice.value("message");
    choices.append(choice);
  }
  QJsonObject chunk = response;
  chunk["choices"] = choices;
  onChunk(chunk);
}

void useSystemNetworkSettings() {
  // Qt reads the lowercase names; curl and most tools take either case.
  for (const char* name : {"http_proxy", "https_proxy", "no_proxy"}) {
    const QByteArray upper = QByteArray(name).toUpper();
    if (qEnvironmentVariableIsEmpty(name) && !qEnvironmentVariableIsEmpty(upper.constData())) {
      qputenv(name, qgetenv(upper.constData()));
    }
  }
  QNetworkProxyFactory::setUseSystemConfiguration(true);
}

} // namespace Smith::Http
