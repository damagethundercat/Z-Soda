#pragma once

#include <cstdlib>
#include <cstring>

namespace zsoda::ae {

inline bool AeDiagnosticsEnabled() {
#if defined(_WIN32)
  static const bool enabled = [] {
    const auto enabled_from = [](const char* name) {
      const char* value = std::getenv(name);
      if (value == nullptr || value[0] == '\0') {
        return false;
      }

      return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
             std::strcmp(value, "off") != 0 && std::strcmp(value, "no") != 0;
    };

    return enabled_from("ZSODA_AE_TRACE") || enabled_from("ZSODA_AE_DIAGNOSTICS");
  }();
  return enabled;
#else
  return false;
#endif
}

}  // namespace zsoda::ae
