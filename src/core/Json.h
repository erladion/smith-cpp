#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <optional>

namespace Smith::Json {

// Compact JSON text, as JSON.stringify writes it.
QString stringify(const QJsonValue& value);
QString stringify(const QJsonObject& object);
QString stringify(const QJsonArray& array);

// Parses JSON text; throws std::runtime_error with the parser's message when invalid.
QJsonValue parse(const QString& text);

// The keys of the object at `path` (keys from the root), in the order the text has them;
// QJsonObject sorts its keys. Empty if the text isn't valid JSON or has no such object.
QStringList jsonKeyOrder(const QString& text, const QStringList& path);

std::optional<QString> optString(const QJsonObject& object, const QString& key);
std::optional<int> optInt(const QJsonObject& object, const QString& key);
std::optional<double> optNumber(const QJsonObject& object, const QString& key);
std::optional<bool> optBool(const QJsonObject& object, const QString& key);

} // namespace Smith::Json
