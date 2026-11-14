#include "core/Json.h"

#include <stdexcept>

namespace Smith::Json {

QString stringify(const QJsonValue& value) {
  switch (value.type()) {
  case QJsonValue::Object:
    return stringify(value.toObject());
  case QJsonValue::Array:
    return stringify(value.toArray());
  default: {
    // QJsonDocument only takes containers: wrap the scalar and strip the brackets.
    QJsonArray wrapper{value};
    const QString text = stringify(wrapper);
    return text.mid(1, text.size() - 2);
  }
  }
}

QString stringify(const QJsonObject& object) {
  return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString stringify(const QJsonArray& array) {
  return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QJsonValue parse(const QString& text) {
  const QByteArray utf8 = text.toUtf8();
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(utf8, &error);
  if (error.error == QJsonParseError::NoError) {
    if (document.isObject()) {
      return document.object();
    }
    return document.array();
  }
  // QJsonDocument rejects top-level scalars; try again wrapped in an array.
  const QJsonDocument wrapped = QJsonDocument::fromJson("[" + utf8 + "]");
  if (wrapped.isArray() && wrapped.array().size() == 1) {
    return wrapped.array().at(0);
  }
  throw std::runtime_error(QString("%1 at offset %2").arg(error.errorString()).arg(error.offset).toStdString());
}

namespace {

// A minimal JSON reader that only follows the structure, recording one object's keys.
class KeyScanner {
public:
  KeyScanner(const QString& text, const QStringList& target) : m_text(text), m_target(target) {}

  QStringList run() {
    try {
      value({});
    } catch (const std::exception&) {
      return {};
    }
    return m_keys;
  }

private:
  void space() {
    while (m_at < m_text.size() && m_text[m_at].isSpace()) {
      m_at++;
    }
  }

  QChar peek() {
    space();
    if (m_at >= m_text.size()) {
      throw std::runtime_error("end of input");
    }
    return m_text[m_at];
  }

  QString string() {
    QString out;
    m_at++;
    while (m_at < m_text.size() && m_text[m_at] != '"') {
      if (m_text[m_at] == '\\') {
        m_at++;
      }
      if (m_at < m_text.size()) {
        out += m_text[m_at++];
      }
    }
    m_at++;
    return out;
  }

  void value(const QStringList& path) {
    const QChar c = peek();
    if (c == '{') {
      m_at++;
      while (peek() != '}') {
        if (peek() != '"') {
          throw std::runtime_error("expected a key");
        }
        const QString key = string();
        if (path == m_target) {
          m_keys << key;
        }
        if (peek() != ':') {
          throw std::runtime_error("expected :");
        }
        m_at++;
        value(path + QStringList{key});
        if (peek() == ',') {
          m_at++;
        }
      }
      m_at++;
    } else if (c == '[') {
      m_at++;
      while (peek() != ']') {
        value(path + QStringList{"[]"});
        if (peek() == ',') {
          m_at++;
        }
      }
      m_at++;
    } else if (c == '"') {
      string();
    } else {
      while (m_at < m_text.size() && !QString(",}] \t\r\n").contains(m_text[m_at])) {
        m_at++;
      }
    }
  }

  const QString& m_text;
  QStringList m_target;
  int m_at = 0;
  QStringList m_keys;
};

} // namespace

QStringList jsonKeyOrder(const QString& text, const QStringList& path) {
  return KeyScanner(text, path).run();
}

std::optional<QString> optString(const QJsonObject& object, const QString& key) {
  const QJsonValue value = object.value(key);
  if (!value.isString()) {
    return std::nullopt;
  }
  return value.toString();
}

std::optional<int> optInt(const QJsonObject& object, const QString& key) {
  const QJsonValue value = object.value(key);
  if (!value.isDouble()) {
    return std::nullopt;
  }
  return value.toInt();
}

std::optional<double> optNumber(const QJsonObject& object, const QString& key) {
  const QJsonValue value = object.value(key);
  if (!value.isDouble()) {
    return std::nullopt;
  }
  return value.toDouble();
}

std::optional<bool> optBool(const QJsonObject& object, const QString& key) {
  const QJsonValue value = object.value(key);
  if (!value.isBool()) {
    return std::nullopt;
  }
  return value.toBool();
}

} // namespace Smith::Json
