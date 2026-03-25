#include "inference/PythonAutostart.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "inference/RuntimePathResolver.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace zsoda::inference {
namespace {

std::string ReadEnvOrEmpty(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return {};
  }
  return value;
}

std::string BuildPythonRuntimeProbeScript() {
  return "import torch, PIL, transformers; "
         "from transformers import AutoImageProcessor, AutoModelForDepthEstimation; "
         "print('MPS=1' if getattr(getattr(torch,'backends',None),'mps',None) is not None and "
         "torch.backends.mps.is_available() else ('CUDA=1' if torch.cuda.is_available() else 'CPU=1'))";
}

std::vector<std::filesystem::path> BuildPythonSearchRoots(const RuntimeOptions& options) {
  std::vector<std::filesystem::path> search_roots;
  if (!options.plugin_directory.empty()) {
    const std::filesystem::path plugin_dir(options.plugin_directory);
    search_roots =
        BuildRuntimeAssetSearchRoots(plugin_dir, std::filesystem::path(options.runtime_asset_root));
  } else if (!options.runtime_asset_root.empty()) {
    search_roots.push_back(std::filesystem::path(options.runtime_asset_root));
  }
  return search_roots;
}

#if defined(_WIN32)
bool Utf8ToWide(std::string_view input, std::wstring* output) {
  if (output == nullptr) {
    return false;
  }
  output->clear();
  if (input.empty()) {
    return true;
  }

  const int required = ::MultiByteToWideChar(CP_UTF8,
                                             MB_ERR_INVALID_CHARS,
                                             input.data(),
                                             static_cast<int>(input.size()),
                                             nullptr,
                                             0);
  if (required <= 0) {
    return false;
  }

  std::wstring buffer(static_cast<std::size_t>(required), L'\0');
  const int written = ::MultiByteToWideChar(CP_UTF8,
                                            MB_ERR_INVALID_CHARS,
                                            input.data(),
                                            static_cast<int>(input.size()),
                                            buffer.data(),
                                            required);
  if (written != required) {
    return false;
  }

  *output = std::move(buffer);
  return true;
}

std::optional<std::filesystem::path> ResolveExecutableOnPath(const wchar_t* executable) {
  if (executable == nullptr || executable[0] == L'\0') {
    return std::nullopt;
  }
  const DWORD required =
      ::SearchPathW(nullptr, executable, nullptr, 0, nullptr, nullptr);
  if (required == 0U) {
    return std::nullopt;
  }
  std::wstring buffer(required, L'\0');
  const DWORD written =
      ::SearchPathW(nullptr, executable, nullptr, required, buffer.data(), nullptr);
  if (written == 0U) {
    return std::nullopt;
  }
  buffer.resize(written);
  return std::filesystem::path(buffer);
}

void PushUniquePythonCandidate(const std::filesystem::path& path,
                               std::vector<std::filesystem::path>* candidates) {
  if (candidates == nullptr || path.empty()) {
    return;
  }
  std::error_code exists_error;
  if (!std::filesystem::exists(path, exists_error) || exists_error) {
    return;
  }
  const auto normalized = path.lexically_normal();
  for (const auto& existing : *candidates) {
    if (existing.lexically_normal() == normalized) {
      return;
    }
  }
  candidates->push_back(normalized);
}

