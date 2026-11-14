#pragma once

#include "core/History.h"
#include "core/Tool.h"
#include "frontend/Entry.h"

namespace Smith {

// Rebuilds transcript entries from a saved conversation, so a resumed session shows its history.
QList<Entry> historyToEntries(const Messages& messages, const ToolList& tools);

} // namespace Smith
