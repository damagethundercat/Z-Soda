#include "inference/OrtDynamicLoader.h"

#include <cstdint>
#include <sstream>
#include <string>
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
    return "Win32 error " + std::to_string(error_code);
  }

  std::string message(buffer, buffer + length);
  ::LocalFree(buffer);
  return TrimTrailingWhitespace(message);
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
  std::wstring load_path_wide;
  std::string resolve_error;
  if (!ResolveLoadPath(dll_path, &load_path_wide, &attempted_load_path_, &resolve_error)) {
    return Fail("failed to resolve ORT DLL path: " + resolve_error, error);
  }

  HMODULE module = ::LoadLibraryExW(load_path_wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (module == nullptr) {
    const std::string win_error = FormatWin32ErrorMessage(::GetLastError());
    return Fail("LoadLibraryW failed: " + win_error, error);
  }

  FARPROC symbol = ::GetProcAddress(module, "OrtGetApiBase");
  if (symbol == nullptr) {
    const std::string win_error = FormatWin32ErrorMessage(::GetLastError());
    ::FreeLibrary(module);
    return Fail("GetProcAddress(\"OrtGetApiBase\") failed: " + win_error, error);
  }

  using OrtGetApiBaseFn = const OrtApiBase* (ORT_API_CALL*)(void);
  const auto get_api_base = reinterpret_cast<OrtGetApiBaseFn>(symbol);
  const OrtApiBase* api_base = get_api_base();
  if (api_base == nullptr) {
    ::FreeLibrary(module);
    return Fail("OrtGetApiBase returned null", error);
  }

  const auto* api_base_compat = reinterpret_cast<const OrtApiBaseCompat*>(api_base);
  if (api_base_compat->GetApi == nullptr) {
    ::FreeLibrary(module);
    return Fail("OrtApiBase::GetApi is null", error);
  }

  const OrtApi* api = api_base_compat->GetApi(requested_api_version_);
  if (api == nullptr) {
    ::FreeLibrary(module);
    return Fail("OrtApiBase::GetApi(ORT_API_VERSION) returned null", error);
  }

  if (api_base_compat->GetVersionString != nullptr) {
    const char* version = api_base_compat->GetVersionString();
    if (version != nullptr) {
      runtime_version_string_ = version;
    }
  }

  std::string loaded_path_error;
  if (!QueryLoadedModulePath(module, &loaded_dll_path_, &loaded_path_error)) {
    ::FreeLibrary(module);
    return Fail("failed to query loaded DLL path: " + loaded_path_error, error);
  }

  module_handle_ = module;
  api_base_ = api_base;
  api_ = api;
  negotiated_api_version_ = requested_api_version_;
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
  if (module_handle_ != nullptr) {
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
  api_base_ = nullptr;
  api_ = nullptr;
  negotiated_api_version_ = 0;
  loaded_dll_path_.clear();
  runtime_version_string_.clear();
}

}  // namespace zsoda::inference
