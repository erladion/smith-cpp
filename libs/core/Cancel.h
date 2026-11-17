#pragma once

#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

namespace Smith {

// Set when the user interrupts; long-running work polls it and stops.
class CancelToken {
public:
  CancelToken() = default;
  // Cancelled with `parent` too, or on its own: to stop one request without stopping the turn.
  explicit CancelToken(std::shared_ptr<const CancelToken> parent) : m_pParent(std::move(parent)) {}

  void cancel() { m_cancelled.store(true); }
  bool isCancelled() const { return m_cancelled.load() || (m_pParent && m_pParent->isCancelled()); }

private:
  std::atomic<bool> m_cancelled{false};
  std::shared_ptr<const CancelToken> m_pParent;
};

using CancelPtr = std::shared_ptr<CancelToken>;

inline CancelPtr makeCancel() {
  return std::make_shared<CancelToken>();
}

inline bool cancelled(const CancelPtr& token) {
  return token && token->isCancelled();
}

// Thrown by blocking work that noticed the token was cancelled.
class AbortError : public std::runtime_error {
public:
  AbortError() : std::runtime_error("Request was aborted.") {}
};

} // namespace Smith
