#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <memory>
#include <optional>

// A fake OpenAI-compatible server that answers from a script, the way
// llama-server would: streamed SSE chunks, plain JSON, or an error. It runs
// on a thread of its own, since the agent under test blocks the test's thread.
class FakeServer {
public:
  struct Reply {
    // Streamed: these deltas, then a chunk with `finish`, then usage.
    QList<QJsonObject> deltas;
    QString finish = "stop";
    // Plain: a whole, non-streamed message with this content.
    std::optional<QString> content;
    // An error response with this status and "error" object.
    int status = 0;
    QJsonObject error;
  };

  static Reply stream(const QList<QJsonObject>& deltas, const QString& finish) {
    Reply reply;
    reply.deltas = deltas;
    reply.finish = finish;
    return reply;
  }

  // A delta that goes out as a line that isn't JSON, as a broken proxy might send.
  static QJsonObject garbled() { return QJsonObject{{"__garbled", true}}; }

  static Reply text(const QString& text) { return stream({QJsonObject{{"content", text}}}, "stop"); }

  static Reply plain(const QString& content) {
    Reply reply;
    reply.content = content;
    return reply;
  }

  static Reply failure(int status, const QJsonObject& error) {
    Reply reply;
    reply.status = status;
    reply.error = error;
    return reply;
  }

  // A tool call streamed in fragments: id and name first, arguments split across chunks.
  static QList<QJsonObject> toolCallDeltas(const QString& name, const QJsonObject& args, const QString& id = "call_1") {
    const QString json = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    auto fragment = [](const QJsonObject& function, const QJsonObject& extra = {}) {
      QJsonObject call{{"index", 0}, {"function", function}};
      for (auto it = extra.begin(); it != extra.end(); ++it) {
        call[it.key()] = it.value();
      }
      return QJsonObject{{"tool_calls", QJsonArray{call}}};
    };
    QJsonObject first = fragment(QJsonObject{{"name", name}, {"arguments", ""}}, QJsonObject{{"id", id}, {"type", "function"}});
    first["role"] = "assistant";
    return {first, fragment(QJsonObject{{"arguments", json.left(7)}}), fragment(QJsonObject{{"arguments", json.mid(7)}})};
  }

  // One complete tool call at `index` in a single delta.
  static QJsonObject callDelta(int index, const QString& id, const QString& name, const QJsonObject& args) {
    const QString json = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    return QJsonObject{{"tool_calls", QJsonArray{QJsonObject{{"index", index},
                                                             {"id", id},
                                                             {"type", "function"},
                                                             {"function", QJsonObject{{"name", name}, {"arguments", json}}}}}}};
  }

