#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace zsoda::ae {

inline bool AeDiagnosticsFlagEnabled(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return false;
  }

  return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
         std::strcmp(value, "off") != 0 && std::strcmp(value, "no") != 0;
}

inline bool AeDiagnosticsEnabled() {
  return AeDiagnosticsFlagEnabled("ZSODA_AE_TRACE") ||
         AeDiagnosticsFlagEnabled("ZSODA_AE_DIAGNOSTICS");
}

inline std::filesystem::path ResolveAeDiagnosticsLogPath() {
  const char* override_path = std::getenv("ZSODA_AE_LOG_PATH");
  if (override_path != nullptr && override_path[0] != '\0') {
    return std::filesystem::path(override_path);
  }

#if defined(_WIN32)
  char temp_path[MAX_PATH] = {};
  const DWORD written = ::GetTempPathA(MAX_PATH, temp_path);
  if (written > 0 && written < MAX_PATH) {
    return std::filesystem::path(temp_path) / "ZSoda_AE_Runtime.log";
  }
#elif defined(__APPLE__)
  const char* home = std::getenv("HOME");
  if (home != nullptr && home[0] != '\0') {
    return std::filesystem::path(home) / "Library" / "Logs" / "ZSoda" / "ZSoda_AE_Runtime.log";
  }
#endif

  std::error_code ec;
  const auto temp_dir = std::filesystem::temp_directory_path(ec);
  if (!ec) {
    return temp_dir / "ZSoda_AE_Runtime.log";
  }
  return std::filesystem::path("ZSoda_AE_Runtime.log");
}

inline void AppendDiagnosticsLine(const char* tag, const char* detail) {
  if (!AeDiagnosticsEnabled()) {
    return;
  }

  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);

  const std::filesystem::path log_path = ResolveAeDiagnosticsLogPath();
  std::error_code ec;
  const auto parent = log_path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
  }

  FILE* file = std::fopen(log_path.string().c_str(), "ab");
  if (file == nullptr) {
    return;
  }

  const auto now = std::chrono::system_clock::now();
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm local_tm = {};
#if defined(_WIN32)
  localtime_s(&local_tm, &now_time);
#else
  localtime_r(&now_time, &local_tm);
#endif

  const char* safe_tag = tag != nullptr ? tag : "<null>";
  const char* safe_detail = detail != nullptr ? detail : "<null>";
  std::fprintf(file,
               "%04d-%02d-%02d %02d:%02d:%02d.%03d | %s | %s\n",
               local_tm.tm_year + 1900,
               local_tm.tm_mon + 1,
               local_tm.tm_mday,
               local_tm.tm_hour,
               local_tm.tm_min,
               local_tm.tm_sec,
               static_cast<int>(millis.count()),
               safe_tag,
               safe_detail);
  std::fclose(file);
}

inline void AppendDiagnosticsTrace(const char* category,
                                   const char* stage,
                                   const char* detail = nullptr) {
  const auto tid = static_cast<unsigned long long>(
      std::hash<std::thread::id>{}(std::this_thread::get_id()));
  const std::string message =
      "tid=" + std::to_string(tid) + ", stage=" + (stage != nullptr ? stage : "<null>") +
      ", detail=" + ((detail != nullptr && detail[0] != '\0') ? detail : "<none>");
  AppendDiagnosticsLine(category, message.c_str());
}

}  // namespace zsoda::ae
