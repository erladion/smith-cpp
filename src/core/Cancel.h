#pragma once

#include <atomic>
#include <memory>
#include <stdexcept>

namespace Smith {

// Set when the user interrupts; long-running work polls it and stops.
class CancelToken {
public:
  void cancel() { m_cancelled.store(true); }
  bool isCancelled() const { return m_cancelled.load(); }

private:
  std::atomic<bool> m_cancelled{false};
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
