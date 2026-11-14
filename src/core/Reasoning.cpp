#include "core/Reasoning.h"

namespace Smith {

const QStringList REASONING_LEVELS = {"default", "off", "low", "medium", "high"};

bool isReasoningLevel(const QString& value) {
  return REASONING_LEVELS.contains(value);
}

QString reasoningHint(const QString& level) {
  if (level == "default") {
    return "send nothing; the server and model decide";
  }
  if (level == "off") {
    return "turn thinking off (models with an enable_thinking switch, e.g. Qwen3)";
  }
  if (level == "low") {
    return "brief reasoning";
  }
  if (level == "medium") {
    return "moderate reasoning";
  }
  if (level == "high") {
    return "thorough reasoning, slower";
  }
  return {};
}

QJsonObject reasoningParams(const QString& level) {
  if (level == "default" || !isReasoningLevel(level)) {
    return {};
  }
  if (level == "off") {
    return QJsonObject{{"chat_template_kwargs", QJsonObject{{"enable_thinking", false}}}};
  }
  return QJsonObject{
      {"reasoning_effort", level},
      {"chat_template_kwargs", QJsonObject{{"enable_thinking", true}, {"reasoning_effort", level}}},
  };
}

} // namespace Smith
