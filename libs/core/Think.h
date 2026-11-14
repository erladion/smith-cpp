#pragma once

#include "core/Events.h"

#include <QList>
#include <QString>

namespace Smith {

struct Segment {
  BlockKind kind = BlockKind::Text;
  QString text;
};

// Splits streamed content into text and reasoning for servers that leave
// <think>...</think> inline instead of sending a separate reasoning field.
// Tags may be split across chunks, so a trailing partial tag is held back
// until the next chunk shows whether it really is a tag.
class ThinkTagParser {
public:
  QList<Segment> push(const QString& chunk);
  // Releases any held-back text at the end of the stream.
  QList<Segment> flush();

private:
  void emitText(QList<Segment>& out, const QString& text) const;

  bool m_inThink = false;
  QString m_pending;
};

// Removes complete think blocks from a finished, non-streamed response.
QString stripThinkTags(const QString& text);

} // namespace Smith
