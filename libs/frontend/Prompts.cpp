#include "frontend/Prompts.h"

namespace Smith {

Prompts::Prompts(QObject* pParent) : QObject(pParent) {}

ApprovalDecision Prompts::approve(const ApprovalRequest& request) {
  auto pending = std::make_shared<Pending<ApprovalDecision>>();
  QMetaObject::invokeMethod(
      this,
      [this, request, pending]() {
        if (refusing()) {
          pending->resolve(ApprovalDecision::Deny);
          return;
        }
        m_approval = WaitingApproval{request, pending};
        Q_EMIT changed();
      },
      Qt::QueuedConnection);
  return pending->wait([this]() { return m_closing.load(); }, ApprovalDecision::Deny);
}

Answer Prompts::ask(const Question& question) {
  auto pending = std::make_shared<Pending<Answer>>();
  QMetaObject::invokeMethod(
      this,
      [this, question, pending]() {
        if (refusing()) {
          pending->resolve(Answer{});
          return;
        }
        m_question = WaitingQuestion{question, pending};
        Q_EMIT questionAsked();
        Q_EMIT changed();
      },
      Qt::QueuedConnection);
  return pending->wait([this]() { return m_closing.load(); }, Answer{});
}

const ApprovalRequest* Prompts::approval() const {
  return m_approval ? &m_approval->request : nullptr;
}

const Question* Prompts::question() const {
  return m_question ? &m_question->question : nullptr;
}

void Prompts::decide(ApprovalDecision decision) {
  if (!m_approval) {
    return;
  }
  const auto pending = m_approval->answer;
  m_approval.reset();
  Q_EMIT changed();
  pending->resolve(decision);
}

void Prompts::answer(const Answer& answer) {
  if (!m_question) {
    return;
  }
  const auto pending = m_question->answer;
  m_question.reset();
  Q_EMIT changed();
  pending->resolve(answer);
}

void Prompts::dismiss() {
  decide(ApprovalDecision::Deny);
  answer(Answer{});
}

void Prompts::close() {
  m_closed = true;
  m_closing = true;
  if (m_approval) {
    m_approval->answer->resolve(ApprovalDecision::Deny);
  }
  if (m_question) {
    m_question->answer->resolve(Answer{});
  }
}

} // namespace Smith
