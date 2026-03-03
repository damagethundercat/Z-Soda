#pragma once

#include <string>

namespace zsoda::inference {

struct ModelDownloadRequest {
  std::string model_id;
  std::string asset_relative_path;
  std::string download_url;
  std::string destination_path;
};

enum class ModelDownloadRequestStatus {
  kSkipped,
  kQueued,
  kFailed,
};

ModelDownloadRequestStatus RequestModelDownloadAsync(const ModelDownloadRequest& request,
                                                     std::string* detail);

}  // namespace zsoda::inference
