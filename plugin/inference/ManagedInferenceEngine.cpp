#include "inference/ManagedInferenceEngine.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <utility>

#include "inference/ModelAutoDownloader.h"

#if defined(ZSODA_WITH_ONNX_RUNTIME)
#include "inference/OnnxRuntimeBackend.h"
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace zsoda::inference {
namespace {

const char* SafeCStr(const char* value, const char* fallback = "<null>") {
  return value != nullptr ? value : fallback;
}

void AppendInferenceTrace(const char* stage, const char* detail = nullptr) {
#if defined(_WIN32)
  char temp_path[MAX_PATH] = {};
  const DWORD written = ::GetTempPathA(MAX_PATH, temp_path);
  if (written == 0 || written >= MAX_PATH) {
    return;
  }

  char log_path[MAX_PATH] = {};
  std::snprintf(log_path, sizeof(log_path), "%s%s", temp_path, "ZSoda_AE_Runtime.log");
  FILE* file = std::fopen(log_path, "ab");
  if (file == nullptr) {
    return;
  }

  SYSTEMTIME now = {};
  ::GetLocalTime(&now);
  const unsigned long tid = static_cast<unsigned long>(::GetCurrentThreadId());
  std::fprintf(file,
               "%04u-%02u-%02u %02u:%02u:%02u.%03u | InferenceTrace | tid=%lu, stage=%s, detail=%s\r\n",
               static_cast<unsigned>(now.wYear),
               static_cast<unsigned>(now.wMonth),
               static_cast<unsigned>(now.wDay),
               static_cast<unsigned>(now.wHour),
               static_cast<unsigned>(now.wMinute),
               static_cast<unsigned>(now.wSecond),
               static_cast<unsigned>(now.wMilliseconds),
               tid,
               stage != nullptr ? stage : "<null>",
               (detail != nullptr && detail[0] != '\0') ? detail : "<none>");
  std::fclose(file);
#else
  (void)stage;
  (void)detail;
#endif
}

bool IsModelAssetPresent(const ResolvedModelAsset& asset) {
  if (asset.absolute_path.empty()) {
    return false;
  }
  std::error_code ec;
  return std::filesystem::is_regular_file(std::filesystem::path(asset.absolute_path), ec) && !ec;
}

bool AreModelAssetsPresent(const std::vector<ResolvedModelAsset>& assets) {
  if (assets.empty()) {
    return false;
  }
  for (const auto& asset : assets) {
    if (!IsModelAssetPresent(asset)) {
      return false;
    }
  }
  return true;
}

std::vector<std::string> CollectMissingModelAssetPaths(
    const std::vector<ResolvedModelAsset>& assets) {
  std::vector<std::string> missing_paths;
  for (const auto& asset : assets) {
    if (!IsModelAssetPresent(asset)) {
      missing_paths.push_back(asset.absolute_path);
    }
  }
  return missing_paths;
}

}  // namespace

ManagedInferenceEngine::ManagedInferenceEngine(std::string model_root)
    : ManagedInferenceEngine(std::move(model_root), RuntimeOptions{}) {}

ManagedInferenceEngine::ManagedInferenceEngine(std::string model_root, RuntimeOptions options)
    : model_root_(std::move(model_root)), options_(std::move(options)) {
  ConfigureBackend();
  LoadManifest();
}

bool ManagedInferenceEngine::Initialize(const std::string& model_id, std::string* error) {
  zsoda::core::CompatLockGuard lock(mutex_);
  const std::string target = model_id.empty() ? catalog_.DefaultModelId() : model_id;
  return SelectModelLocked(target, error);
}

bool ManagedInferenceEngine::SelectModel(const std::string& model_id, std::string* error) {
  zsoda::core::CompatLockGuard lock(mutex_);
  if (model_id.empty()) {
    if (error) {
      *error = "model id cannot be empty";
    }
    return false;
  }
  if (model_id == active_model_id_) {
    model_file_exists_ = AreModelAssetsPresent(active_model_assets_);
    if (!model_file_exists_) {
      const auto* model = catalog_.FindById(active_model_id_);
      if (model != nullptr) {
        MaybeQueueModelDownloadLocked(*model, active_model_assets_);
      }
    }
    TryPromoteActiveModelToOnnxLocked();
    return true;
  }
  return SelectModelLocked(model_id, error);
}

std::vector<std::string> ManagedInferenceEngine::ListModelIds() const {
  zsoda::core::CompatLockGuard lock(mutex_);
  std::vector<std::string> result;
  result.reserve(catalog_.models().size());
  for (const auto& model : catalog_.models()) {
    result.push_back(model.id);
  }
  return result;
}

std::string ManagedInferenceEngine::ActiveModelId() const {
  zsoda::core::CompatLockGuard lock(mutex_);
  return active_model_id_;
}

bool ManagedInferenceEngine::Run(const InferenceRequest& request,
                                 zsoda::core::FrameBuffer* out_depth,
                                 std::string* error) const {
  zsoda::core::CompatLockGuard lock(mutex_);
  AppendInferenceTrace("run_enter", active_model_id_.empty() ? "<no_model>" : active_model_id_.c_str());
  if (active_model_id_.empty()) {
    if (error) {
      *error = "model is not selected";
    }
    AppendInferenceTrace("run_no_model_selected");
    return false;
  }

  bool used_fallback_path = true;
#if defined(ZSODA_WITH_ONNX_RUNTIME)
  if (onnx_backend_ != nullptr && !using_fallback_engine_) {
    AppendInferenceTrace("onnx_run_begin");
    std::string onnx_error;
    if (onnx_backend_->Run(request, out_depth, &onnx_error)) {
      used_fallback_path = false;
      last_run_used_fallback_ = false;
      fallback_reason_.clear();
      AppendInferenceTrace("onnx_run_ok");
    } else {
      last_run_used_fallback_ = true;
      fallback_reason_ = onnx_error.empty()
                             ? "onnx runtime backend run failed; using fallback depth path"
                             : onnx_error;
      AppendInferenceTrace("onnx_run_failed",
                           onnx_error.empty() ? "<none>" : onnx_error.c_str());
    }
  }
#endif

  if (!used_fallback_path) {
    if (error != nullptr) {
      error->clear();
    }
    AppendInferenceTrace("run_exit_onnx");
    return true;
  }

  std::string dummy_error;
  AppendInferenceTrace("fallback_run_begin");
  if (!fallback_engine_.Run(request, out_depth, &dummy_error)) {
    last_run_used_fallback_ = true;
    if (!dummy_error.empty()) {
      fallback_reason_ = dummy_error;
    }
    if (error) {
      *error = dummy_error;
    }
    AppendInferenceTrace("fallback_run_failed",
                         dummy_error.empty() ? "<none>" : dummy_error.c_str());
    return false;
  }
  last_run_used_fallback_ = true;
  AppendInferenceTrace("fallback_run_ok");

  // Apply a small model-specific curve shift so model switching has visible effect
  // while the fallback depth path is active.
  const auto& desc = out_depth->desc();
  const float bias = ModelBias();
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      float value = out_depth->at(x, y, 0);
      value = std::clamp(value + bias, 0.0F, 1.0F);
      out_depth->at(x, y, 0) = value;
    }
  }

  if (!model_file_exists_ && error != nullptr) {
    *error = "selected model assets are not fully installed; using fallback depth path";
  } else if (error != nullptr) {
    error->clear();
  }
  AppendInferenceTrace("run_exit_fallback");
  return true;
}