std::vector<std::filesystem::path> GatherPythonAutostartCandidates(const RuntimeOptions& options) {
  std::vector<std::filesystem::path> candidates;
  for (const auto& root : BuildPythonSearchRoots(options)) {
    PushUniquePythonCandidate(root / "zsoda_py" / "python.exe", &candidates);
    PushUniquePythonCandidate(root / "zsoda_py" / "python" / "python.exe", &candidates);
    PushUniquePythonCandidate(root / "zsoda_py" / "runtime" / "python.exe", &candidates);
  }

  if (const auto path_python = ResolveExecutableOnPath(L"python.exe")) {
    PushUniquePythonCandidate(*path_python, &candidates);
  }
  if (const auto path_python3 = ResolveExecutableOnPath(L"python3.exe")) {
    PushUniquePythonCandidate(*path_python3, &candidates);
  }

  const std::filesystem::path local_appdata = ReadEnvOrEmpty("LOCALAPPDATA");
  const auto collect_python_children = [&](const std::filesystem::path& root,
                                           const auto& directory_name_filter) {
    std::error_code iter_error;
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::directory_iterator iter(root, options, iter_error);
    std::filesystem::directory_iterator end;
    while (!iter_error && iter != end) {
      const auto entry_path = iter->path();
      std::error_code status_error;
      const bool is_directory = iter->is_directory(status_error);
      if (!status_error && is_directory) {
        const std::wstring directory_name = entry_path.filename().wstring();
        if (directory_name_filter(directory_name)) {
          PushUniquePythonCandidate(entry_path / "python.exe", &candidates);
        }
      }

      iter.increment(iter_error);
    }
  };

  if (!local_appdata.empty()) {
    const std::filesystem::path windows_apps = local_appdata / "Microsoft" / "WindowsApps";
    collect_python_children(windows_apps, [](const std::wstring& directory_name) {
      return directory_name.find(L"PythonSoftwareFoundation.Python.") != std::wstring::npos;
    });

    const std::filesystem::path programs_python = local_appdata / "Programs" / "Python";
    collect_python_children(programs_python,
                            [](const std::wstring& /*directory_name*/) { return true; });
  }

  const std::filesystem::path system_drive = "C:\\";
  collect_python_children(system_drive, [](const std::wstring& directory_name) {
    return directory_name.rfind(L"Python", 0) == 0;
  });

  return candidates;
}

#else

