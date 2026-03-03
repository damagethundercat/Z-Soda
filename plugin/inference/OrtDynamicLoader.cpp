#include "inference/OrtDynamicLoader.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef ORT_API_VERSION
#define ORT_API_VERSION 1
#endif

#ifndef ORT_API_CALL
#define ORT_API_CALL
#endif

namespace zsoda::inference {
namespace {

#if defined(_WIN32)
HMODULE CurrentModuleHandle() {
  HMODULE module = nullptr;
  const void* address = reinterpret_cast<const void*>(&CurrentModuleHandle);
  const BOOL ok =
      ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address),
                           &module);
  if (!ok) {
    return nullptr;
  }
  return module;
}

bool QueryModulePath(HMODULE module, std::wstring* path, std::string* error) {
  if (module == nullptr || path == nullptr) {
    if (error != nullptr) {
      *error = "internal error: module/path pointer is null";
    }
    return false;
  }

  std::vector<wchar_t> buffer(260U, L'\0');
  for (int attempt = 0; attempt < 6; ++attempt) {
    const DWORD written =
        ::GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0U) {
      if (error != nullptr) {
        *error = "GetModuleFileNameW failed: " + std::to_string(::GetLastError());
      }
      return false;
    }
    if (written < buffer.size() - 1U) {
      path->assign(buffer.data(), static_cast<std::size_t>(written));
      return true;
    }
    buffer.resize(buffer.size() * 2U, L'\0');
  }

  if (error != nullptr) {
    *error = "GetModuleFileNameW failed: path length exceeded retry limit";
  }
  return false;
}

std::wstring ParentDirectory(const std::wstring& path) {
  if (path.empty()) {
    return {};
  }
  const std::size_t pos = path.find_last_of(L"\\/");
  if (pos == std::wstring::npos) {
    return {};
  }
  return path.substr(0, pos);
}

bool FileExists(const std::wstring& path) {
  if (path.empty()) {
    return false;
  }
  const DWORD attrs = ::GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& leaf) {
  if (dir.empty()) {
    return leaf;
  }
  if (dir.back() == L'\\' || dir.back() == L'/') {
    return dir + leaf;
  }
  return dir + L"\\" + leaf;
}

bool DirectoryExists(const std::wstring& path) {
  if (path.empty()) {
    return false;
  }
  const DWORD attrs = ::GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void AppendDiagnosticFragment(std::string* diagnostics, const std::string& fragment) {
  if (diagnostics == nullptr || fragment.empty()) {
    return;
  }
  if (!diagnostics->empty()) {
    diagnostics->append("; ");
  }
  diagnostics->append(fragment);
}

void AppendUniqueDirectoryIfExists(const std::wstring& candidate, std::vector<std::wstring>* out) {
  if (out == nullptr || candidate.empty() || !DirectoryExists(candidate)) {
    return;
  }
  for (const auto& existing : *out) {
    if (existing == candidate) {
      return;
    }
  }
  out->push_back(candidate);
}

std::vector<std::wstring> BuildDllSearchDirectories(const std::wstring& load_path_wide) {
  std::vector<std::wstring> directories;
  const std::wstring dll_dir = ParentDirectory(load_path_wide);
  if (dll_dir.empty()) {
    return directories;
  }

  const std::wstring parent_dir = ParentDirectory(dll_dir);
  const std::wstring runtime_dir = JoinPath(parent_dir, L"runtime");
  const std::wstring runtime_win64_dir = JoinPath(runtime_dir, L"win-x64");

  AppendUniqueDirectoryIfExists(dll_dir, &directories);
  AppendUniqueDirectoryIfExists(JoinPath(dll_dir, L"win-x64"), &directories);
  AppendUniqueDirectoryIfExists(JoinPath(dll_dir, L"runtime"), &directories);
  AppendUniqueDirectoryIfExists(JoinPath(JoinPath(dll_dir, L"runtime"), L"win-x64"), &directories);
  AppendUniqueDirectoryIfExists(runtime_dir, &directories);
  AppendUniqueDirectoryIfExists(runtime_win64_dir, &directories);

  return directories;
}

struct OrtApiBaseCompat {
  const OrtApi* (ORT_API_CALL* GetApi)(std::uint32_t version);
  const char* (ORT_API_CALL* GetVersionString)(void);
};

std::string TrimTrailingWhitespace(std::string value) {
  while (!value.empty()) {
    const char ch = value.back();
    if (ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ') {
      value.pop_back();
      continue;
    }
    break;
  }
  return value;
}

std::string FormatWin32ErrorMessage(DWORD error_code) {
  LPSTR buffer = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD length = ::FormatMessageA(flags,
                                        nullptr,
                                        error_code,
                                        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                        reinterpret_cast<LPSTR>(&buffer),
                                        0,
                                        nullptr);

  if (length == 0 || buffer == nullptr) {
    return "code=" + std::to_string(error_code);
  }

  std::string message(buffer, buffer + length);
  ::LocalFree(buffer);
  return "code=" + std::to_string(error_code) + " (" + TrimTrailingWhitespace(message) + ")";
}

bool Utf8ToWide(const std::string& utf8, std::wstring* wide, std::string* error) {
  if (wide == nullptr) {
    if (error != nullptr) {
      *error = "internal error: wide output pointer is null";
    }
    return false;
  }
  wide->clear();
  if (utf8.empty()) {
    return true;
  }

  const int required = ::MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (required <= 0) {
    if (error != nullptr) {
      *error = "utf8->wide conversion failed: " + FormatWin32ErrorMessage(::GetLastError());
    }
    return false;
  }

  wide->assign(static_cast<std::size_t>(required), L'\0');
  const int converted = ::MultiByteToWideChar(CP_UTF8,
                                              MB_ERR_INVALID_CHARS,
                                              utf8.data(),
                                              static_cast<int>(utf8.size()),
                                              wide->data(),
                                              required);
  if (converted != required) {
    if (error != nullptr) {
      *error = "utf8->wide conversion produced unexpected length";
    }
    return false;
  }
  return true;
}

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
      *error = "wide->utf8 conversion failed: " + FormatWin32ErrorMessage(::GetLastError());
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
      *error = "wide->utf8 conversion produced unexpected length";
    }
    return false;
  }
  return true;
}

