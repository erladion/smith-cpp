#pragma once

#include "core/Cancel.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <functional>

namespace Smith::Http {

// Blocking HTTP for the OpenAI-compatible API. Each call runs its own event
// loop in the calling thread, so it works on any thread. Errors throw
// ApiError (the server said no), ConnectionError (it couldn't be reached)
// or AbortError (the token was cancelled).

QJsonValue getJson(const QString& url, const QString& apiKey, int timeoutMs, const CancelPtr& cancel = {});

QJsonObject postJson(const QString& url, const QString& apiKey, const QJsonObject& body, const CancelPtr& cancel = {});

// POSTs with "stream": true and calls `onChunk` with each server-sent event's JSON, until [DONE].
// Returns how many events were skipped because they weren't JSON objects.
int postStream(const QString& url, const QString& apiKey, const QJsonObject& body, const CancelPtr& cancel,
                const std::function<void(const QJsonObject&)>& onChunk);

// Applies proxy settings from the environment (HTTPS_PROXY, HTTP_PROXY, NO_PROXY); call once at startup.
void useSystemNetworkSettings();

} // namespace Smith::Http