std::string QuoteShellArgument(std::string_view argument) {
  std::string quoted;
  quoted.reserve(argument.size() + 4U);
  quoted.push_back('"');
  for (const char ch : argument) {
    if (ch == '"') {
      quoted.append("\\\"");
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back('"');
  return quoted;
}

void PushUniquePythonCandidate(const std::filesystem::path& path,
                               std::vector<std::filesystem::path>* candidates) {
  if (candidates == nullptr || path.empty()) {
    return;
  }
  std::error_code status_error;
  if (!std::filesystem::is_regular_file(path, status_error) || status_error) {
    return;
  }
  if (::access(path.c_str(), X_OK) != 0) {
    return;
  }
  const auto normalized = path.lexically_normal();
  for (const auto& existing : *candidates) {
    if (existing.lexically_normal() == normalized) {
      return;
    }
  }
  candidates->push_back(normalized);
}

std::optional<std::filesystem::path> ResolveExecutableOnPath(const char* executable) {
  if (executable == nullptr || executable[0] == '\0') {
    return std::nullopt;
  }

  const std::string executable_name(executable);
  const std::string path_env = ReadEnvOrEmpty("PATH");
  std::size_t start = 0U;
  while (start <= path_env.size()) {
    const std::size_t delimiter = path_env.find(':', start);
    const std::string_view entry =
        delimiter == std::string::npos
            ? std::string_view(path_env).substr(start)
            : std::string_view(path_env).substr(start, delimiter - start);
    if (!entry.empty()) {
      const std::filesystem::path candidate =
          std::filesystem::path(std::string(entry)) / executable_name;
      if (::access(candidate.c_str(), X_OK) == 0) {
        return candidate;
      }
    }
    if (delimiter == std::string::npos) {
      break;
    }
    start = delimiter + 1U;
  }
  return std::nullopt;
}

std::vector<std::filesystem::path> GatherPythonAutostartCandidates(const RuntimeOptions& options) {
  std::vector<std::filesystem::path> candidates;
  for (const auto& root : BuildPythonSearchRoots(options)) {
    PushUniquePythonCandidate(root / "zsoda_py" / "bin" / "python3", &candidates);
    PushUniquePythonCandidate(root / "zsoda_py" / "bin" / "python", &candidates);
    PushUniquePythonCandidate(root / "zsoda_py" / "python" / "bin" / "python3", &candidates);
    PushUniquePythonCandidate(root / "zsoda_py" / "python" / "bin" / "python", &candidates);
  }

  if (const auto python3 = ResolveExecutableOnPath("python3")) {
    PushUniquePythonCandidate(*python3, &candidates);
  }
  if (const auto python = ResolveExecutableOnPath("python")) {
    PushUniquePythonCandidate(*python, &candidates);
  }

  const std::array<std::filesystem::path, 4> known_candidates = {
      "/opt/homebrew/bin/python3",
      "/usr/local/bin/python3",
      "/usr/bin/python3",
      "/Library/Frameworks/Python.framework/Versions/Current/bin/python3",
  };
  for (const auto& candidate : known_candidates) {
    PushUniquePythonCandidate(candidate, &candidates);
  }
  return candidates;
}

#endif

}  // namespace

PythonTorchCapability ProbePythonTorchCapability(const std::filesystem::path& python_path,
                                                 std::string* probe_output) {
  if (probe_output != nullptr) {
    probe_output->clear();
  }
  if (python_path.empty()) {
    return PythonTorchCapability::kUnavailable;
  }

#if defined(_WIN32)
  std::error_code temp_error;
  const std::filesystem::path temp_root = std::filesystem::temp_directory_path(temp_error);
  if (temp_error) {
    return PythonTorchCapability::kUnavailable;
  }
  const auto tick_count = static_cast<unsigned long long>(::GetTickCount64());
  const auto process_id = static_cast<unsigned long long>(::GetCurrentProcessId());
  const std::filesystem::path probe_log =
      temp_root / ("ZSoda_PythonProbe_" + std::to_string(process_id) + "_" + std::to_string(tick_count) + ".log");

  SECURITY_ATTRIBUTES security_attributes = {};
  security_attributes.nLength = sizeof(security_attributes);
  security_attributes.bInheritHandle = TRUE;
  HANDLE output_handle =
      ::CreateFileW(probe_log.wstring().c_str(),
                    GENERIC_WRITE | GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    &security_attributes,
                    CREATE_ALWAYS,
                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                    nullptr);
  if (output_handle == INVALID_HANDLE_VALUE) {
    return PythonTorchCapability::kUnavailable;
  }

  const std::wstring python_wide = python_path.wstring();
  std::wstring probe_script;
  if (!Utf8ToWide(BuildPythonRuntimeProbeScript(), &probe_script)) {
    ::CloseHandle(output_handle);
    return PythonTorchCapability::kUnavailable;
  }

  std::wstring command_line = L"\"";
  command_line.append(python_wide);
  command_line.append(L"\" -c \"");
  command_line.append(probe_script);
  command_line.append(L"\"");

  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  startup_info.hStdOutput = output_handle;
  startup_info.hStdError = output_handle;

  PROCESS_INFORMATION process_info = {};
  std::wstring mutable_command_line = command_line;
  const BOOL create_ok = ::CreateProcessW(nullptr,
                                          mutable_command_line.data(),
                                          nullptr,
                                          nullptr,
                                          TRUE,
                                          CREATE_NO_WINDOW,
                                          nullptr,
                                          python_path.parent_path().wstring().c_str(),
                                          &startup_info,
                                          &process_info);
  if (!create_ok) {
    ::CloseHandle(output_handle);
    return PythonTorchCapability::kUnavailable;
  }

  const DWORD wait_result = ::WaitForSingleObject(process_info.hProcess, 15000);
  if (wait_result == WAIT_TIMEOUT) {
    ::TerminateProcess(process_info.hProcess, 1);
    ::WaitForSingleObject(process_info.hProcess, 2000);
  }
  DWORD exit_code = 1;
  ::GetExitCodeProcess(process_info.hProcess, &exit_code);
  ::CloseHandle(process_info.hThread);
  ::CloseHandle(process_info.hProcess);

  ::SetFilePointer(output_handle, 0, nullptr, FILE_BEGIN);
  std::string captured;
  std::array<char, 256> buffer{};
  DWORD bytes_read = 0;
  while (::ReadFile(output_handle, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr) &&
         bytes_read > 0) {
    captured.append(buffer.data(), static_cast<std::size_t>(bytes_read));
  }
  ::CloseHandle(output_handle);
  if (probe_output != nullptr) {
    *probe_output = captured;
  }

  if (exit_code != 0) {
    return PythonTorchCapability::kUnavailable;
  }
#else
  const std::string command = QuoteShellArgument(python_path.string()) + " -c " +
                              QuoteShellArgument(BuildPythonRuntimeProbeScript()) + " 2>&1";
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return PythonTorchCapability::kUnavailable;
  }

  std::string captured;
  std::array<char, 256> buffer{};
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    captured.append(buffer.data());
  }
  const int close_result = ::pclose(pipe);
  if (probe_output != nullptr) {
    *probe_output = captured;
  }
  if (close_result != 0) {
    return PythonTorchCapability::kUnavailable;
  }