  FakeServer() {
    m_thread.start();
    m_pServer = new QTcpServer;
    m_pServer->moveToThread(&m_thread);
    QMetaObject::invokeMethod(
        m_pServer,
        [this]() {
          m_pServer->listen(QHostAddress::LocalHost, 0);
          QObject::connect(m_pServer, &QTcpServer::newConnection, m_pServer, [this]() {
            while (QTcpSocket* socket = m_pServer->nextPendingConnection()) {
              auto buffer = std::make_shared<QByteArray>();
              QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
                *buffer += socket->readAll();
                handle(socket, *buffer);
              });
              QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
          });
        },
        Qt::BlockingQueuedConnection);
  }

  ~FakeServer() {
    QMetaObject::invokeMethod(m_pServer, [this]() { delete m_pServer; }, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
  }

  QString baseUrl() const { return QString("http://127.0.0.1:%1/v1").arg(port()); }
  quint16 port() const { return m_pServer->serverPort(); }

  void setScript(const QList<Reply>& script) {
    QMutexLocker lock(&m_mutex);
    m_script = script;
  }

  QList<QJsonObject> requests() const {
    QMutexLocker lock(&m_mutex);
    return m_requests;
  }

  void clearRequests() {
    QMutexLocker lock(&m_mutex);
    m_requests.clear();
  }

  // Model ids served at GET /v1/models; nullopt makes that endpoint return 404.
  void setModels(const std::optional<QStringList>& models) {
    QMutexLocker lock(&m_mutex);
    m_models = models;
  }

private:
  void handle(QTcpSocket* socket, QByteArray& buffer) {
    const int headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
      return;
    }
    const QByteArray head = buffer.left(headerEnd);
    int length = 0;
    for (const QByteArray& line : head.split('\n')) {
      if (line.toLower().startsWith("content-length:")) {
        length = line.mid(15).trimmed().toInt();
      }
    }
    if (buffer.size() < headerEnd + 4 + length) {
      return;
    }
    const QByteArray body = buffer.mid(headerEnd + 4, length);
    buffer.clear();
    const QByteArray requestLine = head.left(head.indexOf('\r'));

    if (requestLine.startsWith("GET")) {
      QMutexLocker lock(&m_mutex);
      if (!requestLine.contains("/v1/models") || !m_models) {
        respond(socket, 404, R"({"error":{"message":"not found"}})");
        return;
      }
      QJsonArray data;
      for (const QString& id : *m_models) {
        data.append(QJsonObject{{"id", id}, {"object", "model"}});
      }
      respond(socket, 200, QJsonDocument(QJsonObject{{"object", "list"}, {"data", data}}).toJson(QJsonDocument::Compact));
      return;
    }

    Reply reply;
    bool exhausted = false;
    {
      QMutexLocker lock(&m_mutex);
      m_requests.append(QJsonDocument::fromJson(body).object());
      if (m_script.isEmpty()) {
        exhausted = true;
      } else {
        reply = m_script.takeFirst();
      }
    }
    if (exhausted) {
      respond(socket, 500, R"({"error":{"message":"fake server: script exhausted"}})");
      return;
    }
    if (reply.status) {
      respond(socket, reply.status, QJsonDocument(QJsonObject{{"error", reply.error}}).toJson(QJsonDocument::Compact));
      return;
    }
    if (reply.content) {
      const QJsonObject message{{"role", "assistant"}, {"content", *reply.content}};
      const QJsonObject response{{"id", "c"},
                                 {"object", "chat.completion"},
                                 {"created", 0},
                                 {"model", "fake"},
                                 {"choices", QJsonArray{QJsonObject{{"index", 0}, {"message", message}, {"finish_reason", "stop"}}}}};
      respond(socket, 200, QJsonDocument(response).toJson(QJsonDocument::Compact));
      return;
    }
    QByteArray stream;
    auto chunk = [&](const QJsonArray& choices, const QJsonObject& extra = {}) {
      QJsonObject object{{"id", "c"}, {"object", "chat.completion.chunk"}, {"created", 0}, {"model", "fake"}, {"choices", choices}};
      for (auto it = extra.begin(); it != extra.end(); ++it) {
        object[it.key()] = it.value();
      }
      stream += "data: " + QJsonDocument(object).toJson(QJsonDocument::Compact) + "\n\n";
    };
    for (const QJsonObject& delta : reply.deltas) {
      if (delta.contains("__garbled")) {
        stream += "data: {\"choices\": [{\"delta\": {\"content\": \"cut o\n\n";
        continue;
      }
      chunk(QJsonArray{QJsonObject{{"index", 0}, {"delta", delta}, {"finish_reason", QJsonValue::Null}}});
    }
    chunk(QJsonArray{QJsonObject{{"index", 0}, {"delta", QJsonObject{}}, {"finish_reason", reply.finish}}});
    chunk(QJsonArray{}, QJsonObject{{"usage", QJsonObject{{"prompt_tokens", 100}, {"completion_tokens", 10}, {"total_tokens", 110}}}});
    stream += "data: [DONE]\n\n";
    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\nContent-Length: " + QByteArray::number(stream.size()) +
                  "\r\n\r\n" + stream);
    socket->flush();
    socket->disconnectFromHost();
  }

  void respond(QTcpSocket* socket, int status, const QByteArray& body) {
    socket->write("HTTP/1.1 " + QByteArray::number(status) + (status == 200 ? " OK" : " Error") +
                  "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    socket->flush();
    socket->disconnectFromHost();
  }

  QThread m_thread;
  QTcpServer* m_pServer = nullptr;
  mutable QMutex m_mutex;
  QList<Reply> m_script;
  QList<QJsonObject> m_requests;
  std::optional<QStringList> m_models = QStringList{"fake"};
};
