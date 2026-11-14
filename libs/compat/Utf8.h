#pragma once

#include <QByteArray>
#include <QString>

namespace Smith {

// Bytes as UTF-8 text, NULs included: Qt 5's QString::fromUtf8(QByteArray)
// stops at the first NUL, which hides binary files and cuts git's -z output.
inline QString utf8(const QByteArray& bytes) {
  return QString::fromUtf8(bytes.constData(), bytes.size());
}

} // namespace Smith
