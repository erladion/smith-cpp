#pragma once

#include "tui/Keys.h"
#include "tui/Style.h"

#include <functional>
#include <optional>
#include <variant>

namespace Smith::Tui {

struct PickerItem {
  QString value;
  QString label;
  QString hint;
};

// A second thing to do with an item, on ctrl+<key> (e.g. delete it). `check`
// returns the question to confirm with y/n, or why it can't be done.
struct PickerAction {
  QChar key;
  QString hint;
  struct Check {
    QString ask;
    QString refuse;
  };
  std::function<Check(const PickerItem&)> check;
};

struct PickerOptions {
  // Chosen when backspace is pressed with nothing typed (e.g. "go up" in a folder browser).
  QString back;
  // Replaces the default key help line.
  QString help;
  std::optional<PickerAction> action;
};

// A list moved through with ↑/↓ and narrowed down by typing.
class Picker {
public:
  struct Chosen {
    QString value;
    // Whether it was the action (confirmed), not a plain pick.
    bool action = false;
  };

  enum class Outcome { Continue, Cancel, Chose };

  Picker(const QString& title, const QList<PickerItem>& items, const QString& current = {}, const PickerOptions& options = {});

  // Applies a key: Chose (see `chosen()`), Cancel, or Continue.
  Outcome handle(const Key& key);
  const Chosen& chosen() const { return m_chosen; }

  // Content lines (without the surrounding box).
  QList<Line> lines() const;

private:
  QList<PickerItem> visible() const;
  int selected() const;

  QString m_title;
  QList<PickerItem> m_items;
  QString m_current;
  PickerOptions m_options;
  QString m_filter;
  int m_index = 0;
  std::optional<QPair<PickerItem, QString>> m_confirming;
  QString m_refusal;
  Chosen m_chosen;
};

} // namespace Smith::Tui
