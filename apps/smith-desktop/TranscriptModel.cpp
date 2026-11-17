#include "TranscriptModel.h"

#include "core/Diff.h"
#include "MarkdownHtml.h"

#include <QRegularExpression>

namespace Smith::Desktop {

namespace {

QString levelName(NoticeLevel level) {
  switch (level) {
  case NoticeLevel::Info:
    return "info";
  case NoticeLevel::Warn:
    return "warn";
  case NoticeLevel::Error:
    return "error";
  }
  return "info";
}

} // namespace

QVariantList diffRows(const Diff& diff) {
  QVariantList rows;
  for (const DiffLine& line : diff) {
    QString kind;
    int number = 0;
    switch (line.kind) {
    case DiffLine::Kind::Context:
      kind = "context";
      number = line.newLine;
      break;
    case DiffLine::Kind::Add:
      kind = "add";
      number = line.newLine;
      break;
    case DiffLine::Kind::Remove:
      kind = "remove";
      number = line.oldLine;
      break;
    case DiffLine::Kind::Gap:
      kind = "gap";
      break;
    }
    rows.append(QVariantMap{{"kind", kind}, {"text", line.text}, {"number", number}, {"count", line.count}});
  }
  return rows;
}

QString entryKindName(Entry::Kind kind) {
  switch (kind) {
  case Entry::Kind::Banner:
    return "banner";
  case Entry::Kind::User:
    return "user";
  case Entry::Kind::Text:
    return "text";
  case Entry::Kind::Reasoning:
    return "reasoning";
  case Entry::Kind::Tool:
    return "tool";
  case Entry::Kind::Notice:
    return "notice";
  case Entry::Kind::Meta:
    return "meta";
  case Entry::Kind::Quote:
    return "quote";
  case Entry::Kind::Todos:
    return "todos";
  }
  return "notice";
}

TranscriptModel::TranscriptModel(QObject* parent) : QAbstractListModel(parent) {}

void TranscriptModel::setPalette(const Palette& palette) {
  m_palette = palette;
  // Answers are HTML in the palette's colors.
  if (!m_entries.isEmpty()) {
    Q_EMIT dataChanged(index(0), index(int(m_entries.size()) - 1));
  }
}

int TranscriptModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : int(m_entries.size());
}

QVariant TranscriptModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() >= m_entries.size()) {
    return {};
  }
  const Entry& entry = m_entries[index.row()];
  switch (role) {
  case KindRole:
    return entryKindName(entry.kind);
  case TextRole:
    return entry.text;
  case HtmlRole:
    return entry.kind == Entry::Kind::Text ? markdownToHtml(entry.text, m_palette) : plainToHtml(entry.text);
  case LevelRole:
    return levelName(entry.level);
  case NameRole:
    return entry.name;
  case SummaryRole:
    return entry.summary;
  case OutputRole: {
    QString output = entry.output;
    while (output.endsWith('\n')) {
      output.chop(1);
    }
    return output;
  }
  case OutputLinesRole:
    return entry.output.isEmpty() ? 0 : int(entry.output.trimmed().count('\n') + 1);
  case IsErrorRole:
    return entry.isError;
  case DiffRole:
    return entry.diff ? diffRows(*entry.diff) : QVariantList();
  case DiffStatsRole: {
    if (!entry.diff) {
      return QVariantMap();
    }
    const DiffStats stats = diffStats(*entry.diff);
    return QVariantMap{{"added", stats.added}, {"removed", stats.removed}};
  }
  case TodosRole: {
    QVariantList todos;
    for (const Todo& todo : entry.todos) {
      todos.append(QVariantMap{{"content", todo.content}, {"status", todoStatusName(todo.status)}});
    }
    return todos;
  }
  case BannerRole:
    return QVariantMap{
        {"model", entry.model},
        {"server", entry.server},
        {"cwd", entry.cwd},
        {"session", entry.session},
        {"resumed", entry.resumed},
        {"sandboxActive", entry.sandboxActive},
        {"sandboxDescription", entry.sandboxDescription},
        {"instructions", entry.instructions},
        {"skills", entry.skills},
        {"tools", entry.tools},
        {"greeting", entry.greeting},
    };
  case LiveRole:
    return m_live && index.row() == m_entries.size() - 1;
  case FullDiffRole:
    return entry.fullDiff;
  default:
    return {};
  }
}

QHash<int, QByteArray> TranscriptModel::roleNames() const {
  return {
      {KindRole, "kind"},           {TextRole, "text"},       {HtmlRole, "html"},           {LevelRole, "level"},
      {NameRole, "name"},           {SummaryRole, "summary"}, {OutputRole, "output"},       {OutputLinesRole, "outputLines"},
      {IsErrorRole, "isError"},     {DiffRole, "diff"},       {DiffStatsRole, "diffStats"}, {TodosRole, "todos"},
      {BannerRole, "banner"},       {LiveRole, "live"},       {FullDiffRole, "fullDiff"},
  };
}

void TranscriptModel::append(const Entry& entry) {
  finishLive();
  beginInsertRows({}, m_entries.size(), m_entries.size());
  m_entries.append(entry);
  endInsertRows();
}

void TranscriptModel::clear() {
  beginResetModel();
  m_entries.clear();
  m_live = false;
  endResetModel();
}

void TranscriptModel::appendLive(BlockKind kind, const QString& text) {
  const Entry::Kind entryKind = kind == BlockKind::Text ? Entry::Kind::Text : Entry::Kind::Reasoning;
  if (m_live && m_entries.last().kind != entryKind) {
    finishLive();
  }
  if (!m_live) {
    static const QRegularExpression leading(R"(^\s+)");
    const QString start = QString(text).remove(leading);
    if (start.isEmpty()) {
      return;
    }
    beginInsertRows({}, m_entries.size(), m_entries.size());
    m_entries.append(Entry::simple(entryKind, start));
    m_live = true;
    endInsertRows();
    return;
  }
  m_entries.last().text += text;
  const QModelIndex last = index(m_entries.size() - 1);
  Q_EMIT dataChanged(last, last, {TextRole, HtmlRole});
}

void TranscriptModel::finishLive() {
  if (!m_live) {
    return;
  }
  m_live = false;
  const int row = m_entries.size() - 1;
  QString& text = m_entries.last().text;
  while (!text.isEmpty() && text.back().isSpace()) {
    text.chop(1);
  }
  if (text.isEmpty()) {
    beginRemoveRows({}, row, row);
    m_entries.removeLast();
    endRemoveRows();
    return;
  }
  Q_EMIT dataChanged(index(row), index(row), {TextRole, HtmlRole, LiveRole});
}

} // namespace Smith::Desktop
