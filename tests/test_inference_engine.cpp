#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Frame.h"
#include "inference/InferenceEngine.h"
#include "inference/ModelAutoDownloader.h"
#include "inference/ManagedInferenceEngine.h"
#include "inference/ModelCatalog.h"
#include "inference/RemoteInferenceBackend.h"
#include "inference/RuntimePathResolver.h"
#if defined(ZSODA_WITH_ONNX_RUNTIME)
#include "inference/OnnxRuntimeBackend.h"
#endif
#include "inference/RuntimeOptions.h"

#if defined(ZSODA_WITH_ONNX_RUNTIME)
#define CreateOnnxRuntimeBackend CreateOnnxRuntimeBackend_TestOnly
#include "inference/OnnxRuntimeBackend.cpp"
#undef CreateOnnxRuntimeBackend
#endif

namespace {

class TempDir {
 public:
  TempDir() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
    path_ = std::filesystem::temp_directory_path() /
            ("zsoda-inference-test-" + std::to_string(stamp) + "-" + std::to_string(id));
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

void WriteTextFile(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path);
  assert(stream.is_open());
  stream << content;
}

template <std::size_t Size>
void WriteBinaryFile(const std::filesystem::path& path,
                     const std::array<std::uint8_t, Size>& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary);
  assert(stream.is_open());
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  assert(stream.good());
}

bool HasModel(const std::vector<std::string>& model_ids, const std::string& expected) {
  for (const auto& model_id : model_ids) {
    if (model_id == expected) {
      return true;
    }
  }
  return false;
}

bool Contains(const std::string& source, const std::string& needle) {
  return source.find(needle) != std::string::npos;
}

bool ContainsAny(std::string_view source, std::initializer_list<std::string_view> needles) {
  for (const auto needle : needles) {
    if (!needle.empty() && source.find(needle) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

bool HasOrtLoadOrVersionMismatchDiagnostic(std::string_view message) {
  return ContainsAny(message,
                     {
                         "version mismatch",
                         "api version mismatch",
                         "failed to load",
                         "load failed",
                         "shared library",
                         "dynamic library",
                         "undefined symbol",
                         "symbol lookup",
                         "dlopen",
                         "dlsym",
                         "LoadLibrary",
                     });
}

bool HasOrtInitializationDiagnostic(std::string_view message) {
  return ContainsAny(message,
                     {
                         "onnx runtime backend initialization failed:",
                         "onnx runtime initialize failed:",
                         "onnx runtime backend is unavailable",
                         "onnx runtime backend is disabled at build time",
                     }) ||
         HasOrtLoadOrVersionMismatchDiagnostic(message);
}

bool HasOrtBootstrapDiagnostic(std::string_view message) {
  return HasOrtInitializationDiagnostic(message) ||
         ContainsAny(message,
                     {
                         "onnx runtime dynamic loader failed",
                         "requested_api_version=",
                         "runtime_version=",
                         "candidates=",
                         "loaded_path=",
                     });
}

bool HasOrtRunOrExecutionDiagnostic(std::string_view message) {
  return ContainsAny(message,
                     {
                         "onnx runtime session create failed:",
                         "onnx runtime run failed:",
                         "onnx runtime backend has no active session",
                         "execution is not available in this build",
                         "onnx runtime backend run failed",
                     }) ||
         HasOrtLoadOrVersionMismatchDiagnostic(message);
}

bool SetEnvironmentValue(const std::string& name, const std::string& value) {
#if defined(_WIN32)
  return _putenv_s(name.c_str(), value.c_str()) == 0;
#else
  return setenv(name.c_str(), value.c_str(), 1) == 0;
#endif
}

bool UnsetEnvironmentValue(const std::string& name) {
#if defined(_WIN32)
  return _putenv_s(name.c_str(), "") == 0;
#else
  return unsetenv(name.c_str()) == 0;
#endif
}

class ScopedEnvironmentOverride {
 public:
  ScopedEnvironmentOverride(std::string key, std::string value) : key_(std::move(key)) {
    const char* existing = std::getenv(key_.c_str());
    if (existing != nullptr) {
      had_existing_value_ = true;
      existing_value_ = existing;
    }
    assert(SetEnvironmentValue(key_, value));
  }

  ~ScopedEnvironmentOverride() {
    if (had_existing_value_) {
      assert(SetEnvironmentValue(key_, existing_value_));
    } else {
      assert(UnsetEnvironmentValue(key_));
    }
  }

 private:
  std::string key_;
  bool had_existing_value_ = false;
  std::string existing_value_;
};

std::string FailingRemoteCommandTemplate() {
#if defined(_WIN32)
  return "cmd /c \"exit /b 17\"";
#else
  return "sh -c \"exit 17\"";
#endif
}

zsoda::core::FrameBuffer MakeSource() {
  zsoda::core::FrameDesc desc;
  desc.width = 4;
  desc.height = 4;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;
  zsoda::core::FrameBuffer frame(desc);
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      frame.at(x, y, 0) = static_cast<float>(x + y);
    }
  }
  return frame;
}

std::optional<std::filesystem::path> FindRepoRoot() {
  std::error_code ec;
  auto current = std::filesystem::current_path(ec);
  if (ec) {
    return std::nullopt;
  }

  for (;;) {
    if (std::filesystem::is_regular_file(current / "plugin" / "CMakeLists.txt", ec) &&
        !ec &&
        std::filesystem::is_directory(current / "tests", ec) &&
        !ec) {
      return current;
    }
    const auto parent = current.parent_path();
    if (parent.empty() || parent == current) {
      break;
    }
    current = parent;
  }
  return std::nullopt;
}

#if defined(ZSODA_WITH_ONNX_RUNTIME)
constexpr std::array<std::uint8_t, 132> kMinimalFloatIdentityOnnx = {
    0x08, 0x08, 0x12, 0x0a, 0x7a, 0x73, 0x6f, 0x64, 0x61, 0x2d, 0x74, 0x65, 0x73, 0x74,
    0x3a, 0x6e, 0x0a, 0x19, 0x0a, 0x05, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x12, 0x06, 0x6f,
    0x75, 0x74, 0x70, 0x75, 0x74, 0x22, 0x08, 0x49, 0x64, 0x65, 0x6e, 0x74, 0x69, 0x74,
    0x79, 0x12, 0x0e, 0x7a, 0x73, 0x6f, 0x64, 0x61, 0x5f, 0x69, 0x64, 0x65, 0x6e, 0x74,
    0x69, 0x74, 0x79, 0x5a, 0x1f, 0x0a, 0x05, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x12, 0x16,
    0x0a, 0x14, 0x08, 0x01, 0x12, 0x10, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x03,
    0x0a, 0x02, 0x08, 0x02, 0x0a, 0x02, 0x08, 0x02, 0x62, 0x20, 0x0a, 0x06, 0x6f, 0x75,
    0x74, 0x70, 0x75, 0x74, 0x12, 0x16, 0x0a, 0x14, 0x08, 0x01, 0x12, 0x10, 0x0a, 0x02,
    0x08, 0x01, 0x0a, 0x02, 0x08, 0x03, 0x0a, 0x02, 0x08, 0x02, 0x0a, 0x02, 0x08, 0x02,
    0x42, 0x04, 0x0a, 0x00, 0x10, 0x11,
};

std::optional<std::filesystem::path> FindTestOrtRuntimeLibrary() {
  const char* configured = std::getenv("ZSODA_ONNXRUNTIME_LIBRARY");
  if (configured != nullptr && configured[0] != '\0') {
    const std::filesystem::path configured_path(configured);
    std::error_code ec;
    if (std::filesystem::is_regular_file(configured_path, ec) && !ec) {
      return configured_path;
    }
  }

  const auto repo_root = FindRepoRoot();
  if (!repo_root.has_value()) {
    return std::nullopt;
  }

  const std::vector<std::filesystem::path> search_roots = {
      *repo_root / ".cache" / "ort-sdk",
      *repo_root / "dist-mac-ort",
      *repo_root / "build-mac-ort",
  };
  const std::string library_name = zsoda::inference::DefaultOnnxRuntimeLibraryFileName();
  for (const auto& search_root : search_roots) {
    std::error_code ec;
    if (!std::filesystem::exists(search_root, ec) || ec) {
      continue;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(search_root, ec)) {
      if (ec) {
        break;
      }
      if (entry.is_regular_file(ec) && !ec && entry.path().filename() == library_name) {
        return entry.path();
      }
    }
  }

  return std::nullopt;
}

zsoda::core::FrameBuffer MakeRgbCenteredSquareSource(int width, int height, int square_size) {
  zsoda::core::FrameDesc desc;
  desc.width = width;
  desc.height = height;
  desc.channels = 3;
  desc.format = zsoda::core::PixelFormat::kRGBA32F;
  zsoda::core::FrameBuffer frame(desc);

  const int clamped_square = std::clamp(square_size, 1, std::min(width, height));
  const int start_x = (width - clamped_square) / 2;
  const int start_y = (height - clamped_square) / 2;
  const int end_x = start_x + clamped_square;
  const int end_y = start_y + clamped_square;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float value = (x >= start_x && x < end_x && y >= start_y && y < end_y) ? 1.0F : 0.0F;
      for (int channel = 0; channel < 3; ++channel) {
        frame.at(x, y, channel) = value;
      }
    }
  }
  return frame;
}

