#pragma once

#include "Palette.h"
#include "frontend/Entry.h"

#include <QAbstractListModel>

namespace Smith::Desktop {

// A diff as rows for DiffView: [{kind, text, number, count}].
QVariantList diffRows(const Diff& diff);

// The conversation as the desktop app shows it: the same entries the TUI
// prints, plus streamed text that is still arriving ("live").
class TranscriptModel : public QAbstractListModel {
  Q_OBJECT

public:
  enum Role {
    KindRole = Qt::UserRole + 1,
    TextRole,
    HtmlRole,
    LevelRole,
    NameRole,
    SummaryRole,
    OutputRole,
    OutputLinesRole,
    IsErrorRole,
    DiffRole,
    DiffStatsRole,
    TodosRole,
    BannerRole,
    LiveRole,
  };

  explicit TranscriptModel(QObject* parent = nullptr);

  void setPalette(const Palette& palette);

  int rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

  void append(const Entry& entry);
  void clear();
  // Adds streamed text: to the live entry of the same kind, or a new live entry.
  void appendLive(BlockKind kind, const QString& text);
  // The streamed entry is complete; a blank one is dropped.
  void finishLive();
  bool hasLive() const { return m_live; }

private:
  QList<Entry> m_entries;
  bool m_live = false;
  Palette m_palette;
};

QString entryKindName(Entry::Kind kind);

} // namespace Smith::Desktop
