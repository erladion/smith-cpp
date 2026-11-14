#include "tui/Picker.h"

#include <QRegularExpression>

namespace Smith::Tui {

namespace {

constexpr int WINDOW = 10;

} // namespace

Picker::Picker(const QString& title, const QList<PickerItem>& items, const QString& current, const PickerOptions& options)
    : m_title(title), m_items(items), m_current(current), m_options(options) {
  for (int i = 0; i < items.size(); i++) {
    if (items[i].value == current) {
      m_index = i;
      break;
    }
  }
}

QList<PickerItem> Picker::visible() const {
  const QString needle = m_filter.toLower();
  if (needle.isEmpty()) {
    return m_items;
  }
  QList<PickerItem> out;
  for (const PickerItem& item : m_items) {
    if (item.label.toLower().contains(needle)) {
      out.append(item);
    }
  }
  return out;
}

int Picker::selected() const {
  return std::min(m_index, std::max(0, int(visible().size()) - 1));
}

Picker::Outcome Picker::handle(const Key& key) {
  const QList<PickerItem> shown = visible();
  const int at = selected();
  m_refusal.clear();
  if (m_confirming) {
    const PickerItem item = m_confirming->first;
    m_confirming.reset();
    if (key.name == KeyName::Text && key.text.toLower() == "y") {
      m_chosen = Chosen{item.value, true};
      return Outcome::Chose;
    }
    return Outcome::Continue;
  }
  if (m_options.action && key.name == KeyName::Char && key.ctrl && key.text == QString(m_options.action->key)) {
    if (at >= shown.size()) {
      return Outcome::Continue;
    }
    const PickerItem& item = shown[at];
    const PickerAction::Check check = m_options.action->check(item);
    if (!check.ask.isEmpty()) {
      m_confirming = qMakePair(item, check.ask);
    } else {
      m_refusal = check.refuse;
    }
    return Outcome::Continue;
  }
  if (key.name == KeyName::Escape) {
    return Outcome::Cancel;
  }
  if (key.name == KeyName::Return) {
    if (at < shown.size()) {
      m_chosen = Chosen{shown[at].value, false};
      return Outcome::Chose;
    }
    return Outcome::Continue;
  }
  if (key.name == KeyName::Up) {
    m_index = at == 0 ? int(shown.size()) - 1 : at - 1;
  } else if (key.name == KeyName::Down) {
    m_index = at >= shown.size() - 1 ? 0 : at + 1;
  } else if (key.name == KeyName::Backspace) {
    if (m_filter.isEmpty() && !m_options.back.isEmpty()) {
      m_chosen = Chosen{m_options.back, false};
      return Outcome::Chose;
    }
    m_filter.chop(1);
    m_index = 0;
  } else if (key.name == KeyName::Text || key.name == KeyName::Paste) {
    static const QRegularExpression whitespace(R"(\s+)");
    m_filter += QString(key.text).replace(whitespace, " ");
    m_index = 0;
  }
  return Outcome::Continue;
}

QList<Line> Picker::lines() const {
  const QList<PickerItem> shown = visible();
  const int at = selected();
  const int start = std::min(std::max(0, at - WINDOW / 2), std::max(0, int(shown.size()) - WINDOW));

  QString help = m_options.help;
  if (help.isEmpty()) {
    help = "type to filter · ↑↓ move · enter select";
    if (m_options.action) {
      help += QString(" · ctrl+%1 %2").arg(m_options.action->key).arg(m_options.action->hint);
    }
    help += " · esc cancel";
  }
  Line second;
  if (m_confirming) {
    second = {Span{m_confirming->second + " ", Style{.color = Color::Yellow, .bold = true}}, Span{"y", fg(Color::Green)}, Span{"/n", DIM}};
  } else if (!m_refusal.isEmpty()) {
    second = {Span{m_refusal, fg(Color::Yellow)}};
  } else {
    second = {Span{m_filter.isEmpty() ? help : "filter: " + m_filter, DIM}};
  }
  QList<Line> out{Line{Span{m_title, Style{.bold = true}}}, second};
  if (start > 0) {
    out.append(Line{Span{QString("  ↑ %1 more").arg(start), DIM}});
  }
  for (int i = start; i < std::min(int(shown.size()), start + WINDOW); i++) {
    const PickerItem& item = shown[i];
    const bool isSelected = i == at;
    Line line{
        Span{isSelected ? "› " : "  ", isSelected ? fg(Color::Cyan) : Style{}},
        Span{item.label, isSelected ? Style{.color = Color::Cyan, .bold = true} : Style{}},
    };
    if (!m_current.isEmpty() && item.value == m_current) {
      line.append(Span{" (current)", fg(Color::Green)});
    }
    if (!item.hint.isEmpty()) {
      line.append(Span{"  " + item.hint, DIM});
    }
    out.append(line);
  }
  if (shown.isEmpty()) {
    out.append(Line{Span{"  no matches", DIM}});
  }
  const int below = shown.size() - start - WINDOW;
  if (below > 0) {
    out.append(Line{Span{QString("  ↓ %1 more").arg(below), DIM}});
  }
  return out;
}

} // namespace Smith::Tui
