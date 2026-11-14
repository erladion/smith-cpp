#include "core/Think.h"

#include <QRegularExpression>

namespace Smith {

namespace {

const QString OPEN = "<think>";
const QString CLOSE = "</think>";

// Length of the longest suffix of `buffer` that is a proper prefix of one of the tags.
int partialTagSuffix(const QString& buffer, const QStringList& tags) {
  int best = 0;
  for (const QString& tag : tags) {
    for (int n = std::min(int(tag.size()) - 1, int(buffer.size())); n > best; n--) {
      if (buffer.endsWith(tag.left(n))) {
        best = n;
        break;
      }
    }
  }
  return best;
}

} // namespace

QList<Segment> ThinkTagParser::push(const QString& chunk) {
  QString buffer = m_pending + chunk;
  m_pending.clear();
  QList<Segment> out;

  for (;;) {
    const QString& tag = m_inThink ? CLOSE : OPEN;
    const int at = buffer.indexOf(tag);
    // A stray closing tag outside a think block (templates that open the
    // block in the prompt) is dropped rather than shown as text.
    const int stray = m_inThink ? -1 : buffer.indexOf(CLOSE);
    if (stray != -1 && (at == -1 || stray < at)) {
      emitText(out, buffer.left(stray));
      buffer = buffer.mid(stray + CLOSE.size());
      continue;
    }
    if (at == -1) {
      break;
    }
    emitText(out, buffer.left(at));
    buffer = buffer.mid(at + tag.size());
    m_inThink = !m_inThink;
  }

  const int keep = partialTagSuffix(buffer, m_inThink ? QStringList{CLOSE} : QStringList{OPEN, CLOSE});
  emitText(out, buffer.left(buffer.size() - keep));
  m_pending = buffer.right(keep);
  return out;
}

QList<Segment> ThinkTagParser::flush() {
  QList<Segment> out;
  emitText(out, m_pending);
  m_pending.clear();
  return out;
}

void ThinkTagParser::emitText(QList<Segment>& out, const QString& text) const {
  if (!text.isEmpty()) {
    out.append({m_inThink ? BlockKind::Reasoning : BlockKind::Text, text});
  }
}

QString stripThinkTags(const QString& text) {
  static const QRegularExpression blocks(R"(<think>[\s\S]*?</think>)");
  static const QRegularExpression leading(R"(^[\s\S]*?</think>)");
  QString out = text;
  out.remove(blocks);
  out.remove(leading);
  return out.trimmed();
}

} // namespace Smith