bool ResolveLoadPath(const std::string& dll_path,
                     std::wstring* load_path_wide,
                     std::string* load_path_utf8,
                     std::string* error) {
  if (load_path_wide == nullptr || load_path_utf8 == nullptr) {
    if (error != nullptr) {
      *error = "internal error: load path output pointer is null";
    }
    return false;
  }

  if (dll_path.empty()) {
    std::string module_path_error;
    std::wstring module_path;
    const HMODULE module = CurrentModuleHandle();
    if (module != nullptr && QueryModulePath(module, &module_path, &module_path_error)) {
      const std::wstring candidate =
          JoinPath(ParentDirectory(module_path), std::wstring(L"onnxruntime.dll"));
      if (FileExists(candidate)) {
        *load_path_wide = candidate;
        return WideToUtf8(*load_path_wide, load_path_utf8, error);
      }
    }
    if (error != nullptr) {
      std::ostringstream oss;
      oss << "onnxruntime dll path is not configured and side-by-side dll was not found"
          << " (expected next to plugin module)."
          << " set ZSODA_ONNXRUNTIME_LIBRARY or pass ZSODA_ONNXRUNTIME_DLL_PATH_HINT."
          << " module_resolve_error="
          << (module_path_error.empty() ? "<none>" : module_path_error);
      *error = oss.str();
    }
    return false;
  }

  std::wstring requested_wide;
  if (!Utf8ToWide(dll_path, &requested_wide, error)) {
    return false;
  }

  DWORD required = ::GetFullPathNameW(requested_wide.c_str(), 0, nullptr, nullptr);
  if (required == 0U) {
    // If absolute resolution fails, keep the original path and still attempt to load.
    *load_path_wide = requested_wide;
  } else {
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U, L'\0');
    const DWORD written = ::GetFullPathNameW(
        requested_wide.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (written == 0U || written >= buffer.size()) {
      *load_path_wide = requested_wide;
    } else {
      load_path_wide->assign(buffer.data(), static_cast<std::size_t>(written));
    }
  }

  if (!FileExists(*load_path_wide)) {
    if (error != nullptr) {
      std::string missing_path_utf8;
      std::string conversion_error;
      if (!WideToUtf8(*load_path_wide, &missing_path_utf8, &conversion_error)) {
        missing_path_utf8 = "<path conversion failed: " + conversion_error + ">";
      }
      *error = "resolved ORT DLL path does not exist: " + missing_path_utf8;
    }
    return false;
  }

  return WideToUtf8(*load_path_wide, load_path_utf8, error);
}