float MeasureChannelSpreadAspectRatio(const zsoda::inference::PreparedModelInput& prepared,
                                      int channel) {
  const int width = prepared.tensor_width;
  const int height = prepared.tensor_height;
  const std::size_t plane_size =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  const std::size_t channel_offset = static_cast<std::size_t>(channel) * plane_size;
  if (width <= 0 || height <= 0 || channel < 0 || channel >= prepared.tensor_channels ||
      prepared.nchw_values.size() < channel_offset + plane_size) {
    return 0.0F;
  }

  float sum_weight = 0.0F;
  float mean_x = 0.0F;
  float mean_y = 0.0F;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t index =
          channel_offset + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + x;
      const float weight = std::max(0.0F, prepared.nchw_values[index]);
      sum_weight += weight;
      mean_x += weight * static_cast<float>(x);
      mean_y += weight * static_cast<float>(y);
    }
  }
  if (sum_weight <= 1e-6F) {
    return 0.0F;
  }
  mean_x /= sum_weight;
  mean_y /= sum_weight;

  float var_x = 0.0F;
  float var_y = 0.0F;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t index =
          channel_offset + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + x;
      const float weight = std::max(0.0F, prepared.nchw_values[index]);
      const float dx = static_cast<float>(x) - mean_x;
      const float dy = static_cast<float>(y) - mean_y;
      var_x += weight * dx * dx;
      var_y += weight * dy * dy;
    }
  }
  if (var_x <= 1e-6F || var_y <= 1e-6F) {
    return 0.0F;
  }
  return std::sqrt(var_x / var_y);
}

#endif

void TestModelList() {
  zsoda::inference::ManagedInferenceEngine engine("models");
  const auto models = engine.ListModelIds();
  assert(!models.empty());
  assert(HasModel(models, "distill-any-depth"));
  assert(HasModel(models, "distill-any-depth-base"));
  assert(HasModel(models, "distill-any-depth-large"));
}