RuntimeBackend ManagedInferenceEngine::RequestedBackend() const {
  return options_.preferred_backend;
}

RuntimeBackend ManagedInferenceEngine::ActiveBackend() const {
  return active_backend_;
}

bool ManagedInferenceEngine::UsingFallbackEngine() const {
  return using_fallback_engine_;
}

InferenceBackendStatus ManagedInferenceEngine::BackendStatus() const {
  zsoda::core::CompatLockGuard lock(mutex_);
  InferenceBackendStatus status;
  status.requested_backend = options_.preferred_backend;
  status.active_backend = active_backend_;
  status.using_fallback_engine = using_fallback_engine_;
  status.last_run_used_fallback = last_run_used_fallback_;
  status.active_backend_name = RuntimeBackendName(active_backend_);
#if defined(ZSODA_WITH_ONNX_RUNTIME)
  if (onnx_backend_ != nullptr) {
    const std::string backend_name = SafeCStr(onnx_backend_->Name(), "<null backend>");
    if (!using_fallback_engine_ && !status.last_run_used_fallback) {
      status.engine_name = backend_name;
    } else {
      status.engine_name = std::string(SafeCStr(fallback_engine_.Name(), "DummyDepthEngine")) +
                           " (fallback_from=" + backend_name + ")";
    }
  } else {
    status.engine_name = SafeCStr(fallback_engine_.Name(), "DummyDepthEngine");
  }
#else
  status.engine_name = SafeCStr(fallback_engine_.Name(), "DummyDepthEngine");
#endif
  status.fallback_reason = fallback_reason_;
  return status;
}