bool QueryLoadedModulePath(HMODULE module, std::string* loaded_path, std::string* error) {
  if (module == nullptr || loaded_path == nullptr) {
    if (error != nullptr) {
      *error = "internal error: module/path pointer is null";
    }
    return false;
  }

  std::vector<wchar_t> buffer(260U, L'\0');
  for (int attempt = 0; attempt < 6; ++attempt) {
    const DWORD written =
        ::GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0U) {
      if (error != nullptr) {
        *error = "GetModuleFileNameW failed: " + FormatWin32ErrorMessage(::GetLastError());
      }
      return false;
    }
    if (written < buffer.size() - 1U) {
      const std::wstring module_path(buffer.data(), static_cast<std::size_t>(written));
      return WideToUtf8(module_path, loaded_path, error);
    }
    buffer.resize(buffer.size() * 2U, L'\0');
  }

  if (error != nullptr) {
    *error = "GetModuleFileNameW failed: path length exceeded retry limit";
  }
  return false;
}

bool TryResolvePreloadedOrtModule(HMODULE* module, std::string* loaded_path, std::string* error) {
  if (module == nullptr || loaded_path == nullptr) {
    if (error != nullptr) {
      *error = "internal error: preloaded ORT output pointer is null";
    }
    return false;
  }
  *module = nullptr;
  loaded_path->clear();

  HMODULE preloaded = nullptr;
  const BOOL ok = ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       L"onnxruntime.dll",
                                       &preloaded);
  if (!ok || preloaded == nullptr) {
    if (error != nullptr) {
      *error = "preloaded onnxruntime.dll is not present in process module list";
    }
    return false;
  }

  std::string loaded_path_error;
  if (!QueryLoadedModulePath(preloaded, loaded_path, &loaded_path_error)) {
    if (error != nullptr) {
      *error = "preloaded ORT found but path query failed: " + loaded_path_error;
    }
    return false;
  }

  *module = preloaded;
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool BindOrtApiFromModule(HMODULE module,
                          std::uint32_t requested_api_version,
                          const OrtApiBase** api_base,
                          const OrtApi** api,
                          std::uint32_t* negotiated_api_version,
                          std::string* runtime_version,
                          std::string* error) {
  if (module == nullptr || api_base == nullptr || api == nullptr || negotiated_api_version == nullptr ||
      runtime_version == nullptr) {
    if (error != nullptr) {
      *error = "internal error: invalid ORT binding output pointers";
    }
    return false;
  }

  *api_base = nullptr;
  *api = nullptr;
  *negotiated_api_version = 0;
  runtime_version->clear();

  FARPROC symbol = ::GetProcAddress(module, "OrtGetApiBase");
  if (symbol == nullptr) {
    if (error != nullptr) {
      *error = "GetProcAddress(\"OrtGetApiBase\") failed: " +
               FormatWin32ErrorMessage(::GetLastError());
    }
    return false;
  }

  using OrtGetApiBaseFn = const OrtApiBase* (ORT_API_CALL*)(void);
  const auto get_api_base = reinterpret_cast<OrtGetApiBaseFn>(symbol);
  const OrtApiBase* resolved_base = get_api_base();
  if (resolved_base == nullptr) {
    if (error != nullptr) {
      *error = "OrtGetApiBase returned null";
    }
    return false;
  }

  const auto* api_base_compat = reinterpret_cast<const OrtApiBaseCompat*>(resolved_base);
  if (api_base_compat->GetApi == nullptr) {
    if (error != nullptr) {
      *error = "OrtApiBase::GetApi is null";
    }
    return false;
  }

  const std::uint32_t max_try =
      requested_api_version > 0 ? requested_api_version : static_cast<std::uint32_t>(ORT_API_VERSION);
  const OrtApi* resolved_api = nullptr;
  std::uint32_t resolved_version = 0;
  for (std::uint32_t version = max_try; version >= 1; --version) {
    const OrtApi* candidate = api_base_compat->GetApi(version);
    if (candidate != nullptr) {
      resolved_api = candidate;
      resolved_version = version;
      break;
    }
    if (version == 1) {
      break;
    }
  }

  if (resolved_api == nullptr) {
    if (error != nullptr) {
      *error = std::string(
                   "OrtApiBase::GetApi returned null for every requested version down to 1 "
                   "(requested=") +
               std::to_string(max_try) + ")";
    }
    return false;
  }

  if (api_base_compat->GetVersionString != nullptr) {
    const char* version = api_base_compat->GetVersionString();
    if (version != nullptr) {
      *runtime_version = version;
    }
  }

  *api_base = resolved_base;
  *api = resolved_api;
  *negotiated_api_version = resolved_version;
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

struct LoadAttempt {
  DWORD flags = 0;
  bool uses_ex = true;
  const char* label = "";
};

bool TryLoadOrtModuleWithFallback(const std::wstring& load_path_wide,
                                  HMODULE* module,
                                  std::string* attempts_diagnostics) {
  if (module == nullptr || attempts_diagnostics == nullptr) {
    return false;
  }
  *module = nullptr;
  attempts_diagnostics->clear();

  const std::vector<std::wstring> search_dirs = BuildDllSearchDirectories(load_path_wide);

  HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
  using SetDefaultDllDirectoriesFn = BOOL(WINAPI*)(DWORD);
  using AddDllDirectoryFn = DLL_DIRECTORY_COOKIE(WINAPI*)(PCWSTR);
  using RemoveDllDirectoryFn = BOOL(WINAPI*)(DLL_DIRECTORY_COOKIE);

  const auto set_default_dirs = reinterpret_cast<SetDefaultDllDirectoriesFn>(
      kernel32 == nullptr ? nullptr : ::GetProcAddress(kernel32, "SetDefaultDllDirectories"));
  const auto add_dll_dir = reinterpret_cast<AddDllDirectoryFn>(
      kernel32 == nullptr ? nullptr : ::GetProcAddress(kernel32, "AddDllDirectory"));
  const auto remove_dll_dir = reinterpret_cast<RemoveDllDirectoryFn>(
      kernel32 == nullptr ? nullptr : ::GetProcAddress(kernel32, "RemoveDllDirectory"));

#if defined(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)
  const DWORD default_dll_search_flags = LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
#else
  const DWORD default_dll_search_flags = 0;
#endif

  if (set_default_dirs != nullptr && default_dll_search_flags != 0U) {
    if (!set_default_dirs(default_dll_search_flags)) {
      AppendDiagnosticFragment(attempts_diagnostics,
                               "SetDefaultDllDirectories failed: " +
                                   FormatWin32ErrorMessage(::GetLastError()));
    }
  }

  std::vector<DLL_DIRECTORY_COOKIE> directory_cookies;
  if (add_dll_dir != nullptr) {
    for (const auto& directory : search_dirs) {
      std::string directory_utf8;
      std::string conversion_error;
      if (!WideToUtf8(directory, &directory_utf8, &conversion_error)) {
        directory_utf8 = "<utf8-convert-failed: " + conversion_error + ">";
      }

      const DLL_DIRECTORY_COOKIE cookie = add_dll_dir(directory.c_str());
      if (cookie != nullptr) {
        directory_cookies.push_back(cookie);
        AppendDiagnosticFragment(attempts_diagnostics, "AddDllDirectory success: " + directory_utf8);
      } else {
        AppendDiagnosticFragment(attempts_diagnostics,
                                 "AddDllDirectory failed: " + directory_utf8 + " (" +
                                     FormatWin32ErrorMessage(::GetLastError()) + ")");
      }
    }
  } else if (!search_dirs.empty()) {
    AppendDiagnosticFragment(attempts_diagnostics,
                             "AddDllDirectory API unavailable; using default process DLL search path");
  }

  std::vector<LoadAttempt> attempts;
#if defined(LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR) && defined(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)
  attempts.push_back({LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS,
                      true,
                      "LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS"});
#endif
  attempts.push_back({LOAD_WITH_ALTERED_SEARCH_PATH, true, "LOAD_WITH_ALTERED_SEARCH_PATH"});
  attempts.push_back({0, false, "LoadLibraryW"});

  for (const auto& attempt : attempts) {
    ::SetLastError(ERROR_SUCCESS);
    HMODULE handle = nullptr;
    if (attempt.uses_ex) {
      handle = ::LoadLibraryExW(load_path_wide.c_str(), nullptr, attempt.flags);
    } else {
      handle = ::LoadLibraryW(load_path_wide.c_str());
    }
    if (handle != nullptr) {
      *module = handle;
      if (!attempts_diagnostics->empty()) {
        attempts_diagnostics->append("; ");
      }
      attempts_diagnostics->append("success=");
      attempts_diagnostics->append(attempt.label);
      return true;
    }

    const DWORD code = ::GetLastError();
    if (!attempts_diagnostics->empty()) {
      attempts_diagnostics->append("; ");
    }
    attempts_diagnostics->append(attempt.label);
    attempts_diagnostics->append(": ");
    attempts_diagnostics->append(FormatWin32ErrorMessage(code));
  }

  if (remove_dll_dir != nullptr) {
    for (const DLL_DIRECTORY_COOKIE cookie : directory_cookies) {
      if (cookie != nullptr) {
        (void)remove_dll_dir(cookie);
      }
    }
  }

  return false;
}
#endif

std::string BuildDiagnostics(const std::string& requested_dll_path,
                             const std::string& attempted_load_path,
                             const std::string& loaded_dll_path,
                             const std::string& runtime_version,
                             std::uint32_t requested_api_version,
                             std::uint32_t negotiated_api_version,
                             const std::string& failure_reason) {
  std::ostringstream oss;
  oss << "requested_path=" << (requested_dll_path.empty() ? "<default>" : requested_dll_path)
      << ", attempted_load_path=" << (attempted_load_path.empty() ? "<none>" : attempted_load_path)
      << ", loaded_path=" << (loaded_dll_path.empty() ? "<none>" : loaded_dll_path)
      << ", requested_api_version=" << requested_api_version
      << ", negotiated_api_version=" << negotiated_api_version
      << ", runtime_version=" << (runtime_version.empty() ? "<unknown>" : runtime_version);
  if (!failure_reason.empty()) {
    oss << ", error=" << failure_reason;
  }
  return oss.str();
}

}  // namespace