void TestModelSelection() {
  zsoda::inference::ManagedInferenceEngine engine("models");
  std::string error;
  assert(engine.Initialize("distill-any-depth-base", &error));
  assert(engine.ActiveModelId() == "distill-any-depth-base");

  error.clear();
  assert(!engine.SelectModel("unknown-model", &error));
  assert(!error.empty());

  error.clear();
  assert(engine.SelectModel("distill-any-depth-large", &error));
  assert(engine.ActiveModelId() == "distill-any-depth-large");
}

void TestRuntimeBackendOptions() {
  assert(zsoda::inference::ParseRuntimeBackend("auto") ==
         zsoda::inference::RuntimeBackend::kAuto);
  assert(zsoda::inference::ParseRuntimeBackend("cpu") ==
         zsoda::inference::RuntimeBackend::kCpu);
  assert(zsoda::inference::ParseRuntimeBackend("CUDA") ==
         zsoda::inference::RuntimeBackend::kCuda);
  assert(zsoda::inference::ParseRuntimeBackend("TensorRT") ==
         zsoda::inference::RuntimeBackend::kTensorRT);
  assert(zsoda::inference::ParseRuntimeBackend("trt") ==
         zsoda::inference::RuntimeBackend::kTensorRT);
  assert(zsoda::inference::ParseRuntimeBackend("direct-ml") ==
         zsoda::inference::RuntimeBackend::kDirectML);
  assert(zsoda::inference::ParseRuntimeBackend("core_ml") ==
         zsoda::inference::RuntimeBackend::kCoreML);
  assert(zsoda::inference::ParseRuntimeBackend("remote") ==
         zsoda::inference::RuntimeBackend::kRemote);
  assert(zsoda::inference::ParseRuntimeBackend("unknown-backend") ==
         zsoda::inference::RuntimeBackend::kAuto);
  assert(zsoda::inference::ParsePreprocessResizeMode("upper_bound_letterbox") ==
         zsoda::inference::PreprocessResizeMode::kUpperBoundLetterbox);
  assert(zsoda::inference::ParsePreprocessResizeMode("letterbox") ==
         zsoda::inference::PreprocessResizeMode::kUpperBoundLetterbox);
  assert(zsoda::inference::ParsePreprocessResizeMode("lower_bound_center_crop") ==
         zsoda::inference::PreprocessResizeMode::kLowerBoundCenterCrop);
  assert(zsoda::inference::ParsePreprocessResizeMode("lower_bound") ==
         zsoda::inference::PreprocessResizeMode::kLowerBoundCenterCrop);
  assert(zsoda::inference::ParsePreprocessResizeMode("crop") ==
         zsoda::inference::PreprocessResizeMode::kLowerBoundCenterCrop);
  assert(zsoda::inference::ParsePreprocessResizeMode("stretch") ==
         zsoda::inference::PreprocessResizeMode::kStretch);
  assert(zsoda::inference::ParseRemoteTransportProtocol("binary") ==
         zsoda::inference::RemoteTransportProtocol::kBinary);
  assert(zsoda::inference::ParseRemoteTransportProtocol("json") ==
         zsoda::inference::RemoteTransportProtocol::kJson);
  assert(zsoda::inference::ParseRemoteTransportProtocol("legacy_json") ==
         zsoda::inference::RemoteTransportProtocol::kJson);

  zsoda::inference::RuntimeOptions options;
  options.preferred_backend = zsoda::inference::RuntimeBackend::kCuda;
  assert(options.remote_transport_protocol == zsoda::inference::RemoteTransportProtocol::kBinary);
  zsoda::inference::ManagedInferenceEngine engine("models", options);
  assert(engine.RequestedBackend() == zsoda::inference::RuntimeBackend::kCuda);
#if defined(ZSODA_WITH_ONNX_RUNTIME)
  const auto status = engine.BackendStatus();
  assert(status.requested_backend == zsoda::inference::RuntimeBackend::kCuda);
  assert(status.active_backend == engine.ActiveBackend());
  assert(status.using_fallback_engine == engine.UsingFallbackEngine());
  if (engine.UsingFallbackEngine()) {
    assert(engine.ActiveBackend() == zsoda::inference::RuntimeBackend::kCpu);
    assert(!status.fallback_reason.empty());
    assert(Contains(status.fallback_reason, "onnx runtime"));
    assert(HasOrtInitializationDiagnostic(status.fallback_reason));
  } else {
#if defined(ZSODA_WITH_ONNX_RUNTIME_API)
    const auto active_backend = engine.ActiveBackend();
    const bool valid_active_backend = active_backend == zsoda::inference::RuntimeBackend::kCpu ||
                                      active_backend == zsoda::inference::RuntimeBackend::kCuda ||
                                      active_backend == zsoda::inference::RuntimeBackend::kTensorRT ||
                                      active_backend == zsoda::inference::RuntimeBackend::kDirectML ||
                                      active_backend == zsoda::inference::RuntimeBackend::kCoreML;
    assert(valid_active_backend);
#else
    const auto active_backend = engine.ActiveBackend();
    assert(active_backend == zsoda::inference::RuntimeBackend::kCpu ||
           active_backend == zsoda::inference::RuntimeBackend::kCuda);
#endif
  }
#else
  assert(engine.UsingFallbackEngine());
  assert(engine.ActiveBackend() == zsoda::inference::RuntimeBackend::kCpu);
#endif
}

