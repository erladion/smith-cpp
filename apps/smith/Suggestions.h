#pragma once

#include "tui/Keys.h"
#include "tui/Style.h"
#include "frontend/Commands.h"

#include <functional>
#include <optional>

namespace Smith::Ui {

// The command suggestions under the input box: which one is highlighted,
// whether esc hid them, and the keys that act on them.
class SuggestionList {
public:
  // Suggestions for the input, unless hidden for it.
  QList<Suggestion> items(const QString& input, const QString& cwd, const QStringList& endpoints) const;
  // The input changed: start again from the first suggestion.
  void reset() { m_index = 0; }

  enum class Outcome { NotHandled, Handled, Run };

  // Arrows move the highlight, tab fills in the highlighted suggestion, esc
  // hides them. For Run, `toRun` gets the text to run.
  Outcome handle(const Tui::Key& key, const QList<Suggestion>& items, const QString& input, const std::function<void(const QString&)>& setInput,
                 QString& toRun);

  QList<Tui::Line> lines(const QList<Suggestion>& items, int width) const;

  // Model ids the server listed, for completing /model.
  QStringList models;
  // The repository's task worktrees, for completing /land and /drop.
  QStringList tasks;

private:
  int m_index = 0;
  // The input they were hidden for with esc; they come back once it changes.
  std::optional<QString> m_dismissedFor;
};

} // namespace Smith::Ui