OrtDynamicLoader::~OrtDynamicLoader() {
  Unload();
}

bool OrtDynamicLoader::Load(const std::string& dll_path,
                            std::uint32_t requested_api_version,
                            std::string* error) {
  Unload();
  requested_dll_path_ = dll_path;
  requested_api_version_ =
      requested_api_version > 0 ? requested_api_version : static_cast<std::uint32_t>(ORT_API_VERSION);
  negotiated_api_version_ = 0;
  attempted_load_path_.clear();
  loaded_dll_path_.clear();
  runtime_version_string_.clear();
  diagnostics_.clear();

#if !defined(_WIN32)
  return Fail("OrtDynamicLoader is only implemented for Windows in this build", error);
#else
  std::string preloaded_bind_error;
  HMODULE preloaded_module = nullptr;
  std::string preloaded_path;
  std::string preloaded_resolve_error;
  if (TryResolvePreloadedOrtModule(&preloaded_module, &preloaded_path, &preloaded_resolve_error)) {
    const OrtApiBase* preloaded_api_base = nullptr;
    const OrtApi* preloaded_api = nullptr;
    std::uint32_t preloaded_negotiated_version = 0;
    std::string preloaded_runtime_version;
    std::string bind_error;
    if (BindOrtApiFromModule(preloaded_module,
                             requested_api_version_,
                             &preloaded_api_base,
                             &preloaded_api,
                             &preloaded_negotiated_version,
                             &preloaded_runtime_version,
                             &bind_error)) {
      module_handle_ = preloaded_module;
      owns_module_handle_ = false;
      api_base_ = preloaded_api_base;
      api_ = preloaded_api;
      negotiated_api_version_ = preloaded_negotiated_version;
      attempted_load_path_ = preloaded_path;
      loaded_dll_path_ = preloaded_path;
      runtime_version_string_ = preloaded_runtime_version;
      diagnostics_ = BuildDiagnostics(requested_dll_path_,
                                      attempted_load_path_,
                                      loaded_dll_path_,
                                      runtime_version_string_,
                                      requested_api_version_,
                                      negotiated_api_version_,
                                      std::string("using preloaded process module: onnxruntime.dll"));
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }
    preloaded_bind_error = "preloaded onnxruntime.dll found at " + preloaded_path +
                           " but API bind failed: " + bind_error;
  } else {
    preloaded_bind_error = preloaded_resolve_error;
  }

  std::wstring load_path_wide;
  std::string resolve_error;
  if (!ResolveLoadPath(dll_path, &load_path_wide, &attempted_load_path_, &resolve_error)) {
    std::string combined = "failed to resolve ORT DLL path: " + resolve_error;
    if (!preloaded_bind_error.empty()) {
      combined += " | " + preloaded_bind_error;
    }
    return Fail(combined, error);
  }

  HMODULE module = nullptr;
  std::string load_attempts_diagnostics;
  if (!TryLoadOrtModuleWithFallback(load_path_wide, &module, &load_attempts_diagnostics)) {
    const std::string detail = load_attempts_diagnostics.empty() ? "<none>" : load_attempts_diagnostics;
    std::string combined = "LoadLibraryW failed: all attempts exhausted (" + detail + ")";
    if (!preloaded_bind_error.empty()) {
      combined += " | " + preloaded_bind_error;
    }
    return Fail(combined, error);
  }

  const OrtApiBase* api_base = nullptr;
  const OrtApi* api = nullptr;
  std::uint32_t negotiated_api_version = 0;
  std::string bind_error;
  if (!BindOrtApiFromModule(module,
                            requested_api_version_,
                            &api_base,
                            &api,
                            &negotiated_api_version,
                            &runtime_version_string_,
                            &bind_error)) {
    ::FreeLibrary(module);
    return Fail(bind_error, error);
  }

  std::string loaded_path_error;
  if (!QueryLoadedModulePath(module, &loaded_dll_path_, &loaded_path_error)) {
    ::FreeLibrary(module);
    return Fail("failed to query loaded DLL path: " + loaded_path_error, error);
  }

  module_handle_ = module;
  owns_module_handle_ = true;
  api_base_ = api_base;
  api_ = api;
  negotiated_api_version_ = negotiated_api_version;
  diagnostics_ = BuildDiagnostics(requested_dll_path_,
                                  attempted_load_path_,
                                  loaded_dll_path_,
                                  runtime_version_string_,
                                  requested_api_version_,
                                  negotiated_api_version_,
                                  std::string());
  if (error != nullptr) {
    error->clear();
  }
  return true;
#endif
}