void TestRemoteBackendCommandValidation() {
  zsoda::inference::RuntimeOptions options;
  options.preferred_backend = zsoda::inference::RuntimeBackend::kRemote;

  ScopedEnvironmentOverride clear_remote_command("ZSODA_REMOTE_INFERENCE_COMMAND", "");
  ScopedEnvironmentOverride clear_legacy_remote_command("ZSODA_REMOTE_BACKEND_COMMAND", "");

  std::string error;
  auto unconfigured_backend = zsoda::inference::CreateRemoteInferenceBackendWithCommand(
      options, {.command_template = ""}, &error);
  assert(unconfigured_backend == nullptr);
  assert(Contains(error, "remote inference command is not configured"));

  error.clear();
  auto failing_backend = zsoda::inference::CreateRemoteInferenceBackendWithCommand(
      options, {.command_template = FailingRemoteCommandTemplate()}, &error);
  assert(failing_backend != nullptr);
  assert(error.empty());
  assert(failing_backend->ActiveBackend() == zsoda::inference::RuntimeBackend::kRemote);
  assert(Contains(failing_backend->Name(), "RemoteInferenceBackend"));

  error.clear();
  assert(failing_backend->SelectModel("remote-test-model", "remote-test-model.onnx", &error));
  assert(error.empty());

  const auto source = MakeSource();
  zsoda::inference::InferenceRequest request;
  request.source = &source;
  request.quality = 1;

  zsoda::core::FrameBuffer output;
  error.clear();
  assert(!failing_backend->Run(request, &output, &error));
  assert(ContainsAny(error,
                     {
                         "remote command failed",
                         "failed to invoke remote command",
                     }));
}

void TestRemoteBackendSafeFallbackWithManagedEngine() {
  TempDir temp_dir;
  const auto manifest = temp_dir.path() / zsoda::inference::ModelCatalog::DefaultManifestFilename();
  WriteTextFile(
      manifest,
      "# id|display_name|relative_path|download_url|preferred_default\n"
      "remote-safe-v1|Remote Safe v1|remote/remote_safe_v1.onnx|https://example.com/remote_safe_v1.onnx|true\n");
  const auto model_path = temp_dir.path() / "remote/remote_safe_v1.onnx";
  WriteTextFile(model_path, "dummy");

  ScopedEnvironmentOverride force_remote_command("ZSODA_REMOTE_INFERENCE_COMMAND",
                                                 FailingRemoteCommandTemplate());
  ScopedEnvironmentOverride clear_legacy_remote_command("ZSODA_REMOTE_BACKEND_COMMAND", "");

  zsoda::inference::RuntimeOptions options;
  options.preferred_backend = zsoda::inference::RuntimeBackend::kRemote;
  options.remote_inference_enabled = true;
  zsoda::inference::ManagedInferenceEngine engine(temp_dir.path().string(), options);

  assert(engine.RequestedBackend() == zsoda::inference::RuntimeBackend::kRemote);
  const auto initial_status = engine.BackendStatus();
  assert(initial_status.requested_backend == zsoda::inference::RuntimeBackend::kRemote);
  assert(initial_status.active_backend == zsoda::inference::RuntimeBackend::kRemote);
  assert(!initial_status.using_fallback_engine);
  assert(Contains(engine.BackendStatusString(), "requested=remote"));

  std::string error;
  assert(engine.Initialize("remote-safe-v1", &error));
  assert(error.empty());

  const auto source = MakeSource();
  zsoda::inference::InferenceRequest request;
  request.source = &source;
  request.quality = 1;

  zsoda::core::FrameBuffer output;
  assert(!engine.Run(request, &output, &error));
  assert(!error.empty());
  assert(Contains(error, "remote command failed"));

  const auto after = engine.BackendStatus();
  assert(after.requested_backend == zsoda::inference::RuntimeBackend::kRemote);
  assert(after.active_backend == zsoda::inference::RuntimeBackend::kRemote);
  assert(after.last_run_used_fallback);
  assert(!after.fallback_reason.empty());
  assert(Contains(after.fallback_reason, "remote command failed"));
  assert(Contains(after.engine_name, "BackendUnavailable"));
  assert(Contains(engine.BackendStatusString(), "last_run_fallback=true"));
}

void TestRemoteBackendEndpointInitialization() {
  zsoda::inference::RuntimeOptions options;
  options.preferred_backend = zsoda::inference::RuntimeBackend::kRemote;
  options.remote_inference_enabled = true;
  options.remote_endpoint = "http://127.0.0.1:8345/zsoda/depth";

  ScopedEnvironmentOverride clear_remote_command("ZSODA_REMOTE_INFERENCE_COMMAND", "");
  ScopedEnvironmentOverride clear_legacy_remote_command("ZSODA_REMOTE_BACKEND_COMMAND", "");

  std::string error;
  auto backend = zsoda::inference::CreateRemoteInferenceBackendWithCommand(
      options, {.command_template = ""}, &error);
  assert(backend != nullptr);
  assert(error.empty());
  assert(backend->ActiveBackend() == zsoda::inference::RuntimeBackend::kRemote);
  assert(Contains(backend->Name(), "RemoteInferenceBackend"));
}

