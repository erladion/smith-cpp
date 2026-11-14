#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QStringDecoder>
#else
#include <QTextCodec>
#include <QTextDecoder>

#include <memory>
#endif

namespace Smith {

// Decodes UTF-8 that arrives in pieces: a character split between two pieces
// is held back until the rest arrives.
class Utf8Decoder {
public:
  QString decode(const char* data, qsizetype size) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return m_decoder.decode(QByteArrayView(data, size));
#else
    return m_pDecoder->toUnicode(data, int(size));
#endif
  }

  QString decode(const QByteArray& data) {
    return decode(data.constData(), data.size());
  }

private:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  QStringDecoder m_decoder{QStringDecoder::Utf8};
#else
  std::unique_ptr<QTextDecoder> m_pDecoder{QTextCodec::codecForName("UTF-8")->makeDecoder()};
#endif
};

} // namespace Smith
