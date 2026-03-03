#include "inference/ModelAutoDownloader.h"

#include <filesystem>
#include <cstdio>
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

#if defined(_WIN32)
void AppendDownloadLogLine(const std::string& line) {
  char temp_path[MAX_PATH] = {};
  const DWORD written = ::GetTempPathA(MAX_PATH, temp_path);
  if (written == 0 || written >= MAX_PATH) {
    return;
  }

  char log_path[MAX_PATH] = {};
  std::snprintf(log_path, sizeof(log_path), "%s%s", temp_path, "ZSoda_ModelDownload.log");
  FILE* file = std::fopen(log_path, "ab");
  if (file == nullptr) {
    return;
  }

  SYSTEMTIME now = {};
  ::GetLocalTime(&now);
  std::fprintf(file,
               "%04u-%02u-%02u %02u:%02u:%02u.%03u | %s\r\n",
               static_cast<unsigned>(now.wYear),
               static_cast<unsigned>(now.wMonth),
               static_cast<unsigned>(now.wDay),
               static_cast<unsigned>(now.wHour),
               static_cast<unsigned>(now.wMinute),
               static_cast<unsigned>(now.wSecond),
               static_cast<unsigned>(now.wMilliseconds),
               line.c_str());
  std::fclose(file);
}
#endif

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
      const bool downloaded = SUCCEEDED(hr) && IsFilePresent(request.destination_path);
      if (!downloaded) {
        std::error_code remove_ec;
        std::filesystem::remove(std::filesystem::path(request.destination_path), remove_ec);
      }
      AppendDownloadLogLine("model_id=" + request.model_id + ", destination=" + request.destination_path +
                            ", hr=0x" + [] (HRESULT value) {
                              char buffer[16] = {};
                              std::snprintf(buffer, sizeof(buffer), "%08X", static_cast<unsigned>(value));
                              return std::string(buffer);
                            }(hr) +
                            ", downloaded=" + (downloaded ? "true" : "false"));
    } else {
      AppendDownloadLogLine("model_id=" + request.model_id + ", destination=" + request.destination_path +
                            ", create_directories_failed=true");
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
