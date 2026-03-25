#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "inference/RuntimeOptions.h"

namespace zsoda::inference {

using RemoteServiceHealthCheckFn = bool (*)(std::string_view endpoint,
                                            int timeout_ms,
                                            std::string* response_payload,
                                            std::string* error);
using RemoteTraceFn = void (*)(const char* stage, const char* detail);

struct PythonServiceLaunchResult {
  std::filesystem::path script_path;
  std::filesystem::path log_path;
  std::string python_command;
  int actual_port = 0;
};

bool StartDetachedPythonService(const RuntimeOptions& options,
                                std::string_view host,
                                int requested_port,
                                RemoteServiceHealthCheckFn health_check,
                                RemoteTraceFn trace,
                                PythonServiceLaunchResult* result,
                                std::string* error);

}  // namespace zsoda::inference
