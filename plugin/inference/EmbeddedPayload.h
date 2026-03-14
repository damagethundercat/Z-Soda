#pragma once

#include <string>

namespace zsoda::inference {

struct EmbeddedPayloadInfo {
  bool has_payload = false;
  bool extracted = false;
  std::string asset_root;
  std::string payload_id;
};

[[nodiscard]] EmbeddedPayloadInfo EnsureEmbeddedPayloadAvailable(
    const std::string& module_path,
    std::string* error);

}  // namespace zsoda::inference