void TestCreateDefaultEngineKeepsAutoBackendWhenRemoteFallbackIsEnabled() {
  TempDir temp_dir;
  const auto model_path =
      temp_dir.path() / "distill-any-depth" / "distill_any_depth_base.onnx";
  const auto ort_runtime_path = temp_dir.path() / "zsoda_ort" / "onnxruntime.dll";
  WriteTextFile(model_path, "dummy");
  WriteTextFile(ort_runtime_path, "dummy");

  ScopedEnvironmentOverride clear_backend("ZSODA_INFERENCE_BACKEND", "");
  ScopedEnvironmentOverride set_model_root("ZSODA_MODEL_ROOT", temp_dir.path().string());
  ScopedEnvironmentOverride clear_model_manifest("ZSODA_MODEL_MANIFEST", "");
  ScopedEnvironmentOverride set_ort_runtime("ZSODA_ONNXRUNTIME_LIBRARY", ort_runtime_path.string());
  ScopedEnvironmentOverride set_remote_enabled("ZSODA_REMOTE_INFERENCE_ENABLED", "1");
  ScopedEnvironmentOverride set_remote_endpoint("ZSODA_REMOTE_INFERENCE_ENDPOINT",
                                                "http://127.0.0.1:8345/zsoda/depth");
  ScopedEnvironmentOverride clear_remote_autostart("ZSODA_REMOTE_SERVICE_AUTOSTART", "0");
  ScopedEnvironmentOverride clear_remote_script("ZSODA_REMOTE_SERVICE_SCRIPT", "");
  ScopedEnvironmentOverride clear_remote_python("ZSODA_REMOTE_SERVICE_PYTHON", "");

  auto engine = zsoda::inference::CreateDefaultEngine();
  auto managed = std::dynamic_pointer_cast<zsoda::inference::ManagedInferenceEngine>(engine);
  assert(managed != nullptr);
  assert(managed->RequestedBackend() == zsoda::inference::RuntimeBackend::kAuto);

  const auto status = managed->BackendStatus();
  assert(status.requested_backend == zsoda::inference::RuntimeBackend::kAuto);
}

void TestRemoteFallbackDoesNotSuppressMissingLocalModelDiagnostics() {
  TempDir temp_dir;
  const auto manifest = temp_dir.path() / zsoda::inference::ModelCatalog::DefaultManifestFilename();
  const auto missing_model_path = temp_dir.path() / "missing" / "missing_depth_v2.onnx";
  const auto missing_ort_path = temp_dir.path() / "runtime" / "missing-onnxruntime.dll";
  WriteTextFile(
      manifest,
      "# id|display_name|relative_path|download_url|preferred_default\n"
      "missing-depth-v2|Missing Depth v2|missing/missing_depth_v2.onnx|https://example.com/missing_depth_v2.onnx|true\n");

  zsoda::inference::RuntimeOptions options;
  options.preferred_backend = zsoda::inference::RuntimeBackend::kAuto;
  options.remote_inference_enabled = true;
  options.remote_endpoint = "http://127.0.0.1:8345/zsoda/depth";
  options.onnxruntime_library_path = missing_ort_path.string();

  zsoda::inference::ManagedInferenceEngine engine(temp_dir.path().string(), options);
  std::string error;
  assert(engine.Initialize("missing-depth-v2", &error));
  assert(Contains(error, "model asset file not found:"));
  assert(Contains(error, "missing_depth_v2.onnx"));
}

void TestExplicitRemoteBackendStillIgnoresMissingLocalModelAssets() {
  TempDir temp_dir;
  const auto manifest = temp_dir.path() / zsoda::inference::ModelCatalog::DefaultManifestFilename();
  WriteTextFile(
      manifest,
      "# id|display_name|relative_path|download_url|preferred_default\n"
      "missing-remote-v1|Missing Remote v1|missing/missing_remote_v1.onnx|https://example.com/missing_remote_v1.onnx|true\n");

  zsoda::inference::RuntimeOptions options;
  options.preferred_backend = zsoda::inference::RuntimeBackend::kRemote;
  options.remote_inference_enabled = true;
  options.remote_endpoint = "http://127.0.0.1:8345/zsoda/depth";

  zsoda::inference::ManagedInferenceEngine engine(temp_dir.path().string(), options);
  std::string error;
  assert(engine.Initialize("missing-remote-v1", &error));
  assert(error.empty());

  const auto status = engine.BackendStatus();
  assert(status.requested_backend == zsoda::inference::RuntimeBackend::kRemote);
  assert(status.active_backend == zsoda::inference::RuntimeBackend::kRemote);
}

void TestRunRequiresSelectedModel() {
  zsoda::inference::ManagedInferenceEngine engine("models");
  const auto source = MakeSource();

  zsoda::inference::InferenceRequest request;
  request.source = &source;
  request.quality = 1;

  zsoda::core::FrameBuffer output;
  std::string error;
  assert(!engine.Run(request, &output, &error));
  assert(error == "model is not selected");
}

void TestBackendStatusDiagnostics() {
  TempDir temp_dir;
  const auto manifest = temp_dir.path() / zsoda::inference::ModelCatalog::DefaultManifestFilename();
  WriteTextFile(
      manifest,
      "# id|display_name|relative_path|download_url|preferred_default\n"
      "status-depth-v1|Status Depth v1|status/status_depth_v1.onnx|https://example.com/status_depth_v1.onnx|true\n");
  const auto model_path = temp_dir.path() / "status/status_depth_v1.onnx";
  WriteTextFile(model_path, "dummy");

  zsoda::inference::RuntimeOptions options;
  options.preferred_backend = zsoda::inference::RuntimeBackend::kCuda;
  zsoda::inference::ManagedInferenceEngine engine(temp_dir.path().string(), options);

  const auto initial = engine.BackendStatus();
  assert(initial.requested_backend == zsoda::inference::RuntimeBackend::kCuda);
  assert(initial.active_backend == engine.ActiveBackend());
  assert(initial.using_fallback_engine == engine.UsingFallbackEngine());
  assert(!initial.active_backend_name.empty());
  assert(!initial.engine_name.empty());

  const std::string initial_status = engine.BackendStatusString();
  assert(Contains(initial_status, "requested="));
  assert(Contains(initial_status, "active="));
  assert(Contains(initial_status, "engine="));
  assert(Contains(initial_status, "configured_fallback="));
  assert(Contains(initial_status, "last_run_fallback="));
  assert(Contains(initial_status, "fallback_reason="));

  std::string error;
  assert(engine.Initialize("status-depth-v1", &error));
  const auto source = MakeSource();

  zsoda::inference::InferenceRequest request;
  request.source = &source;
  request.quality = 1;

  zsoda::core::FrameBuffer output;
  assert(!engine.Run(request, &output, &error));

  const auto after = engine.BackendStatus();
  assert(after.last_run_used_fallback);
  assert(!after.fallback_reason.empty());
#if defined(ZSODA_WITH_ONNX_RUNTIME)
  assert(Contains(after.engine_name, "BackendUnavailable"));
  const bool has_ort_backend_context = Contains(after.engine_name, "requested=OnnxRuntimeBackend");
  if (has_ort_backend_context) {
    assert(HasOrtRunOrExecutionDiagnostic(after.fallback_reason) ||
           Contains(after.fallback_reason, "model path does not exist:"));
  } else {
    assert(after.using_fallback_engine);
    assert(Contains(after.fallback_reason, "onnx runtime"));
    assert(HasOrtInitializationDiagnostic(after.fallback_reason));
  }
#else
  assert(after.engine_name == "BackendUnavailable");
#endif
  assert(Contains(engine.BackendStatusString(), "last_run_fallback=true"));
  assert(Contains(engine.BackendStatusString(), "fallback_reason="));
}