std::string ManagedInferenceEngine::BackendStatusString() const {
  const InferenceBackendStatus status = BackendStatus();
  std::string result;
  result.reserve(160 + status.fallback_reason.size());
  result.append("requested=");
  result.append(RuntimeBackendName(status.requested_backend));
  result.append(", active=");
  result.append(status.active_backend_name);
  result.append(", engine=");
  result.append(status.engine_name);
  result.append(", configured_fallback=");
  result.append(status.using_fallback_engine ? "true" : "false");
  result.append(", last_run_fallback=");
  result.append(status.last_run_used_fallback ? "true" : "false");
  result.append(", fallback_reason=");
  result.append(status.fallback_reason.empty() ? "none" : status.fallback_reason);
  return result;
}

void ManagedInferenceEngine::ConfigureBackend() {
#if defined(ZSODA_WITH_ONNX_RUNTIME)
  std::string backend_error;
  onnx_backend_ = CreateOnnxRuntimeBackend(options_, &backend_error);
  if (onnx_backend_ != nullptr) {
    using_fallback_engine_ = false;
    active_backend_ = onnx_backend_->ActiveBackend();
    last_run_used_fallback_ = false;
    fallback_reason_.clear();
  } else {
    using_fallback_engine_ = true;
    active_backend_ = RuntimeBackend::kCpu;
    last_run_used_fallback_ = true;
    fallback_reason_ = backend_error.empty()
                           ? "onnx runtime backend is unavailable; using fallback depth path"
                           : "onnx runtime backend initialization failed: " + backend_error;
  }
#else
  using_fallback_engine_ = true;
  active_backend_ = RuntimeBackend::kCpu;
  last_run_used_fallback_ = true;
  fallback_reason_ = "onnx runtime backend is disabled at build time; using fallback depth path";
#endif
}

void ManagedInferenceEngine::LoadManifest() {
  std::string manifest_error;
  if (!options_.model_manifest_path.empty()) {
    catalog_.LoadManifestFile(options_.model_manifest_path, &manifest_error);
    return;
  }
  catalog_.LoadManifestFromRoot(model_root_, &manifest_error);
}

bool ManagedInferenceEngine::SelectModelLocked(const std::string& model_id, std::string* error) {
  const auto* model = catalog_.FindById(model_id);
  if (model == nullptr) {
    if (error) {
      *error = "unknown model id: " + model_id;
    }
    return false;
  }

  std::vector<ResolvedModelAsset> model_assets =
      catalog_.ResolveModelAssets(model_root_, model_id);
  const std::string model_path = model_assets.empty()
                                     ? catalog_.ResolveModelPath(model_root_, model_id)
                                     : model_assets.front().absolute_path;
  if (model_assets.empty() && !model_path.empty()) {
    model_assets.push_back({
        model->relative_path,
        model->download_url,
        model_path,
    });
  }
  const bool exists = AreModelAssetsPresent(model_assets);
  std::string init_error;
  if (!fallback_engine_.Initialize(model_id, &init_error)) {
    if (error) {
      *error = init_error;
    }
    return false;
  }

#if defined(ZSODA_WITH_ONNX_RUNTIME)
  if (onnx_backend_ != nullptr) {
    std::string backend_error;
    using_fallback_engine_ = !onnx_backend_->SelectModel(model_id, model_path, &backend_error);
    if (using_fallback_engine_) {
      last_run_used_fallback_ = true;
      fallback_reason_ = backend_error.empty()
                             ? "onnx runtime backend rejected selected model; using fallback depth path"
                             : backend_error;
    } else {
      last_run_used_fallback_ = false;
      fallback_reason_.clear();
      active_backend_ = onnx_backend_->ActiveBackend();
    }
  } else {
    using_fallback_engine_ = true;
    last_run_used_fallback_ = true;
    if (fallback_reason_.empty()) {
      fallback_reason_ = "onnx runtime backend is unavailable; using fallback depth path";
    }
  }
#endif

  active_model_id_ = model_id;
  active_model_path_ = model_path;
  active_model_assets_ = std::move(model_assets);
  model_file_exists_ = exists;
  if (!exists) {
    MaybeQueueModelDownloadLocked(*model, active_model_assets_);
  }
  TryPromoteActiveModelToOnnxLocked();
  if (error) {
    if (exists) {
      error->clear();
    } else {
      const auto missing_assets = CollectMissingModelAssetPaths(active_model_assets_);
      if (!missing_assets.empty()) {
        std::ostringstream oss;
        oss << "model asset file not found: " << missing_assets.front();
        if (missing_assets.size() > 1U) {
          oss << " (+" << (missing_assets.size() - 1U) << " more)";
        }
        *error = oss.str();
      } else {
        *error = "model asset file not found: " + model_path;
      }
    }
  }
  return true;
}

