#include "inference/RuntimePathResolver.h"

#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace zsoda::inference {
namespace {

bool HasText(const std::string& value) {
  return !value.empty();
}

bool IsExistingDirectory(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::is_directory(path, ec);
}

bool IsExistingFile(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec);
}

void SetResolvedOnnxRuntimeLibraryPath(RuntimePathResolution* resolved,
                                       const std::filesystem::path& path) {
  if (resolved == nullptr) {
    return;
  }
  resolved->onnxruntime_library_path = path.string();
  const auto parent = path.parent_path();
  resolved->onnxruntime_library_dir = parent.empty() ? std::string() : parent.string();
}

std::optional<std::filesystem::path> ParsePluginDirectoryPath(
    const std::optional<std::string>& plugin_directory) {
  if (!plugin_directory.has_value() || plugin_directory->empty()) {
    return std::nullopt;
  }
  return std::filesystem::path(*plugin_directory);
}

#if defined(_WIN32)
bool WideToUtf8(const std::wstring& wide, std::string* utf8, std::string* error) {
  if (utf8 == nullptr) {
    if (error != nullptr) {
      *error = "internal error: utf8 output pointer is null";
    }
    return false;
  }
  utf8->clear();
  if (wide.empty()) {
    return true;
  }

  const int required = ::WideCharToMultiByte(CP_UTF8,
                                             0,
                                             wide.data(),
                                             static_cast<int>(wide.size()),
                                             nullptr,
                                             0,
                                             nullptr,
                                             nullptr);
  if (required <= 0) {
    if (error != nullptr) {
      *error = "WideCharToMultiByte failed: " + std::to_string(::GetLastError());
    }
    return false;
  }

  utf8->assign(static_cast<std::size_t>(required), '\0');
  const int converted = ::WideCharToMultiByte(CP_UTF8,
                                              0,
                                              wide.data(),
                                              static_cast<int>(wide.size()),
                                              utf8->data(),
                                              required,
                                              nullptr,
                                              nullptr);
  if (converted != required) {
    if (error != nullptr) {
      *error = "WideCharToMultiByte produced unexpected length";
    }
    return false;
  }
  return true;
}
#endif

}  // namespace

const char* DefaultOnnxRuntimeLibraryFileName() {
#if defined(_WIN32)
  return "onnxruntime.dll";
#elif defined(__APPLE__)
  return "libonnxruntime.dylib";
#else
  return "libonnxruntime.so";
#endif
}

RuntimePathResolution ResolveRuntimePaths(const RuntimePathHints& hints) {
  RuntimePathResolution resolved;
  const auto plugin_directory = ParsePluginDirectoryPath(hints.plugin_directory);

  if (HasText(hints.model_root_env)) {
    resolved.model_root = hints.model_root_env;
  } else if (plugin_directory.has_value()) {
    const auto plugin_models = *plugin_directory / "models";
    if (IsExistingDirectory(plugin_models)) {
      resolved.model_root = plugin_models.string();
    }
  }

  if (resolved.model_root.empty()) {
    resolved.model_root = "models";
  }

  if (HasText(hints.model_manifest_env)) {
    resolved.model_manifest_path = hints.model_manifest_env;
  } else {
    const auto model_root_manifest = std::filesystem::path(resolved.model_root) / "models.manifest";
    if (IsExistingFile(model_root_manifest)) {
      resolved.model_manifest_path = model_root_manifest.string();
    } else if (plugin_directory.has_value()) {
      const auto plugin_manifest = *plugin_directory / "models" / "models.manifest";
      if (IsExistingFile(plugin_manifest)) {
        resolved.model_manifest_path = plugin_manifest.string();
      }
    }
  }

  if (HasText(hints.onnxruntime_library_env)) {
    SetResolvedOnnxRuntimeLibraryPath(&resolved, std::filesystem::path(hints.onnxruntime_library_env));
  } else if (plugin_directory.has_value()) {
    const std::vector<std::filesystem::path> candidates = {
        *plugin_directory / "runtime" / DefaultOnnxRuntimeLibraryFileName(),
        *plugin_directory / DefaultOnnxRuntimeLibraryFileName(),
    };
    for (const auto& candidate : candidates) {
      if (IsExistingFile(candidate)) {
        SetResolvedOnnxRuntimeLibraryPath(&resolved, candidate);
        break;
      }
    }
  }

  return resolved;
}

std::optional<std::string> TryResolveCurrentModuleDirectory(std::string* error) {
#if !defined(_WIN32)
  if (error != nullptr) {
    error->clear();
  }
  return std::nullopt;
#else
  HMODULE module = nullptr;
  const BOOL ok = ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(
                                           reinterpret_cast<const void*>(&TryResolveCurrentModuleDirectory)),
                                       &module);
  if (!ok || module == nullptr) {
    if (error != nullptr) {
      *error = "GetModuleHandleExW failed: " + std::to_string(::GetLastError());
    }
    return std::nullopt;
  }

  std::vector<wchar_t> buffer(260U, L'\0');
  for (int attempt = 0; attempt < 6; ++attempt) {
    const DWORD written =
        ::GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0U) {
      if (error != nullptr) {
        *error = "GetModuleFileNameW failed: " + std::to_string(::GetLastError());
      }
      return std::nullopt;
    }
    if (written < buffer.size() - 1U) {
      const std::filesystem::path module_path(
          std::wstring(buffer.data(), static_cast<std::size_t>(written)));
      const auto parent = module_path.parent_path();
      if (parent.empty()) {
        if (error != nullptr) {
          *error = "module path has no parent directory";
        }
        return std::nullopt;
      }
      std::string utf8;
      std::string utf8_error;
      if (!WideToUtf8(parent.native(), &utf8, &utf8_error)) {
        if (error != nullptr) {
          *error = utf8_error;
        }
        return std::nullopt;
      }
      if (error != nullptr) {
        error->clear();
      }
      return utf8;
    }
    buffer.resize(buffer.size() * 2U, L'\0');
  }

  if (error != nullptr) {
    *error = "GetModuleFileNameW failed: path length exceeded retry limit";
  }
  return std::nullopt;
#endif
}

}  // namespace zsoda::inference
