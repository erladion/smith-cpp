#include "frontend/Entry.h"

namespace Smith {

Entry Entry::notice(NoticeLevel level, const QString& text) {
  Entry entry;
  entry.kind = Kind::Notice;
  entry.level = level;
  entry.text = text;
  return entry;
}

Entry Entry::simple(Kind kind, const QString& text) {
  Entry entry;
  entry.kind = kind;
  entry.text = text;
  return entry;
}

} // namespace Smith