void ManagedInferenceEngine::TryPromoteActiveModelToOnnxLocked() {
#if defined(ZSODA_WITH_ONNX_RUNTIME)
  if (onnx_backend_ == nullptr || active_model_id_.empty() || active_model_path_.empty()) {
    return;
  }
  if (!model_file_exists_) {
    return;
  }
  if (!using_fallback_engine_) {
    return;
  }

  std::string backend_error;
  if (onnx_backend_->SelectModel(active_model_id_, active_model_path_, &backend_error)) {
    using_fallback_engine_ = false;
    last_run_used_fallback_ = false;
    fallback_reason_.clear();
    active_backend_ = onnx_backend_->ActiveBackend();
    return;
  }

  if (!backend_error.empty()) {
    fallback_reason_ = backend_error;
  }
#endif
}

void ManagedInferenceEngine::MaybeQueueModelDownloadLocked(
    const ModelSpec& model,
    const std::vector<ResolvedModelAsset>& assets) {
  if (!options_.auto_download_missing_models) {
    return;
  }

  std::size_t queued_count = 0;
  std::size_t failed_count = 0;
  std::string last_failure_detail;
  for (const auto& asset : assets) {
    if (IsModelAssetPresent(asset)) {
      continue;
    }

    std::string download_detail;
    const auto status = RequestModelDownloadAsync(
        {
            .model_id = model.id,
            .asset_relative_path = asset.relative_path,
            .download_url = asset.download_url,
            .destination_path = asset.absolute_path,
        },
        &download_detail);
    if (status == ModelDownloadRequestStatus::kQueued) {
      ++queued_count;
      continue;
    }
    if (status == ModelDownloadRequestStatus::kFailed) {
      ++failed_count;
      if (!download_detail.empty()) {
        last_failure_detail = download_detail;
      }
      continue;
    }
  }

  if (queued_count > 0) {
    std::ostringstream oss;
    oss << "selected model assets are not fully installed; background download queued (queued="
        << queued_count;
    if (failed_count > 0) {
      oss << ", failed=" << failed_count;
    }
    oss << ")";
    if (fallback_reason_.empty()) {
      fallback_reason_ = oss.str();
    } else {
      fallback_reason_ += " | auto_download=" + oss.str();
    }
    return;
  }

  if (failed_count > 0) {
    std::string failure_message = "auto download failed for missing model assets";
    if (!last_failure_detail.empty()) {
      failure_message += ": " + last_failure_detail;
    }
    if (fallback_reason_.empty()) {
      fallback_reason_ = failure_message;
    } else {
      fallback_reason_ += " | auto_download=" + failure_message;
    }
  }
}

float ManagedInferenceEngine::ModelBias() const {
  if (active_model_id_ == "depth-anything-v3-large") {
    return 0.08F;
  }
  if (active_model_id_ == "depth-anything-v3-base") {
    return 0.04F;
  }
  if (active_model_id_ == "depth-anything-v3-small") {
    return 0.02F;
  }
  if (active_model_id_ == "midas-dpt-large") {
    return -0.03F;
  }
  return 0.0F;
}

}  // namespace zsoda::inference