#endif

  if (captured.find("MPS=1") != std::string::npos) {
    return PythonTorchCapability::kTorchMps;
  }
  if (captured.find("CUDA=1") != std::string::npos) {
    return PythonTorchCapability::kTorchCuda;
  }
  if (captured.find("CPU=1") != std::string::npos) {
    return PythonTorchCapability::kTorchCpu;
  }
  return PythonTorchCapability::kUnavailable;
}

std::string ResolvePythonCommandForAutostart(const RuntimeOptions& options) {
  return ResolvePythonAutostartSelection(options).python_path.string();
}

PythonAutostartSelection ResolvePythonAutostartSelection(const RuntimeOptions& options) {
  PythonAutostartSelection fallback_selection;
  try {
    if (!options.remote_service_python.empty()) {
      fallback_selection.python_path = std::filesystem::path(options.remote_service_python);
      fallback_selection.used_explicit_override = true;
      fallback_selection.capability =
          ProbePythonTorchCapability(fallback_selection.python_path, &fallback_selection.probe_output);
      return fallback_selection;
    }

    const auto candidates = GatherPythonAutostartCandidates(options);
#if defined(_WIN32)
    PythonAutostartSelection cpu_fallback;
    for (const auto& candidate : candidates) {
      std::string probe_output;
      const auto capability = ProbePythonTorchCapability(candidate, &probe_output);
      if (capability == PythonTorchCapability::kTorchCuda) {
        return {
            .python_path = candidate,
            .capability = capability,
            .probe_output = std::move(probe_output),
            .used_explicit_override = false,
        };
      }
      if (capability == PythonTorchCapability::kTorchCpu && cpu_fallback.python_path.empty()) {
        cpu_fallback.python_path = candidate;
        cpu_fallback.capability = capability;
        cpu_fallback.probe_output = std::move(probe_output);
      }
    }

    if (!cpu_fallback.python_path.empty()) {
      return cpu_fallback;
    }
#else
    PythonAutostartSelection mps_fallback;
    PythonAutostartSelection cuda_fallback;
    PythonAutostartSelection cpu_fallback;
    for (const auto& candidate : candidates) {
      std::string probe_output;
      const auto capability = ProbePythonTorchCapability(candidate, &probe_output);
      if (capability == PythonTorchCapability::kTorchMps) {
        return {
            .python_path = candidate,
            .capability = capability,
            .probe_output = std::move(probe_output),
            .used_explicit_override = false,
        };
      }
      if (capability == PythonTorchCapability::kTorchCuda && cuda_fallback.python_path.empty()) {
        cuda_fallback.python_path = candidate;
        cuda_fallback.capability = capability;
        cuda_fallback.probe_output = std::move(probe_output);
      }
      if (capability == PythonTorchCapability::kTorchCpu && cpu_fallback.python_path.empty()) {
        cpu_fallback.python_path = candidate;
        cpu_fallback.capability = capability;
        cpu_fallback.probe_output = std::move(probe_output);
      }
      if (capability != PythonTorchCapability::kUnavailable && mps_fallback.python_path.empty()) {
        mps_fallback.python_path = candidate;
        mps_fallback.capability = capability;
        mps_fallback.probe_output = std::move(probe_output);
      }
    }

    if (!cuda_fallback.python_path.empty()) {
      return cuda_fallback;
    }
    if (!cpu_fallback.python_path.empty()) {
      return cpu_fallback;
    }
    if (!mps_fallback.python_path.empty()) {
      return mps_fallback;
    }
#endif
  } catch (...) {
    // Autodiscovery must never break AE loader setup. Fall back to PATH python.
  }

#if defined(_WIN32)
  fallback_selection.python_path = "python";
#else
  fallback_selection.python_path = "python3";
#endif
  fallback_selection.capability =
      ProbePythonTorchCapability(fallback_selection.python_path, &fallback_selection.probe_output);
  return fallback_selection;
}

}  // namespace zsoda::inference
