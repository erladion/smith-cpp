#pragma once

#include <QRegularExpression>
#include <QString>

namespace Smith {

// Minimal glob matching: `*`, `?`, `**`, `{a,b}` and `[abc]`/`[!abc]`,
// matched against slash-separated paths relative to the search directory.
QRegularExpression globToRegExp(const QString& pattern);

} // namespace Smith
