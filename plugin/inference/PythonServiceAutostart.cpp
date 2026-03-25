#include "inference/PythonServiceAutostart.h"

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "inference/PythonAutostart.h"
#include "inference/RuntimePathResolver.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace zsoda::inference {
namespace {

std::atomic<std::uint64_t> g_temp_file_counter{0U};

using SteadyClock = std::chrono::steady_clock;

std::string ReadEnvOrEmpty(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return {};
  }
  return value;
}

bool IsExistingFile(const std::filesystem::path& path) {
  if (path.empty()) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec) && !ec;
}

std::string ResolvePreloadModelId(const RuntimeOptions& options,
                                  const std::filesystem::path& script_path) {
  std::string model_id = ReadEnvOrEmpty("ZSODA_LOCKED_MODEL_ID");
  if (model_id.empty()) {
    model_id = ReadEnvOrEmpty("ZSODA_HQ_MODEL_ID");
  }
  if (model_id.empty() &&
      script_path.filename().string().find("distill_any_depth_remote_service.py") !=
          std::string::npos) {
    model_id = "distill-any-depth-base";
  }
  return model_id;
}

std::string BuildLocalStatusEndpoint(std::string_view host, int port) {
  std::ostringstream endpoint;
  endpoint << "http://" << host << ":" << port << "/status";
  return endpoint.str();
}

std::filesystem::path BuildAutostartPortFilePath() {
  std::error_code ec;
  const auto temp_root = std::filesystem::temp_directory_path(ec);
  if (ec) {
    return {};
  }
  const std::uint64_t id = g_temp_file_counter.fetch_add(1U, std::memory_order_relaxed);
  return temp_root / ("ZSoda_RemoteServicePort_" + std::to_string(id) + ".txt");
}

bool ParsePortText(std::string_view text, int* port) {
  if (port == nullptr) {
    return false;
  }

  std::string digits;
  digits.reserve(text.size());
  for (const char ch : text) {
    if (std::isdigit(static_cast<unsigned char>(ch))) {
      digits.push_back(ch);
    } else if (!std::isspace(static_cast<unsigned char>(ch))) {
      return false;
    }
  }
  if (digits.empty()) {
    return false;
  }

  errno = 0;
  char* end = nullptr;
  const long parsed = std::strtol(digits.c_str(), &end, 10);
  if (errno != 0 || end == digits.c_str() || (end != nullptr && *end != '\0') || parsed <= 0 ||
      parsed > std::numeric_limits<int>::max()) {
    return false;
  }

  *port = static_cast<int>(parsed);
  return true;
}