void OrtDynamicLoader::Unload() {
#if defined(_WIN32)
  if (module_handle_ != nullptr && owns_module_handle_) {
    ::FreeLibrary(reinterpret_cast<HMODULE>(module_handle_));
  }
#endif
  ResetState();
}

bool OrtDynamicLoader::IsLoaded() const {
  return module_handle_ != nullptr && api_base_ != nullptr && api_ != nullptr;
}

const OrtApiBase* OrtDynamicLoader::ApiBase() const {
  return api_base_;
}

const OrtApi* OrtDynamicLoader::Api() const {
  return api_;
}

std::uint32_t OrtDynamicLoader::RequestedApiVersion() const {
  return requested_api_version_;
}

std::uint32_t OrtDynamicLoader::NegotiatedApiVersion() const {
  return negotiated_api_version_;
}

const std::string& OrtDynamicLoader::RequestedDllPath() const {
  return requested_dll_path_;
}

const std::string& OrtDynamicLoader::AttemptedLoadPath() const {
  return attempted_load_path_;
}

const std::string& OrtDynamicLoader::LoadedDllPath() const {
  return loaded_dll_path_;
}

const std::string& OrtDynamicLoader::RuntimeVersionString() const {
  return runtime_version_string_;
}

const std::string& OrtDynamicLoader::Diagnostics() const {
  return diagnostics_;
}

bool OrtDynamicLoader::Fail(const std::string& reason, std::string* error) {
  diagnostics_ = BuildDiagnostics(requested_dll_path_,
                                  attempted_load_path_,
                                  loaded_dll_path_,
                                  runtime_version_string_,
                                  requested_api_version_,
                                  negotiated_api_version_,
                                  reason);
  if (error != nullptr) {
    *error = diagnostics_;
  }
  return false;
}

void OrtDynamicLoader::ResetState() {
  module_handle_ = nullptr;
  owns_module_handle_ = false;
  api_base_ = nullptr;
  api_ = nullptr;
  negotiated_api_version_ = 0;
  loaded_dll_path_.clear();
  runtime_version_string_.clear();
}

}  // namespace zsoda::inference