void TestManifestLoadingAndDefaults() {
  TempDir temp_dir;
  const auto manifest = temp_dir.path() / zsoda::inference::ModelCatalog::DefaultManifestFilename();
  WriteTextFile(
      manifest,
      "# id|display_name|relative_path|download_url|preferred_default|auxiliary_assets\n"
      "custom-depth-v1|Custom Depth v1|custom/custom_depth_v1.onnx|https://example.com/custom_depth_v1.onnx|true\n"
      "distill-any-depth-base|Distill Any Depth Base (Manifest)|distill-any-depth/base_override.onnx|https://example.com/base_override.onnx|false\n");

  zsoda::inference::ManagedInferenceEngine engine(temp_dir.path().string());
  const auto ids = engine.ListModelIds();
  assert(HasModel(ids, "custom-depth-v1"));
  assert(HasModel(ids, "distill-any-depth-base"));

  std::string error;
  assert(engine.Initialize("", &error));
  assert(engine.ActiveModelId() == "custom-depth-v1");
}

void TestManifestLoadValidation() {
  TempDir temp_dir;
  const auto manifest = temp_dir.path() / "broken.manifest";
  WriteTextFile(manifest, "invalid|entry|missing-url\n");

  zsoda::inference::ModelCatalog catalog;
  std::string error;
  zsoda::inference::ManifestLoadResult result;
  assert(!catalog.LoadManifestFile(manifest.string(), &error, &result));
  assert(!error.empty());
}

void TestManifestAuxiliaryAssetsParsing() {
  TempDir temp_dir;
  const auto manifest = temp_dir.path() / "custom.manifest";
  WriteTextFile(
      manifest,
      "# id|display_name|relative_path|download_url|preferred_default|auxiliary_assets\n"
      "aux-depth-v1|Aux Depth v1|aux/model.onnx|https://example.com/model.onnx|true|aux/model.onnx_data::https://example.com/model.onnx_data\n");

  zsoda::inference::ModelCatalog catalog;
  std::string error;
  zsoda::inference::ManifestLoadResult result;
  assert(catalog.LoadManifestFile(manifest.string(), &error, &result));
  assert(error.empty());
  assert(result.added == 1);

  const auto assets = catalog.ResolveModelAssets(temp_dir.path().string(), "aux-depth-v1");
  assert(assets.size() == 2);
  assert(assets[0].relative_path == "aux/model.onnx");
  assert(assets[1].relative_path == "aux/model.onnx_data");
  assert(Contains(assets[0].absolute_path, "aux/model.onnx"));
  assert(Contains(assets[1].absolute_path, "aux/model.onnx_data"));
}

void TestManifestModelSelectionRunPath() {
  TempDir temp_dir;
  const auto manifest = temp_dir.path() / zsoda::inference::ModelCatalog::DefaultManifestFilename();
  WriteTextFile(
      manifest,
      "# id|display_name|relative_path|download_url|preferred_default\n"
      "custom-depth-v2|Custom Depth v2|custom/custom_depth_v2.onnx|https://example.com/custom_depth_v2.onnx|true\n");
  const auto model_path = temp_dir.path() / "custom/custom_depth_v2.onnx";
  WriteTextFile(model_path, "dummy");

  zsoda::inference::ManagedInferenceEngine engine(temp_dir.path().string());
  std::string error;
  assert(engine.Initialize("custom-depth-v2", &error));
  assert(error.empty());

  const auto source = MakeSource();
  zsoda::inference::InferenceRequest request;
  request.source = &source;
  request.quality = 1;

  zsoda::core::FrameBuffer output;
  const bool run_ok = engine.Run(request, &output, &error);

  const auto status = engine.BackendStatus();
  if (status.last_run_used_fallback) {
    assert(!run_ok);
    assert(!status.fallback_reason.empty());
    assert(error == status.fallback_reason);
  } else {
    assert(run_ok);
    assert(status.fallback_reason.empty());
    assert(error.empty());
    assert(!output.empty());
  }
}

