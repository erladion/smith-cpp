#pragma once

#include "core/History.h"

#include <QJsonArray>

#include <exception>
#include <optional>

namespace Smith {

// Rough but serviceable for code and English across common tokenizers. Real
// counts come from the server's usage reports; this only fills the gaps.
constexpr double CHARS_PER_TOKEN = 3.5;

extern const QString SUMMARY_PREFIX;
extern const QString SUMMARY_INSTRUCTIONS;

qint64 estimateTokens(const QString& text);
qint64 estimateTokens(const Message& message);
qint64 estimateTokens(const Messages& messages);
qint64 estimateTokens(const QJsonArray& value);

struct TrimResult {
  Messages messages;
  int count = 0;
};

// Replaces the content of older tool results with a short note.
TrimResult trimOldToolResults(const Messages& messages, int keepRecent = 3, int minChars = 400);

// Where the kept "tail" of the conversation starts: the earliest user or
// assistant message whose suffix fits in `budgetTokens`.
int chooseTail(const Messages& messages, qint64 budgetTokens);

// Renders messages as a plain-text transcript for the summarizer, capped at `maxChars`.
QString renderTranscript(const Messages& messages, int maxChars);

struct Overflow {
  std::optional<int> contextSize;
};

// Recognizes "prompt too long" errors from llama.cpp, vLLM and OpenAI.
std::optional<Overflow> contextOverflow(const std::exception& error);

} // namespace Smith
