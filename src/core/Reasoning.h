#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace Smith {

// "default", "off", "low", "medium", "high".
extern const QStringList REASONING_LEVELS;

bool isReasoningLevel(const QString& value);
QString reasoningHint(const QString& level);

// Extra request fields for a reasoning level. There is no single standard:
// OpenAI and vLLM read the top-level `reasoning_effort`; llama.cpp and vLLM
// pass `chat_template_kwargs` into the model's chat template, where gpt-oss
// reads `reasoning_effort` and Qwen3-style templates read `enable_thinking`.
// "default" sends nothing, for servers that reject unknown fields.
QJsonObject reasoningParams(const QString& level);

} // namespace Smith
