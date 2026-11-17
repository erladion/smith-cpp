#pragma once

#include <QString>

namespace Smith {

// Spots a model going round in circles as it streams: the last stretch of
// what it wrote has already come several times before. Small local models do
// this in their reasoning, especially quantized ones with greedy sampling.
class LoopDetector {
public:
  // How long the repeated stretch is, how often it must have come, and how far back to look.
  static constexpr int Stretch = 200;
  static constexpr int Repeats = 4;
  static constexpr int Window = 16000;

  // Adds streamed text; true once the text is repeating itself.
  bool push(const QString& text);

private:
  QString m_text;
  int m_checkedAt = 0;
};

} // namespace Smith