bool WaitForAutostartPortFile(const std::filesystem::path& port_file_path,
                              SteadyClock::time_point deadline,
                              int* port,
                              std::string* error) {
  if (port == nullptr) {
    if (error != nullptr) {
      *error = "internal error: autostart port output is null";
    }
    return false;
  }

  while (SteadyClock::now() < deadline) {
    std::ifstream stream(port_file_path, std::ios::binary);
    if (stream.is_open()) {
      std::ostringstream buffer;
      buffer << stream.rdbuf();
      int parsed_port = 0;
      if (ParsePortText(buffer.str(), &parsed_port)) {
        *port = parsed_port;
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  if (error != nullptr) {
    *error = "timed out while waiting for remote inference service to publish its port";
  }
  return false;
}

std::filesystem::path ResolveRemoteServiceScriptPath(const RuntimeOptions& options) {
  if (!options.remote_service_script_path.empty()) {
    const std::filesystem::path explicit_path(options.remote_service_script_path);
    if (IsExistingFile(explicit_path)) {
      return explicit_path;
    }
  }

  std::vector<std::filesystem::path> search_roots;
  if (!options.plugin_directory.empty()) {
    const std::filesystem::path plugin_dir(options.plugin_directory);
    search_roots =
        BuildRuntimeAssetSearchRoots(plugin_dir, std::filesystem::path(options.runtime_asset_root));
  } else if (!options.runtime_asset_root.empty()) {
    search_roots.push_back(std::filesystem::path(options.runtime_asset_root));
  }

  for (const auto& root : search_roots) {
    const std::array<std::filesystem::path, 3> candidates = {
        root / "zsoda_py" / "distill_any_depth_remote_service.py",
        root / "tools" / "distill_any_depth_remote_service.py",
        root / "distill_any_depth_remote_service.py",
    };
    for (const auto& candidate : candidates) {
      if (IsExistingFile(candidate)) {
        return candidate;
      }
    }
  }

  return {};
}

std::filesystem::path DefaultRemoteServiceLogPath() {
  std::error_code temp_error;
  const std::filesystem::path temp_root = std::filesystem::temp_directory_path(temp_error);
  if (!temp_error) {
    return temp_root / "ZSoda_RemoteService.log";
  }
  return std::filesystem::path("ZSoda_RemoteService.log");
}

std::string CollapseWhitespace(std::string_view text) {
  std::string collapsed;
  collapsed.reserve(text.size());
  bool last_was_space = false;
  for (const char ch : text) {
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      if (!last_was_space) {
        collapsed.push_back(' ');
        last_was_space = true;
      }
      continue;
    }
    collapsed.push_back(ch);
    last_was_space = false;
  }
  return collapsed;
}

std::string SummarizePythonProbeOutput(std::string_view output) {
  std::string summary = CollapseWhitespace(output);
  while (!summary.empty() && std::isspace(static_cast<unsigned char>(summary.front())) != 0) {
    summary.erase(summary.begin());
  }
  while (!summary.empty() && std::isspace(static_cast<unsigned char>(summary.back())) != 0) {
    summary.pop_back();
  }
  constexpr std::size_t kMaxSummaryLength = 320U;
  if (summary.size() > kMaxSummaryLength) {
    summary.resize(kMaxSummaryLength);
    summary.append("...");
  }
  return summary;
}

std::string ReadLogTail(const std::filesystem::path& path, std::size_t max_bytes = 2048U) {
  if (path.empty()) {
    return {};
  }
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return {};
  }

  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    return {};
  }

  const std::streamoff bytes_to_read =
      static_cast<std::streamoff>(std::min<std::uintmax_t>(size, max_bytes));
  if (bytes_to_read > 0) {
    stream.seekg(-bytes_to_read, std::ios::end);
  }
  std::string content(static_cast<std::size_t>(bytes_to_read), '\0');
  if (bytes_to_read > 0) {
    stream.read(content.data(), bytes_to_read);
    content.resize(static_cast<std::size_t>(stream.gcount()));
  } else {
    content.clear();
  }
  return content;
}