void TestMissingModelFileDiagnostics() {
  TempDir temp_dir;
  const auto manifest = temp_dir.path() / zsoda::inference::ModelCatalog::DefaultManifestFilename();
  WriteTextFile(
      manifest,
      "# id|display_name|relative_path|download_url|preferred_default|auxiliary_assets\n"
      "missing-depth-v1|Missing Depth v1|missing/missing_depth_v1.onnx|https://example.com/missing_depth_v1.onnx|true|missing/missing_depth_v1.onnx_data::https://example.com/missing_depth_v1.onnx_data\n");

  zsoda::inference::ManagedInferenceEngine engine(temp_dir.path().string());
  std::string error;
  assert(engine.Initialize("missing-depth-v1", &error));
  assert(Contains(error, "model asset file not found:"));
#if defined(ZSODA_WITH_ONNX_RUNTIME)
  const auto status = engine.BackendStatus();
  assert(Contains(status.engine_name, "BackendUnavailable"));
  if (Contains(status.engine_name, "requested=OnnxRuntimeBackend")) {
    assert(Contains(status.fallback_reason, "model path does not exist:"));
    assert(Contains(status.fallback_reason, "missing_depth_v1.onnx"));
  } else {
    assert(Contains(status.fallback_reason, "onnx runtime"));
    assert(HasOrtInitializationDiagnostic(status.fallback_reason));
  }
#endif

  const auto source = MakeSource();
  zsoda::inference::InferenceRequest request;
  request.source = &source;
  request.quality = 1;

  zsoda::core::FrameBuffer output;
  assert(!engine.Run(request, &output, &error));

  const auto run_status = engine.BackendStatus();
  assert(run_status.last_run_used_fallback);
  if (!run_status.fallback_reason.empty()) {
    assert(error == run_status.fallback_reason);
  } else {
    assert(error == "selected model assets are not fully installed");
  }
}

#if defined(ZSODA_WITH_ONNX_RUNTIME)
void TestOnnxPreprocessAspectRatioForNonSquareInput() {
  const auto run_case = [](int source_width,
                           int source_height,
                           zsoda::inference::PreprocessResizeMode resize_mode) {
    const auto source = MakeRgbCenteredSquareSource(source_width, source_height, 4);
    zsoda::inference::InferenceRequest request;
    request.source = &source;
    request.quality = 1;

    zsoda::inference::ModelPipelineProfile profile;
    profile.input_width = 12;
    profile.input_height = 12;
    profile.normalize_mean = {0.0F, 0.0F, 0.0F};
    profile.normalize_std = {1.0F, 1.0F, 1.0F};
    profile.invert_depth = false;

    zsoda::inference::PreparedModelInput prepared;
    std::string error;
    assert(zsoda::inference::PrepareInputForModel(
        request,
        profile,
        profile.input_width,
        profile.input_height,
        resize_mode,
        false,
        &prepared,
        &error));
    assert(error.empty());

    const float spread_ratio = MeasureChannelSpreadAspectRatio(prepared, 0);
    assert(spread_ratio > 0.0F);
    assert(spread_ratio >= 0.8F);
    assert(spread_ratio <= 1.25F);
  };

  for (const auto mode : {zsoda::inference::PreprocessResizeMode::kUpperBoundLetterbox,
                          zsoda::inference::PreprocessResizeMode::kLowerBoundCenterCrop}) {
    run_case(16, 8, mode);
    run_case(8, 16, mode);
  }
}

void TestOnnxPreprocessStretchModeDistortsAspectRatio() {
  const auto source = MakeRgbCenteredSquareSource(16, 8, 4);
  zsoda::inference::InferenceRequest request;
  request.source = &source;
  request.quality = 2;
  request.resize_mode = zsoda::inference::PreprocessResizeMode::kStretch;

  zsoda::inference::ModelPipelineProfile profile;
  profile.input_width = 12;
  profile.input_height = 12;
  profile.normalize_mean = {0.0F, 0.0F, 0.0F};
  profile.normalize_std = {1.0F, 1.0F, 1.0F};
  profile.invert_depth = false;

  zsoda::inference::PreparedModelInput prepared;
  std::string error;
  assert(zsoda::inference::PrepareInputForModel(request,
                                                profile,
                                                profile.input_width,
                                                profile.input_height,
                                                zsoda::inference::PreprocessResizeMode::kStretch,
                                                false,
                                                &prepared,
                                                &error));
  assert(error.empty());

  const float spread_ratio = MeasureChannelSpreadAspectRatio(prepared, 0);
  assert(spread_ratio > 0.0F);
  assert(spread_ratio <= 0.7F || spread_ratio >= 1.4F);
}

void TestDistillAnyDepthProfileDefaults() {
  const auto profile = zsoda::inference::ResolvePipelineProfile("distill-any-depth-base");
  assert(profile.input_width == 518);
  assert(profile.input_height == 518);
  assert(profile.input_frame_count == 1);
  assert(std::abs(profile.normalize_mean[0] - 0.485F) < 1e-6F);
  assert(std::abs(profile.normalize_mean[1] - 0.456F) < 1e-6F);
  assert(std::abs(profile.normalize_mean[2] - 0.406F) < 1e-6F);
  assert(std::abs(profile.normalize_std[0] - 0.229F) < 1e-6F);
  assert(std::abs(profile.normalize_std[1] - 0.224F) < 1e-6F);
  assert(std::abs(profile.normalize_std[2] - 0.225F) < 1e-6F);
  assert(!profile.invert_depth);
  assert(!profile.prefer_latest_output_map);
  assert(profile.use_upper_bound_dynamic_aspect);
  assert(profile.patch_multiple == 14);
  assert(!profile.use_middle_reference_strategy);
}

