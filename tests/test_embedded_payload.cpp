#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "inference/EmbeddedPayload.h"

namespace {

class TempDir {
 public:
  TempDir() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
    path_ = std::filesystem::temp_directory_path() /
            ("zsoda-embedded-payload-test-" + std::to_string(stamp) + "-" + std::to_string(id));
    std::filesystem::create_directories(path_);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

class ScopedEnvVar {
 public:
  ScopedEnvVar(const char* name, const std::string& value) : name_(name) {
    const char* existing = std::getenv(name_);
    if (existing != nullptr) {
      had_original_ = true;
      original_value_ = existing;
    }
#if defined(_WIN32)
    const int rc = _putenv_s(name_, value.c_str());
#else
    const int rc = setenv(name_, value.c_str(), 1);
#endif
    assert(rc == 0);
  }

  ~ScopedEnvVar() {
#if defined(_WIN32)
    const int rc =
        had_original_ ? _putenv_s(name_, original_value_.c_str()) : _putenv_s(name_, "");
#else
    const int rc =
        had_original_ ? setenv(name_, original_value_.c_str(), 1) : unsetenv(name_);
#endif
    assert(rc == 0);
  }

 private:
  const char* name_;
  bool had_original_ = false;
  std::string original_value_;
};

void WriteBinaryFile(const std::filesystem::path& path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  assert(stream.is_open());
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
  assert(stream.good());
}

void WriteU32LE(std::vector<std::uint8_t>* out, std::uint32_t value) {
  assert(out != nullptr);
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
  }
}

void WriteU64LE(std::vector<std::uint8_t>* out, std::uint64_t value) {
  assert(out != nullptr);
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
  }
}

void WritePaddedMagic(std::vector<std::uint8_t>* out, std::string_view magic) {
  assert(out != nullptr);
  constexpr std::size_t kFieldSize = 16U;
  assert(magic.size() <= kFieldSize);
  const auto start = out->size();
  out->insert(out->end(), magic.begin(), magic.end());
  while (out->size() - start < kFieldSize) {
    out->push_back(0U);
  }
}

