#pragma once

#include <QString>

namespace Smith {

// Faster alternatives to standard tools, when installed.
struct FastTools {
  // ripgrep: faster than grep -r, and respects .gitignore.
  QString rg;
  // fd: faster than find. Debian and Ubuntu install it as fdfind.
  QString fd;
  // jq: for reading JSON in shell commands.
  QString jq;
};

// Looks the tools up once; later calls reuse the result.
const FastTools& fastTools();

} // namespace Smith
