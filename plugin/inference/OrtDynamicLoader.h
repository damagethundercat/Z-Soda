#pragma once

#include <cstdint>
#include <string>

struct OrtApi;
struct OrtApiBase;

namespace zsoda::inference {

class OrtDynamicLoader {
 public:
  OrtDynamicLoader() = default;
  ~OrtDynamicLoader();

  OrtDynamicLoader(const OrtDynamicLoader&) = delete;
  OrtDynamicLoader& operator=(const OrtDynamicLoader&) = delete;
  OrtDynamicLoader(OrtDynamicLoader&&) = delete;
  OrtDynamicLoader& operator=(OrtDynamicLoader&&) = delete;

  bool Load(const std::string& dll_path,
            std::uint32_t requested_api_version,
            std::string* error,
            bool prefer_preloaded = false);
  void Unload();

  [[nodiscard]] bool IsLoaded() const;
  [[nodiscard]] const OrtApiBase* ApiBase() const;
  [[nodiscard]] const OrtApi* Api() const;
  [[nodiscard]] std::uint32_t RequestedApiVersion() const;
  [[nodiscard]] std::uint32_t NegotiatedApiVersion() const;
  [[nodiscard]] const std::string& RequestedDllPath() const;
  [[nodiscard]] const std::string& AttemptedLoadPath() const;
  [[nodiscard]] const std::string& LoadedDllPath() const;
  [[nodiscard]] const std::string& RuntimeVersionString() const;
  [[nodiscard]] const std::string& Diagnostics() const;
  [[nodiscard]] void* NativeModuleHandle() const;

 private:
  bool Fail(const std::string& reason, std::string* error);
  void ResetState();

  void* module_handle_ = nullptr;
  bool owns_module_handle_ = false;
  const OrtApiBase* api_base_ = nullptr;
  const OrtApi* api_ = nullptr;
  std::uint32_t requested_api_version_ = 0;
  std::uint32_t negotiated_api_version_ = 0;
  std::string requested_dll_path_;
  std::string attempted_load_path_;
  std::string loaded_dll_path_;
  std::string runtime_version_string_;
  std::string diagnostics_;
};

}  // namespace zsoda::inference
