#include "App.h"

#include <QRegularExpression>

namespace Smith::Ui {

using Tui::Key;
using Tui::KeyName;

// ---- approvals and questions ----

ApprovalDecision App::approveFromWorker(const ApprovalRequest& request) {
  auto pending = std::make_shared<Pending<ApprovalDecision>>();
  QMetaObject::invokeMethod(
      this,
      [this, request, pending]() {
        if (m_closed) {
          pending->resolve(ApprovalDecision::Deny);
          return;
        }
        m_approval = PendingApproval{request, pending};
        requestRender();
      },
      Qt::QueuedConnection);
  // Once the app has closed, nobody will answer.
  return pending->wait([this]() { return m_closing.load(); }, ApprovalDecision::Deny);
}

Answer App::askFromWorker(const Question& question) {
  auto pending = std::make_shared<Pending<Answer>>();
  QMetaObject::invokeMethod(
      this,
      [this, question, pending]() {
        if (m_closed) {
          pending->resolve(Answer{});
          return;
        }
        // Whatever was typed meanwhile (to queue) stays for after; the answer starts empty.
        m_stashed = m_editor.value();
        m_editor.setValue({});
        m_question = PendingQuestion{question, 0, pending};
        requestRender();
      },
      Qt::QueuedConnection);
  return pending->wait([this]() { return m_closing.load(); }, Answer{});
}

void App::decide(ApprovalDecision decision) {
  if (!m_approval) {
    return;
  }
  const auto pending = m_approval->answer;
  m_approval.reset();
  pending->resolve(decision);
}

void App::answer(const Answer& value) {
  if (!m_question) {
    return;
  }
  const auto pending = m_question->answer;
  m_question.reset();
  m_editor.setValue(m_stashed);
  m_stashed.clear();
  pending->resolve(value);
}

void App::onQuestionKey(const Key& key) {
  PendingQuestion& q = *m_question;
  const bool typing = !m_editor.value().isEmpty();
  const int n = q.question.options.size();
  static const QRegularExpression digit("^[1-9]$");
  if (key.name == KeyName::Escape) {
    answer(Answer{});
    if (m_pCancel) {
      m_pCancel->cancel();
    }
  } else if (!typing && n > 0 && (key.name == KeyName::Up || key.name == KeyName::Down)) {
    q.selected = (q.selected + (key.name == KeyName::Up ? n - 1 : 1)) % n;
  } else if (!typing && key.name == KeyName::Text && digit.match(key.text).hasMatch() && key.text.toInt() <= n) {
    answer(Answer{Answer::Kind::Option, key.text.toInt() - 1, {}});
  } else if (!typing && key.name == KeyName::Return && n > 0) {
    answer(Answer{Answer::Kind::Option, q.selected, {}});
  } else if (m_editor.handle(key)) {
    const QString text = m_editor.commit().trimmed();
    if (!text.isEmpty()) {
      answer(Answer{Answer::Kind::Text, 0, text});
    }
  }
}

// ---- keys ----

QList<Suggestion> App::currentSuggestions() const {
  if (m_busy || prompting() || m_picker || m_textPrompt) {
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

  if (m_approval) {
    const QString letter = key.name == KeyName::Text ? key.text.toLower() : QString();
    if (letter == "y") {
      decide(ApprovalDecision::Allow);
    } else if (letter == "a") {
      decide(ApprovalDecision::AllowAlways);
    } else if (letter == "n") {
      decide(ApprovalDecision::Deny);
    } else if (key.name == KeyName::Escape) {
      decide(ApprovalDecision::Deny);
      if (m_pCancel) {
        m_pCancel->cancel();
      }
    }
    return;
  }

  if (m_question) {
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

  if (m_busy) {
    if (key.name == KeyName::Escape) {
      if (m_pCancel) {
        m_pCancel->cancel();
      }
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
