#include "inference/ModelAutoDownloader.h"

#include <filesystem>
#include <string>
#include <thread>
#include <unordered_set>

#include "core/CompatMutex.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <urlmon.h>
#include <windows.h>
#endif

namespace zsoda::inference {
namespace {

zsoda::core::CompatMutex& DownloadMutex() {
  static zsoda::core::CompatMutex mutex;
  return mutex;
}

std::unordered_set<std::string>& InflightDownloads() {
  static std::unordered_set<std::string> inflight;
  return inflight;
}

void SetDetail(std::string* detail, const std::string& message) {
  if (detail != nullptr) {
    *detail = message;
  }
}

bool IsFilePresent(const std::string& path) {
  std::error_code ec;
  return std::filesystem::is_regular_file(std::filesystem::path(path), ec);
}

}  // namespace

ModelDownloadRequestStatus RequestModelDownloadAsync(const ModelDownloadRequest& request,
                                                     std::string* detail) {
  if (request.model_id.empty()) {
    SetDetail(detail, "auto download skipped: model id is empty");
    return ModelDownloadRequestStatus::kSkipped;
  }
  if (request.download_url.empty()) {
    SetDetail(detail, "auto download skipped: download url is empty");
    return ModelDownloadRequestStatus::kSkipped;
  }
  if (request.destination_path.empty()) {
    SetDetail(detail, "auto download skipped: destination path is empty");
    return ModelDownloadRequestStatus::kSkipped;
  }
  if (IsFilePresent(request.destination_path)) {
    SetDetail(detail, "auto download skipped: model already exists");
    return ModelDownloadRequestStatus::kSkipped;
  }

#if !defined(_WIN32)
  SetDetail(detail, "auto download skipped: runtime downloader is currently Windows-only");
  return ModelDownloadRequestStatus::kSkipped;
#else
  {
    zsoda::core::CompatLockGuard lock(DownloadMutex());
    if (InflightDownloads().find(request.destination_path) != InflightDownloads().end()) {
      SetDetail(detail, "auto download skipped: request already in progress");
      return ModelDownloadRequestStatus::kSkipped;
    }
    InflightDownloads().insert(request.destination_path);
  }

  const auto worker = [request]() {
    const std::string key = request.destination_path;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(request.destination_path).parent_path(),
                                        ec);
    if (!ec) {
      const HRESULT hr = ::URLDownloadToFileA(
          nullptr, request.download_url.c_str(), request.destination_path.c_str(), 0, nullptr);
      (void)hr;
    }

    zsoda::core::CompatLockGuard lock(DownloadMutex());
    InflightDownloads().erase(key);
  };

  try {
    std::thread(worker).detach();
  } catch (...) {
    zsoda::core::CompatLockGuard lock(DownloadMutex());
    InflightDownloads().erase(request.destination_path);
    SetDetail(detail, "auto download failed: unable to start worker thread");
    return ModelDownloadRequestStatus::kFailed;
  }

  SetDetail(detail, "auto download queued");
  return ModelDownloadRequestStatus::kQueued;
#endif
}

}  // namespace zsoda::inference
