#pragma once

#include "frontend/Frontend.h"

#include <QObject>

#include <atomic>
#include <memory>
#include <optional>

namespace Smith {

// Approvals and ask_user questions for a frontend. The agent's threads ask
// and wait; the UI thread, where this object lives, shows what is waiting
// and answers it. One of each can wait at a time: the agent asks in turn.
class Prompts : public QObject {
  Q_OBJECT

public:
  explicit Prompts(QObject* pParent = nullptr);

  // ---- from the agent's threads ----

  // Blocks until the user decides; Deny once closed.
  ApprovalDecision approve(const ApprovalRequest& request);
  // Blocks until the user answers; dismissed once closed.
  Answer ask(const Question& question);

  // ---- on the UI thread ----

  // What is waiting for an answer, or null.
  const ApprovalRequest* approval() const;
  const Question* question() const;
  bool waiting() const { return m_approval.has_value() || m_question.has_value(); }

  void decide(ApprovalDecision decision);
  void answer(const Answer& answer);
  // Says no to whatever waits, as when the user interrupts the turn.
  void dismiss();
  // Says no to whatever waits and to everything asked from now on.
  void close();

Q_SIGNALS:
  // A question arrived; `changed` follows.
  void questionAsked();
  // What is waiting changed.
  void changed();

private:
  struct WaitingApproval {
    ApprovalRequest request;
    std::shared_ptr<Pending<ApprovalDecision>> answer;
  };

  struct WaitingQuestion {
    Question question;
    std::shared_ptr<Pending<Answer>> answer;
  };

  std::optional<WaitingApproval> m_approval;
  std::optional<WaitingQuestion> m_question;
  bool m_closed = false;
  // m_closed for the agent's threads: once set, nobody will answer them.
  std::atomic<bool> m_closing{false};
};

} // namespace Smith