void TestOnnxBackendValidationScaffold() {
  zsoda::inference::RuntimeOptions options;
  options.preferred_backend = zsoda::inference::RuntimeBackend::kCuda;

  std::string error;
  auto backend = zsoda::inference::CreateOnnxRuntimeBackend(options, &error);
  if (backend == nullptr) {
    assert(!error.empty());
    assert(Contains(error, "onnx runtime"));
    assert(HasOrtBootstrapDiagnostic(error));
    return;
  }
  assert(error.empty());
  assert(Contains(backend->Name(), "OnnxRuntimeBackend"));

  error.clear();
  assert(!backend->SelectModel("", "placeholder.onnx", &error));
  assert(Contains(error, "model id cannot be empty"));

  error.clear();
  assert(!backend->SelectModel("distill-any-depth-base", "", &error));
  assert(Contains(error, "model path cannot be empty"));

  TempDir temp_dir;
  const auto missing = temp_dir.path() / "missing.onnx";
  error.clear();
  assert(!backend->SelectModel("distill-any-depth-base", missing.string(), &error));
  assert(Contains(error, "model path does not exist:"));

  const auto model_dir = temp_dir.path() / "as-directory";
  std::filesystem::create_directories(model_dir);
  error.clear();
  assert(!backend->SelectModel("distill-any-depth-base", model_dir.string(), &error));
  assert(Contains(error, "model path is not a regular file:"));

  const auto wrong_extension = temp_dir.path() / "distill_any_depth_base.txt";
  WriteTextFile(wrong_extension, "dummy");
  error.clear();
  assert(!backend->SelectModel("distill-any-depth-base", wrong_extension.string(), &error));
  assert(Contains(error, "model file extension must be .onnx:"));

  const auto valid_model = temp_dir.path() / "distill_any_depth_base.onnx";
  WriteTextFile(valid_model, "dummy");
  error.clear();
#if defined(ZSODA_WITH_ONNX_RUNTIME_API)
  assert(!backend->SelectModel("distill-any-depth-base", valid_model.string(), &error));
  assert(HasOrtRunOrExecutionDiagnostic(error));
#else
  assert(backend->SelectModel("distill-any-depth-base", valid_model.string(), &error));
  assert(error.empty());

  const auto source = MakeSource();
  zsoda::inference::InferenceRequest request;
  request.source = &source;
  request.quality = 1;

  zsoda::core::FrameBuffer output;
  error.clear();
  assert(!backend->Run(request, &output, &error));
  assert(HasOrtRunOrExecutionDiagnostic(error));
  assert(Contains(error, "model_id=distill-any-depth-base"));
  assert(Contains(error, valid_model.string()));
#endif
}

void TestOnnxSelectModelAcceptsMinimalFloatIdentityGraph() {
  const auto ort_runtime = FindTestOrtRuntimeLibrary();
  if (!ort_runtime.has_value()) {
    return;
  }

  TempDir temp_dir;
  const auto model_path = temp_dir.path() / "float_identity.onnx";
  WriteBinaryFile(model_path, kMinimalFloatIdentityOnnx);

  ScopedEnvironmentOverride set_ort_runtime("ZSODA_ONNXRUNTIME_LIBRARY", ort_runtime->string());
  ScopedEnvironmentOverride disable_preloaded("ZSODA_ORT_PREFER_PRELOADED", "0");

  zsoda::inference::RuntimeOptions options;
  options.preferred_backend = zsoda::inference::RuntimeBackend::kCpu;

  std::string error;
  auto backend = zsoda::inference::CreateOnnxRuntimeBackend(options, &error);
  if (backend == nullptr) {
    assert(!error.empty());
    assert(HasOrtBootstrapDiagnostic(error));
    return;
  }
  assert(error.empty());
  assert(backend->ActiveBackend() == zsoda::inference::RuntimeBackend::kCpu);

  error.clear();
  assert(backend->SelectModel("regression-float-identity", model_path.string(), &error));
  assert(error.empty());
}
#endif

void TestModelAutoDownloaderValidation() {
  std::string detail;
  const auto empty_id_status = zsoda::inference::RequestModelDownloadAsync(
      {.model_id = "", .download_url = "https://example.com/model.onnx", .destination_path = "x"},
      &detail);
  assert(empty_id_status == zsoda::inference::ModelDownloadRequestStatus::kSkipped);
  assert(Contains(detail, "model id is empty"));

  detail.clear();
  const auto empty_url_status = zsoda::inference::RequestModelDownloadAsync(
      {.model_id = "distill-any-depth-base", .download_url = "", .destination_path = "x"},
      &detail);
  assert(empty_url_status == zsoda::inference::ModelDownloadRequestStatus::kSkipped);
  assert(Contains(detail, "download url is empty"));

  detail.clear();
  const auto empty_path_status = zsoda::inference::RequestModelDownloadAsync(
      {.model_id = "distill-any-depth-base", .download_url = "https://example.com/model.onnx", .destination_path = ""},
      &detail);
  assert(empty_path_status == zsoda::inference::ModelDownloadRequestStatus::kSkipped);
  assert(Contains(detail, "destination path is empty"));
}

}  // namespace

void RunInferenceEngineTests() {
  TestModelList();
  TestModelSelection();
  TestRuntimeBackendOptions();
  TestRemoteBackendCommandValidation();
  TestRemoteBackendSafeFallbackWithManagedEngine();
  TestRemoteBackendEndpointInitialization();
  TestCreateDefaultEngineKeepsAutoBackendWhenRemoteFallbackIsEnabled();
  TestRemoteFallbackDoesNotSuppressMissingLocalModelDiagnostics();
  TestExplicitRemoteBackendStillIgnoresMissingLocalModelAssets();
  TestRunRequiresSelectedModel();
  TestBackendStatusDiagnostics();
  TestManifestLoadingAndDefaults();
  TestManifestLoadValidation();
  TestManifestAuxiliaryAssetsParsing();
  TestManifestModelSelectionRunPath();
  TestMissingModelFileDiagnostics();
#if defined(ZSODA_WITH_ONNX_RUNTIME)
  TestOnnxPreprocessAspectRatioForNonSquareInput();
  TestOnnxPreprocessStretchModeDistortsAspectRatio();
  TestDistillAnyDepthProfileDefaults();
  TestOnnxBackendValidationScaffold();
  TestOnnxSelectModelAcceptsMinimalFloatIdentityGraph();
#endif
  TestModelAutoDownloaderValidation();
}
