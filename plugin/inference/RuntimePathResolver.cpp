#include "inference/RuntimePathResolver.h"

#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <dlfcn.h>
#endif

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

std::string ToLowerCopy(std::string_view value) {
  std::string lowered;
  lowered.reserve(value.size());
  for (const char ch : value) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return lowered;
}

bool IsExistingDirectory(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::is_directory(path, ec);
}

bool IsExistingFile(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec);
}

bool IsSameOnnxRuntimeFileName(const std::filesystem::path& path) {
  const auto filename = path.filename().string();
  if (filename.empty()) {
    return false;
  }
  std::string lowered;
  lowered.reserve(filename.size());
  for (const char ch : filename) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return lowered == "onnxruntime.dll" || lowered == "libonnxruntime.dylib" ||
         lowered == "libonnxruntime.so";
}

void AppendUniquePath(std::vector<std::filesystem::path>* paths,
                      const std::filesystem::path& candidate) {
  if (paths == nullptr || candidate.empty()) {
    return;
  }

  const auto normalized = candidate.lexically_normal();
  if (normalized.empty()) {
    return;
  }
  for (const auto& existing : *paths) {
    if (existing == normalized) {
      return;
    }
  }
  paths->push_back(normalized);
}

std::optional<std::filesystem::path> TryResolveBundleContentsDirectory(
    const std::filesystem::path& plugin_directory) {
  if (plugin_directory.empty()) {
    return std::nullopt;
  }

  const std::string leaf = ToLowerCopy(plugin_directory.filename().string());
  if (leaf == "contents") {
    return plugin_directory;
  }

  const auto parent = plugin_directory.parent_path();
  if (!parent.empty() && ToLowerCopy(parent.filename().string()) == "contents") {
    return parent;
  }

  if (ToLowerCopy(plugin_directory.extension().string()) == ".plugin") {
    return plugin_directory / "Contents";
  }

  return std::nullopt;
}

std::filesystem::path PreferredRuntimeAssetRoot(const std::filesystem::path& plugin_directory) {
  if (const auto contents_dir = TryResolveBundleContentsDirectory(plugin_directory);
      contents_dir.has_value()) {
    return *contents_dir / "Resources";
  }
  return plugin_directory;
}

std::filesystem::path PreferIsolatedOnnxRuntimePath(const std::filesystem::path& candidate) {
  if (!IsSameOnnxRuntimeFileName(candidate)) {
    return candidate;
  }
  const auto parent = candidate.parent_path();
  if (parent.empty()) {
    return candidate;
  }
  const auto isolated_candidate = parent / "zsoda_ort" / candidate.filename();
  if (IsExistingFile(isolated_candidate)) {
    return isolated_candidate;
  }
  return candidate;
}

void SetResolvedOnnxRuntimeLibraryPath(RuntimePathResolution* resolved,
                                       const std::filesystem::path& path) {
  if (resolved == nullptr) {
    return;
  }
  const auto preferred = PreferIsolatedOnnxRuntimePath(path);
  resolved->onnxruntime_library_path = preferred.string();
  const auto parent = preferred.parent_path();
  resolved->onnxruntime_library_dir = parent.empty() ? std::string() : parent.string();
}

std::optional<std::filesystem::path> ParsePluginDirectoryPath(
    const std::optional<std::string>& plugin_directory) {
  if (!plugin_directory.has_value() || plugin_directory->empty()) {
    return std::nullopt;
  }
  return std::filesystem::path(*plugin_directory);
}