std::vector<std::filesystem::path> CollectFiles(const std::filesystem::path& root) {
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (entry.is_regular_file()) {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

void AppendModulePayload(const std::filesystem::path& module_path,
                         const std::vector<std::filesystem::path>& roots) {
  std::vector<std::uint8_t> payload;
  WritePaddedMagic(&payload, "ZSODA_PAYLOAD_V1");
  std::uint32_t entry_count = 0U;
  for (const auto& root : roots) {
    entry_count += static_cast<std::uint32_t>(CollectFiles(root).size());
  }
  WriteU32LE(&payload, entry_count);
  WriteU32LE(&payload, 0U);

  for (const auto& root : roots) {
    const auto files = CollectFiles(root);
    for (const auto& file_path : files) {
      const auto relative = file_path.lexically_relative(root);
      const std::filesystem::path payload_path = root.filename() / relative;
      const std::string payload_text = payload_path.generic_string();
      const std::uint64_t file_size = static_cast<std::uint64_t>(std::filesystem::file_size(file_path));

      WriteU32LE(&payload, static_cast<std::uint32_t>(payload_text.size()));
      WriteU64LE(&payload, file_size);
      payload.insert(payload.end(), payload_text.begin(), payload_text.end());

      std::ifstream input(file_path, std::ios::binary);
      assert(input.is_open());
      std::vector<char> buffer(static_cast<std::size_t>(file_size));
      input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      assert(input.good() || input.eof());
      payload.insert(payload.end(), buffer.begin(), buffer.end());
    }
  }

  std::array<std::uint8_t, 32U> digest = {};
  std::vector<std::uint8_t> footer;
  WritePaddedMagic(&footer, "ZSODA_FOOTER_V1");
  WriteU64LE(&footer, static_cast<std::uint64_t>(payload.size()));
  footer.insert(footer.end(), digest.begin(), digest.end());

  std::ofstream out(module_path, std::ios::binary | std::ios::app);
  assert(out.is_open());
  out.write(reinterpret_cast<const char*>(payload.data()),
            static_cast<std::streamsize>(payload.size()));
  out.write(reinterpret_cast<const char*>(footer.data()),
            static_cast<std::streamsize>(footer.size()));
  assert(out.good());
}

void TestEmbeddedPayloadRoundTripFromSyntheticModule() {
  TempDir temp_dir;
  const auto stage_root = temp_dir.path() / "stage";
  const auto module_path = temp_dir.path() / "ZSoda.aex";
  const auto python_root = stage_root / "zsoda_py";
  const auto models_root = stage_root / "models";

  WriteBinaryFile(module_path, "module-prefix");
  WriteBinaryFile(python_root / "distill_any_depth_remote_service.py",
                  "print('zsoda payload test')\n");
  WriteBinaryFile(models_root / "hf" / "distill-any-depth-base" / "config.json",
                  "{\"kind\":\"stub-model\"}\n");

  AppendModulePayload(module_path, {python_root, models_root});

  std::string error;
  const auto info = zsoda::inference::EnsureEmbeddedPayloadAvailable(module_path.string(), &error);
  assert(error.empty());
  assert(info.has_payload);
  assert(info.extracted);
  assert(!info.asset_root.empty());
  assert(!info.payload_id.empty());

  const auto extracted_root = std::filesystem::path(info.asset_root);
  assert(std::filesystem::exists(extracted_root / ".zsoda_payload_ready"));
  assert(std::filesystem::exists(extracted_root / "zsoda_py" /
                                 "distill_any_depth_remote_service.py"));
  assert(std::filesystem::exists(extracted_root / "models" / "hf" /
                                 "distill-any-depth-base" / "config.json"));
}

#if defined(_WIN32)
void TestEmbeddedPayloadUsesShortWindowsCacheRoot() {
  TempDir temp_dir;
  const auto local_appdata = temp_dir.path() / "LocalAppData";
  const ScopedEnvVar local_appdata_env("LOCALAPPDATA", local_appdata.string());
  const char* user_profile = std::getenv("USERPROFILE");
  assert(user_profile != nullptr && user_profile[0] != '\0');
  const auto short_user_profile = std::filesystem::path(user_profile) / "UP";
  std::error_code cleanup_error;
  std::filesystem::remove_all(short_user_profile, cleanup_error);
  const ScopedEnvVar user_profile_env("USERPROFILE", short_user_profile.string());

  const auto stage_root = temp_dir.path() / "stage";
  const auto module_path = temp_dir.path() / "ZSoda.aex";
  const auto python_root = stage_root / "zsoda_py";
  const auto models_root = stage_root / "models";
  const auto long_python_leaf =
      std::filesystem::path("python/Lib/site-packages/transformers/models/"
                            "audio_spectrogram_transformer/__pycache__/")
      / "feature_extraction_audio_spectrogram_transformer.cpython-312.pyc";

  WriteBinaryFile(module_path, "module-prefix");
  WriteBinaryFile(python_root / "distill_any_depth_remote_service.py",
                  "print('zsoda payload test')\n");
  WriteBinaryFile(python_root / long_python_leaf, "pyc");
  WriteBinaryFile(models_root / "hf" / "distill-any-depth-base" / "config.json",
                  "{\"kind\":\"stub-model\"}\n");

  AppendModulePayload(module_path, {python_root, models_root});

  std::string error;
  const auto info = zsoda::inference::EnsureEmbeddedPayloadAvailable(module_path.string(), &error);
  assert(error.empty());
  assert(info.extracted);
  assert(!info.asset_root.empty());

  const auto extracted_root = std::filesystem::path(info.asset_root);
  assert(extracted_root.parent_path().filename() == "ZS");

  const auto long_extracted_path = extracted_root / "zsoda_py" / long_python_leaf;
  assert(std::filesystem::exists(long_extracted_path));
  assert(long_extracted_path.string().size() < 260U);

  std::filesystem::remove_all(short_user_profile, cleanup_error);
}
#endif

}  // namespace

void RunEmbeddedPayloadTests() {
  TestEmbeddedPayloadRoundTripFromSyntheticModule();
#if defined(_WIN32)
  TestEmbeddedPayloadUsesShortWindowsCacheRoot();
#endif
}
