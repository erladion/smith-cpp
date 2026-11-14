#include "Suggestions.h"

#include "tui/Text.h"

#include <QRegularExpression>

namespace Smith::Ui {

using Tui::Color;
using Tui::DIM;
using Tui::fg;
using Tui::KeyName;
using Tui::Line;
using Tui::Span;
using Tui::Style;

namespace {

constexpr int MAX_SHOWN = 8;

} // namespace

QList<Suggestion> SuggestionList::items(const QString& input, const QString& cwd, const QStringList& endpoints) const {
  if (m_dismissedFor && *m_dismissedFor == input) {
    return {};
  }
  return suggest(input, CompletionContext{cwd, endpoints, models, tasks});
}

SuggestionList::Outcome SuggestionList::handle(const Tui::Key& key, const QList<Suggestion>& items, const QString& input,
                                               const std::function<void(const QString&)>& setInput, QString& toRun) {
  if (items.isEmpty() || key.ctrl || key.meta || key.shift) {
    return Outcome::NotHandled;
  }
  const int at = std::min(m_index, int(items.size()) - 1);
  switch (key.name) {
  case KeyName::Up:
  case KeyName::Down:
    m_index = (at + (key.name == KeyName::Up ? items.size() - 1 : 1)) % items.size();
    return Outcome::Handled;
  case KeyName::Tab: {
    // A command that takes an argument gets a space, so its completions show.
    static const QRegularExpression argumentMark(R"([\[|])");
    const Suggestion& chosen = items[at];
    const bool takesArgs = !chosen.text.contains(' ') && argumentMark.match(chosen.label).hasMatch();
    setInput(chosen.text + (takesArgs ? " " : ""));
    m_index = 0;
    return Outcome::Handled;
  }
  case KeyName::Return:
    toRun = items[at].text;
    return Outcome::Run;
  case KeyName::Escape:
    m_dismissedFor = input;
    return Outcome::Handled;
  default:
    return Outcome::NotHandled;
  }
}

QList<Line> SuggestionList::lines(const QList<Suggestion>& items, int width) const {
  if (items.isEmpty()) {
    return {};
  }
  const int at = std::min(m_index, int(items.size()) - 1);
  const int start = std::min(std::max(0, at - MAX_SHOWN / 2), std::max(0, int(items.size()) - MAX_SHOWN));
  int labelWidth = 0;
  for (const Suggestion& item : items) {
    labelWidth = std::max(labelWidth, int(item.label.size()));
  }
  labelWidth = std::min(28, labelWidth);
  QList<Line> out;
  for (int i = start; i < std::min(int(items.size()), start + MAX_SHOWN); i++) {
    const Suggestion& item = items[i];
    const bool selected = i == at;
    out.append(Tui::truncate(
        Line{
            Span{selected ? " › " : "   ", fg(Color::Cyan)},
            Span{item.label.leftJustified(labelWidth), selected ? Style{.color = Color::Cyan, .bold = true} : Style{}},
            Span{item.hint.isEmpty() ? QString() : "  " + item.hint, DIM},
        },
        width));
  }
  const int more = items.size() - (start + MAX_SHOWN);
  out.append(Line{Span{QString("   %1tab completes · enter runs · esc hides").arg(more > 0 ? QString("%1 more · ").arg(more) : QString()), DIM}});
  return out;
}

} // namespace Smith::Ui