#if defined(_WIN32)
bool Utf8ToWide(std::string_view utf8, std::wstring* wide, std::string* error) {
  if (wide == nullptr) {
    if (error != nullptr) {
      *error = "internal error: wide output pointer is null";
    }
    return false;
  }
  wide->clear();
  if (utf8.empty()) {
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  const int required = ::MultiByteToWideChar(CP_UTF8,
                                             0,
                                             utf8.data(),
                                             static_cast<int>(utf8.size()),
                                             nullptr,
                                             0);
  if (required <= 0) {
    if (error != nullptr) {
      *error = "MultiByteToWideChar failed: " + std::to_string(::GetLastError());
    }
    return false;
  }

  wide->assign(static_cast<std::size_t>(required), L'\0');
  const int written = ::MultiByteToWideChar(CP_UTF8,
                                            0,
                                            utf8.data(),
                                            static_cast<int>(utf8.size()),
                                            wide->data(),
                                            required);
  if (written != required) {
    if (error != nullptr) {
      *error = "MultiByteToWideChar produced unexpected length";
    }
    return false;
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}
#endif

void FillResult(const std::filesystem::path& script_path,
                const std::filesystem::path& log_path,
                std::string python_command,
                int actual_port,
                PythonServiceLaunchResult* result) {
  if (result == nullptr) {
    return;
  }
  result->script_path = script_path;
  result->log_path = log_path;
  result->python_command = std::move(python_command);
  result->actual_port = actual_port;
}

}  // namespace

bool StartDetachedPythonService(const RuntimeOptions& options,
                                std::string_view host,
                                int requested_port,
                                RemoteServiceHealthCheckFn health_check,
                                RemoteTraceFn trace,
                                PythonServiceLaunchResult* result,
                                std::string* error) {
  if (result != nullptr) {
    *result = {};
  }
  if (health_check == nullptr) {
    if (error != nullptr) {
      *error = "internal error: remote service health-check callback is null";
    }
    return false;
  }

  const std::filesystem::path script_path = ResolveRemoteServiceScriptPath(options);
  if (script_path.empty()) {
    if (error != nullptr) {
      *error = "remote service script was not found";
    }
    return false;
  }

  const PythonAutostartSelection python_selection = ResolvePythonAutostartSelection(options);
  const std::string python_command = python_selection.python_path.string();
  const auto python_capability = python_selection.capability;
  const std::string& python_probe_output = python_selection.probe_output;
  if (python_capability == PythonTorchCapability::kUnavailable) {
    if (error != nullptr) {
      *error = "remote inference python runtime is unavailable or missing required packages (python=" +
               python_command + ")" +
               (python_probe_output.empty()
                    ? std::string()
                    : ": " + SummarizePythonProbeOutput(python_probe_output));
    }
    return false;
  }

  if (trace != nullptr) {
    const std::string trace_detail =
        std::string("python=") + python_command + ", script=" + script_path.string();
    trace("service_autostart_python", trace_detail.c_str());
  }

  const int launch_port = requested_port > 0 ? requested_port : 0;
  const std::string preload_model_id = ResolvePreloadModelId(options, script_path);
  const std::filesystem::path log_path = options.remote_service_log_path.empty()
                                             ? DefaultRemoteServiceLogPath()
                                             : std::filesystem::path(options.remote_service_log_path);
  const std::filesystem::path port_file_path =
      launch_port == 0 ? BuildAutostartPortFilePath() : std::filesystem::path();

#if defined(_WIN32)
  std::wstring python_wide;
  if (!Utf8ToWide(python_command, &python_wide, error)) {
    return false;
  }

  std::wstring script_wide;
  if (!Utf8ToWide(script_path.string(), &script_wide, error)) {
    return false;
  }

  std::wstring host_wide;
  if (!Utf8ToWide(std::string(host), &host_wide, error)) {
    return false;
  }

  std::wstring preload_model_wide;
  if (!preload_model_id.empty() && !Utf8ToWide(preload_model_id, &preload_model_wide, error)) {
    return false;
  }

  std::wstring log_path_wide;
  if (!Utf8ToWide(log_path.string(), &log_path_wide, error)) {
    return false;
  }

  std::wstring port_file_path_wide;
  if (!port_file_path.empty() && !Utf8ToWide(port_file_path.string(), &port_file_path_wide, error)) {
    return false;
  }

  std::wstring command_line;
  command_line.reserve(512);
  command_line.push_back(L'"');
  command_line.append(python_wide);
  command_line.append(L"\" \"");
  command_line.append(script_wide);
  command_line.append(L"\" --host \"");
  command_line.append(host_wide);
  command_line.append(L"\" --port ");
  command_line.append(std::to_wstring(launch_port));
  if (!preload_model_wide.empty()) {
    command_line.append(L" --preload-model-id \"");
    command_line.append(preload_model_wide);
    command_line.push_back(L'"');
  }
  if (!port_file_path_wide.empty()) {
    command_line.append(L" --port-file \"");
    command_line.append(port_file_path_wide);
    command_line.push_back(L'"');
  }

  const std::filesystem::path working_directory = script_path.parent_path();
  std::wstring working_directory_wide;
  if (!Utf8ToWide(working_directory.string(), &working_directory_wide, error)) {
    return false;
  }

  SECURITY_ATTRIBUTES security_attributes = {};
  security_attributes.nLength = sizeof(security_attributes);
  security_attributes.bInheritHandle = TRUE;
  HANDLE log_handle = ::CreateFileW(log_path_wide.c_str(),
                                    FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &security_attributes,
                                    OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
  if (log_handle == INVALID_HANDLE_VALUE) {
    if (error != nullptr) {
      *error = "CreateFileW failed for service log (" + std::to_string(::GetLastError()) + ")";
    }
    return false;
  }

  ::SetFilePointer(log_handle, 0, nullptr, FILE_END);
  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  startup_info.hStdOutput = log_handle;
  startup_info.hStdError = log_handle;

  PROCESS_INFORMATION process_info = {};
  std::wstring mutable_command_line = command_line;
  const BOOL create_ok = ::CreateProcessW(nullptr,
                                          mutable_command_line.data(),
                                          nullptr,
                                          nullptr,
                                          TRUE,
                                          CREATE_NO_WINDOW | DETACHED_PROCESS,
                                          nullptr,
                                          working_directory_wide.c_str(),
                                          &startup_info,
                                          &process_info);
  ::CloseHandle(log_handle);
  if (!create_ok) {
    if (error != nullptr) {
      *error = "CreateProcessW failed for remote inference service (" +
               std::to_string(::GetLastError()) + ")";
    }
    return false;
  }

  ::CloseHandle(process_info.hThread);
  ::CloseHandle(process_info.hProcess);
#else
  const std::string normalized_host = std::string(host);

  const auto log_parent = log_path.parent_path();
  if (!log_parent.empty()) {
    std::error_code create_error;
    std::filesystem::create_directories(log_parent, create_error);
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    if (error != nullptr) {
      *error = "fork failed for remote inference service";
    }
    return false;
  }

  if (pid == 0) {
    const std::filesystem::path working_directory = script_path.parent_path();
    if (!working_directory.empty()) {
      (void)::chdir(working_directory.c_str());
    }

    (void)::setsid();

    const int stdin_handle = ::open("/dev/null", O_RDONLY);
    if (stdin_handle >= 0) {
      (void)::dup2(stdin_handle, STDIN_FILENO);
      if (stdin_handle > STDERR_FILENO) {
        (void)::close(stdin_handle);
      }
    }

    const int log_handle = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_handle >= 0) {
      (void)::dup2(log_handle, STDOUT_FILENO);
      (void)::dup2(log_handle, STDERR_FILENO);
      if (log_handle > STDERR_FILENO) {
        (void)::close(log_handle);
      }
    }

    std::vector<std::string> args_storage;
    args_storage.reserve(preload_model_id.empty() ? 6U : 8U);
    args_storage.push_back(python_command);
    args_storage.push_back(script_path.string());
    args_storage.push_back("--host");
    args_storage.push_back(normalized_host);
    args_storage.push_back("--port");
    args_storage.push_back(std::to_string(launch_port));
    if (!preload_model_id.empty()) {
      args_storage.push_back("--preload-model-id");
      args_storage.push_back(preload_model_id);
    }
    if (!port_file_path.empty()) {
      args_storage.push_back("--port-file");
      args_storage.push_back(port_file_path.string());
    }

    std::vector<char*> argv;
    argv.reserve(args_storage.size() + 1U);
    for (auto& value : args_storage) {
      argv.push_back(value.data());
    }
    argv.push_back(nullptr);
    ::execvp(python_command.c_str(), argv.data());
    std::fprintf(stderr, "execvp failed for remote inference service\n");
    _exit(127);
  }
#endif

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  int resolved_port = launch_port;
  if (resolved_port <= 0) {
    if (!WaitForAutostartPortFile(port_file_path, deadline, &resolved_port, error)) {
      const std::string log_tail = SummarizePythonProbeOutput(ReadLogTail(log_path));
      if (error != nullptr && !log_tail.empty()) {
        *error += " | service_log=" + log_tail;
      }
      std::error_code remove_error;
      std::filesystem::remove(port_file_path, remove_error);
      return false;
    }
  }

  const std::string status_endpoint = BuildLocalStatusEndpoint(host, resolved_port);
  std::string status_payload;
  std::string status_error;
  while (std::chrono::steady_clock::now() < deadline) {
    if (health_check(status_endpoint, 2000, &status_payload, &status_error)) {
      if (error != nullptr) {
        error->clear();
      }
      std::error_code remove_error;
      std::filesystem::remove(port_file_path, remove_error);
      FillResult(script_path, log_path, python_command, resolved_port, result);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  if (error != nullptr) {
    *error = "remote inference service did not become healthy at " + std::string(status_endpoint) +
             " (python=" + python_command + ", log=" + log_path.string() + ")" +
             (status_error.empty() ? std::string() : ": " + status_error);
    const std::string log_tail = SummarizePythonProbeOutput(ReadLogTail(log_path));
    if (!log_tail.empty()) {
      *error += " | service_log=" + log_tail;
    }
  }
  std::error_code remove_error;
  std::filesystem::remove(port_file_path, remove_error);
  return false;
}

}  // namespace zsoda::inference
