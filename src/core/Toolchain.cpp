#include "core/Toolchain.h"

#include "core/Process.h"

namespace Smith {

const FastTools& fastTools() {
  static const FastTools found = []() {
    FastTools tools;
    tools.rg = which("rg");
    tools.fd = which("fd");
    if (tools.fd.isEmpty()) {
      tools.fd = which("fdfind");
    }
    tools.jq = which("jq");
    return tools;
  }();
  return found;
}

} // namespace Smith
