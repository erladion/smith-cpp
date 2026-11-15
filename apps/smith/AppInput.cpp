#include "App.h"

#include <QRegularExpression>

namespace Smith::Ui {

using Tui::Key;
using Tui::KeyName;

// ---- approvals and questions ----

void App::answer(const Answer& value) {
  if (!m_prompts.question()) {
    return;
  }
  m_editor.setValue(m_stashed);
  m_stashed.clear();
  m_prompts.answer(value);
}

void App::onQuestionKey(const Key& key) {
  const Question& question = *m_prompts.question();
  const bool typing = !m_editor.value().isEmpty();
  const int n = question.options.size();
  static const QRegularExpression digit("^[1-9]$");
  if (key.name == KeyName::Escape) {
    answer(Answer{});
    m_turns.interrupt();
  } else if (!typing && n > 0 && (key.name == KeyName::Up || key.name == KeyName::Down)) {
    m_questionSelected = (m_questionSelected + (key.name == KeyName::Up ? n - 1 : 1)) % n;
  } else if (!typing && key.name == KeyName::Text && digit.match(key.text).hasMatch() && key.text.toInt() <= n) {
    answer(Answer{Answer::Kind::Option, key.text.toInt() - 1, {}});
  } else if (!typing && key.name == KeyName::Return && n > 0) {
    answer(Answer{Answer::Kind::Option, m_questionSelected, {}});
  } else if (m_editor.handle(key)) {
    const QString text = m_editor.commit().trimmed();
    if (!text.isEmpty()) {
      answer(Answer{Answer::Kind::Text, 0, text});
    }
  }
}

// ---- keys ----

QList<Suggestion> App::currentSuggestions() const {
  if (m_turns.busy() || prompting() || m_picker || m_textPrompt) {
    return {};
  }
  QStringList endpoints;
  for (const Endpoint& endpoint : m_options.endpoints) {
    endpoints << endpoint.name;
  }
  return m_suggestions.items(m_editor.value(), m_project.cwd, endpoints);
}

void App::onKey(const Key& key) {
  if (key.isCtrl('c')) {
    exitApp();
    return;
  }

  if (const ApprovalRequest* approval = m_prompts.approval()) {
    const QString letter = key.name == KeyName::Text ? key.text.toLower() : QString();
    if (letter == "y") {
      m_prompts.decide(ApprovalDecision::Allow);
    } else if (letter == "a" && approval->canAllowAlways) {
      m_prompts.decide(ApprovalDecision::AllowAlways);
    } else if (letter == "n") {
      m_prompts.decide(ApprovalDecision::Deny);
    } else if (key.name == KeyName::Escape) {
      m_prompts.decide(ApprovalDecision::Deny);
      m_turns.interrupt();
    }
    return;
  }

  if (m_prompts.question()) {
    onQuestionKey(key);
    return;
  }

  if (m_picker) {
    const Tui::Picker::Outcome outcome = m_picker->picker->handle(key);
    if (outcome == Tui::Picker::Outcome::Cancel) {
      m_picker.reset();
    } else if (outcome == Tui::Picker::Outcome::Chose) {
      // Cleared first: choosing can open the next picker (the folder browser).
      const Tui::Picker::Chosen chosen = m_picker->picker->chosen();
      const auto onPick = m_picker->onPick;
      const auto onAction = m_picker->onAction;
      m_picker.reset();
      if (chosen.action) {
        if (onAction) {
          onAction(chosen.value);
        }
      } else if (onPick) {
        onPick(chosen.value);
      }
    }
    return;
  }

  if (m_textPrompt) {
    if (key.name == KeyName::Escape) {
      m_editor.setValue(m_textPrompt->stashed);
      m_textPrompt.reset();
      print(Entry::notice(NoticeLevel::Info, "Cancelled."));
    } else if (m_editor.handle(key)) {
      // Not committed: an answer like this doesn't belong in the ↑ history.
      const QString value = m_editor.value();
      const TextPrompt prompt = *m_textPrompt;
      m_textPrompt.reset();
      m_editor.setValue(prompt.stashed);
      prompt.onSubmit(value);
    }
    return;
  }

  if (m_turns.busy()) {
    if (key.name == KeyName::Escape) {
      m_turns.interrupt();
    } else if (agentRunning() && m_editor.handle(key)) {
      queueMessage(m_editor.commit());
    }
    return;
  }

  if (key.isCtrl('d') && m_editor.value().isEmpty()) {
    exitApp();
    return;
  }

  // While command suggestions are showing, arrows pick one, tab completes it and enter runs it.
  QString toRun;
  const SuggestionList::Outcome handled =
      m_suggestions.handle(key, currentSuggestions(), m_editor.value(), [this](const QString& value) { m_editor.setValue(value); }, toRun);
  if (handled == SuggestionList::Outcome::Handled) {
    return;
  }
  if (handled == SuggestionList::Outcome::Run) {
    m_editor.setValue(toRun);
    submit(m_editor.commit());
    return;
  }

  const QString before = m_editor.value();
  if (m_editor.handle(key)) {
    submit(m_editor.commit());
  }
  if (m_editor.value() != before) {
    m_suggestions.reset();
  }
}

} // namespace Smith::Ui