std::optional<std::filesystem::path> ResolveAdobeMediaCoreModelsDirectory(
    const std::filesystem::path& plugin_directory) {
  if (plugin_directory.empty()) {
    return std::nullopt;
  }

  std::filesystem::path cursor = plugin_directory;
  while (!cursor.empty()) {
    const std::string leaf = ToLowerCopy(cursor.filename().string());
    if (leaf == "adobe") {
      const std::filesystem::path media_core_models =
          cursor / "Common" / "Plug-ins" / "7.0" / "MediaCore" / "models";
      if (IsExistingDirectory(media_core_models)) {
        return media_core_models;
      }
      break;
    }

    const std::filesystem::path parent = cursor.parent_path();
    if (parent.empty() || parent == cursor) {
      break;
    }
    cursor = parent;
  }

  return std::nullopt;
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

std::vector<std::filesystem::path> BuildRuntimeAssetSearchRoots(
    const std::filesystem::path& plugin_directory,
    const std::filesystem::path& extra_asset_root) {
  std::vector<std::filesystem::path> roots;
  AppendUniquePath(&roots, extra_asset_root);
  AppendUniquePath(&roots, plugin_directory);

  const auto contents_dir = TryResolveBundleContentsDirectory(plugin_directory);
  if (!contents_dir.has_value()) {
    return roots;
  }

  AppendUniquePath(&roots, *contents_dir);
  AppendUniquePath(&roots, *contents_dir / "Resources");
  AppendUniquePath(&roots, *contents_dir / "MacOS");
  AppendUniquePath(&roots, contents_dir->parent_path());
  return roots;
}

RuntimePathResolution ResolveRuntimePaths(const RuntimePathHints& hints) {
  RuntimePathResolution resolved;
  const auto plugin_directory = ParsePluginDirectoryPath(hints.plugin_directory);
  const std::vector<std::filesystem::path> search_roots =
      plugin_directory.has_value()
          ? BuildRuntimeAssetSearchRoots(*plugin_directory,
                                        std::filesystem::path(hints.bundled_asset_root))
                                   : std::vector<std::filesystem::path>{};
  const std::filesystem::path preferred_asset_root =
      plugin_directory.has_value() ? PreferredRuntimeAssetRoot(*plugin_directory)
                                   : std::filesystem::path{};

  if (HasText(hints.model_root_env)) {
    resolved.model_root = hints.model_root_env;
  } else if (plugin_directory.has_value()) {
    for (const auto& root : search_roots) {
      const auto plugin_models = root / "models";
      if (IsExistingDirectory(plugin_models)) {
        resolved.model_root = plugin_models.string();
        break;
      }
    }
    if (resolved.model_root.empty()) {
      if (const auto media_core_models =
              ResolveAdobeMediaCoreModelsDirectory(*plugin_directory);
          media_core_models.has_value()) {
        resolved.model_root = media_core_models->string();
      } else {
        // Keep model root deterministic/absolute to avoid process working-directory drift.
        resolved.model_root = (preferred_asset_root / "models").string();
      }
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
      for (const auto& root : search_roots) {
        const auto plugin_manifest = root / "models" / "models.manifest";
        if (IsExistingFile(plugin_manifest)) {
          resolved.model_manifest_path = plugin_manifest.string();
          break;
        }
      }
    }
  }

  if (HasText(hints.onnxruntime_library_env)) {
    SetResolvedOnnxRuntimeLibraryPath(&resolved, std::filesystem::path(hints.onnxruntime_library_env));
  } else if (plugin_directory.has_value()) {
    for (const auto& root : search_roots) {
      const std::vector<std::filesystem::path> candidates = {
          root / "zsoda_ort" / DefaultOnnxRuntimeLibraryFileName(),
          root / "runtime" / "win-x64" / DefaultOnnxRuntimeLibraryFileName(),
          root / "runtime" / DefaultOnnxRuntimeLibraryFileName(),
          root / DefaultOnnxRuntimeLibraryFileName(),
      };
      for (const auto& candidate : candidates) {
        if (IsExistingFile(candidate)) {
          SetResolvedOnnxRuntimeLibraryPath(&resolved, candidate);
          break;
        }
      }
      if (!resolved.onnxruntime_library_path.empty()) {
        break;
      }
    }
  }

  return resolved;
}

std::optional<std::string> TryResolveCurrentModulePath(std::string* error) {
#if defined(__APPLE__)
  Dl_info info = {};
  if (::dladdr(reinterpret_cast<const void*>(&TryResolveCurrentModulePath), &info) == 0 ||
      info.dli_fname == nullptr || info.dli_fname[0] == '\0') {
    if (error != nullptr) {
      *error = "dladdr failed to resolve current module path";
    }
    return std::nullopt;
  }

  const std::filesystem::path module_path(info.dli_fname);
  if (error != nullptr) {
    error->clear();
  }
  return module_path.string();
#elif !defined(_WIN32)
  if (error != nullptr) {
    error->clear();
  }
  return std::nullopt;
#else
  HMODULE module = nullptr;
  const BOOL ok = ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(
                                           reinterpret_cast<const void*>(&TryResolveCurrentModulePath)),
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
      std::string utf8;
      std::string utf8_error;
      if (!WideToUtf8(module_path.native(), &utf8, &utf8_error)) {
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

std::optional<std::string> TryResolveCurrentModuleDirectory(std::string* error) {
  const auto module_path = TryResolveCurrentModulePath(error);
  if (!module_path.has_value() || module_path->empty()) {
    return std::nullopt;
  }

  const auto parent = std::filesystem::path(*module_path).parent_path();
  if (parent.empty()) {
    if (error != nullptr) {
      *error = "module path has no parent directory";
    }
    return std::nullopt;
  }

  if (error != nullptr) {
    error->clear();
  }
  return parent.string();
}

}  // namespace zsoda::inference
