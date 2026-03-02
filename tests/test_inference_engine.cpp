#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/Frame.h"
#include "inference/ManagedInferenceEngine.h"
#include "inference/ModelCatalog.h"
#if defined(ZSODA_WITH_ONNX_RUNTIME)
#include "inference/OnnxRuntimeBackend.h"
#endif
#include "inference/RuntimeOptions.h"

namespace {

class TempDir {
 public:
  TempDir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("zsoda-inference-test-" + std::to_string(stamp));
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

void TestModelList() {
  zsoda::inference::ManagedInferenceEngine engine("models");
  const auto models = engine.ListModelIds();
  assert(!models.empty());
  assert(HasModel(models, "depth-anything-v3-small"));
}

void TestModelSelection() {
  zsoda::inference::ManagedInferenceEngine engine("models");
  std::string error;
  assert(engine.Initialize("depth-anything-v3-small", &error));
  assert(engine.ActiveModelId() == "depth-anything-v3-small");

  error.clear();
  assert(!engine.SelectModel("unknown-model", &error));
  assert(!error.empty());

  error.clear();
  assert(engine.SelectModel("depth-anything-v3-large", &error));
  assert(engine.ActiveModelId() == "depth-anything-v3-large");
}

void TestRuntimeBackendOptions() {
  assert(zsoda::inference::ParseRuntimeBackend("auto") ==
         zsoda::inference::RuntimeBackend::kAuto);
  assert(zsoda::inference::ParseRuntimeBackend("cpu") ==
         zsoda::inference::RuntimeBackend::kCpu);
  assert(zsoda::inference::ParseRuntimeBackend("CUDA") ==
         zsoda::inference::RuntimeBackend::kCuda);
  assert(zsoda::inference::ParseRuntimeBackend("direct-ml") ==
         zsoda::inference::RuntimeBackend::kDirectML);
  assert(zsoda::inference::ParseRuntimeBackend("core_ml") ==
         zsoda::inference::RuntimeBackend::kCoreML);
  assert(zsoda::inference::ParseRuntimeBackend("unknown-backend") ==
         zsoda::inference::RuntimeBackend::kAuto);

  zsoda::inference::RuntimeOptions options;
  options.preferred_backend = zsoda::inference::RuntimeBackend::kCuda;
  zsoda::inference::ManagedInferenceEngine engine("models", options);
  assert(engine.RequestedBackend() == zsoda::inference::RuntimeBackend::kCuda);
#if defined(ZSODA_WITH_ONNX_RUNTIME)
  assert(!engine.UsingFallbackEngine());
#if defined(ZSODA_WITH_ONNX_RUNTIME_API)
  assert(engine.ActiveBackend() == zsoda::inference::RuntimeBackend::kCpu);
#else
  assert(engine.ActiveBackend() == zsoda::inference::RuntimeBackend::kCuda);
#endif
#else
  assert(engine.UsingFallbackEngine());
  assert(engine.ActiveBackend() == zsoda::inference::RuntimeBackend::kCpu);
#endif
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
  assert(engine.Run(request, &output, &error));

  const auto after = engine.BackendStatus();
  assert(after.last_run_used_fallback);
  assert(!after.fallback_reason.empty());
#if defined(ZSODA_WITH_ONNX_RUNTIME)
  assert(Contains(after.engine_name, "DummyDepthEngine"));
#if defined(ZSODA_WITH_ONNX_RUNTIME_API)
  assert(Contains(after.engine_name, "fallback_from=OnnxRuntimeBackend["));
  assert(Contains(after.fallback_reason, "onnx runtime session create failed:"));
#else
  assert(Contains(after.engine_name, "fallback_from=OnnxRuntimeBackendScaffold["));
  assert(Contains(after.fallback_reason, "execution is not available in this build"));
  assert(Contains(after.fallback_reason, "model_id=status-depth-v1"));
#endif
#else
  assert(after.engine_name == "DummyDepthEngine");
#endif
  assert(Contains(engine.BackendStatusString(), "last_run_fallback=true"));
  assert(Contains(engine.BackendStatusString(), "fallback_reason="));
}

void TestManifestLoadingAndDefaults() {
  TempDir temp_dir;
  const auto manifest = temp_dir.path() / zsoda::inference::ModelCatalog::DefaultManifestFilename();
  WriteTextFile(
      manifest,
      "# id|display_name|relative_path|download_url|preferred_default\n"
      "custom-depth-v1|Custom Depth v1|custom/custom_depth_v1.onnx|https://example.com/custom_depth_v1.onnx|true\n"
      "depth-anything-v3-small|Depth Anything v3 Small (Manifest)|depth-anything-v3/small_override.onnx|https://example.com/small_override.onnx|false\n");

  zsoda::inference::ManagedInferenceEngine engine(temp_dir.path().string());
  const auto ids = engine.ListModelIds();
  assert(HasModel(ids, "custom-depth-v1"));
  assert(HasModel(ids, "depth-anything-v3-small"));

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
  assert(engine.Run(request, &output, &error));
  assert(error.empty());
  assert(!output.empty());
}

void TestMissingModelFileDiagnostics() {
  TempDir temp_dir;
  const auto manifest = temp_dir.path() / zsoda::inference::ModelCatalog::DefaultManifestFilename();
  WriteTextFile(
      manifest,
      "# id|display_name|relative_path|download_url|preferred_default\n"
      "missing-depth-v1|Missing Depth v1|missing/missing_depth_v1.onnx|https://example.com/missing_depth_v1.onnx|true\n");

  zsoda::inference::ManagedInferenceEngine engine(temp_dir.path().string());
  std::string error;
  assert(engine.Initialize("missing-depth-v1", &error));
  assert(Contains(error, "model file not found:"));
#if defined(ZSODA_WITH_ONNX_RUNTIME)
  const auto status = engine.BackendStatus();
  assert(Contains(status.engine_name, "DummyDepthEngine"));
#if defined(ZSODA_WITH_ONNX_RUNTIME_API)
  assert(Contains(status.engine_name, "fallback_from=OnnxRuntimeBackend["));
#else
  assert(Contains(status.engine_name, "fallback_from=OnnxRuntimeBackendScaffold["));
#endif
  assert(Contains(status.fallback_reason, "model path does not exist:"));
  assert(Contains(status.fallback_reason, "missing_depth_v1.onnx"));
#endif

  const auto source = MakeSource();
  zsoda::inference::InferenceRequest request;
  request.source = &source;
  request.quality = 1;

  zsoda::core::FrameBuffer output;
  assert(engine.Run(request, &output, &error));
  assert(!output.empty());
  assert(error == "selected model file is not installed; using fallback depth path");
}

#if defined(ZSODA_WITH_ONNX_RUNTIME)
void TestOnnxBackendValidationScaffold() {
  zsoda::inference::RuntimeOptions options;
  options.preferred_backend = zsoda::inference::RuntimeBackend::kCuda;

  std::string error;
  auto backend = zsoda::inference::CreateOnnxRuntimeBackend(options, &error);
  assert(backend != nullptr);
  assert(error.empty());
#if defined(ZSODA_WITH_ONNX_RUNTIME_API)
  assert(Contains(backend->Name(), "OnnxRuntimeBackend[cpu]"));
#else
  assert(Contains(backend->Name(), "OnnxRuntimeBackendScaffold[cuda]"));
#endif

  error.clear();
  assert(!backend->SelectModel("", "placeholder.onnx", &error));
  assert(Contains(error, "model id cannot be empty"));

  error.clear();
  assert(!backend->SelectModel("depth-anything-v3-small", "", &error));
  assert(Contains(error, "model path cannot be empty"));

  TempDir temp_dir;
  const auto missing = temp_dir.path() / "missing.onnx";
  error.clear();
  assert(!backend->SelectModel("depth-anything-v3-small", missing.string(), &error));
  assert(Contains(error, "model path does not exist:"));

  const auto model_dir = temp_dir.path() / "as-directory";
  std::filesystem::create_directories(model_dir);
  error.clear();
  assert(!backend->SelectModel("depth-anything-v3-small", model_dir.string(), &error));
  assert(Contains(error, "model path is not a regular file:"));

  const auto wrong_extension = temp_dir.path() / "depth_anything_v3_small.txt";
  WriteTextFile(wrong_extension, "dummy");
  error.clear();
  assert(!backend->SelectModel("depth-anything-v3-small", wrong_extension.string(), &error));
  assert(Contains(error, "model file extension must be .onnx:"));

  const auto valid_model = temp_dir.path() / "depth_anything_v3_small.onnx";
  WriteTextFile(valid_model, "dummy");
  error.clear();
#if defined(ZSODA_WITH_ONNX_RUNTIME_API)
  assert(!backend->SelectModel("depth-anything-v3-small", valid_model.string(), &error));
  assert(Contains(error, "onnx runtime session create failed:"));
#else
  assert(backend->SelectModel("depth-anything-v3-small", valid_model.string(), &error));
  assert(error.empty());

  const auto source = MakeSource();
  zsoda::inference::InferenceRequest request;
  request.source = &source;
  request.quality = 1;

  zsoda::core::FrameBuffer output;
  error.clear();
  assert(!backend->Run(request, &output, &error));
  assert(Contains(error, "execution is not available in this build"));
  assert(Contains(error, "model_id=depth-anything-v3-small"));
  assert(Contains(error, valid_model.string()));
#endif
}
#endif

}  // namespace

void RunInferenceEngineTests() {
  TestModelList();
  TestModelSelection();
  TestRuntimeBackendOptions();
  TestRunRequiresSelectedModel();
  TestBackendStatusDiagnostics();
  TestManifestLoadingAndDefaults();
  TestManifestLoadValidation();
  TestManifestModelSelectionRunPath();
  TestMissingModelFileDiagnostics();
#if defined(ZSODA_WITH_ONNX_RUNTIME)
  TestOnnxBackendValidationScaffold();
#endif
}
