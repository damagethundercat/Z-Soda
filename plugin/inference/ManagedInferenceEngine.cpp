#include "inference/ManagedInferenceEngine.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <utility>

#include "ae/AeDiagnostics.h"
#include "inference/ModelAutoDownloader.h"

#include "inference/OnnxRuntimeBackend.h"

namespace zsoda::inference {
namespace {

constexpr auto kRemoteBackendRecoveryRetryInterval = std::chrono::milliseconds(750);

const char* SafeCStr(const char* value, const char* fallback = "<null>") {
  return value != nullptr ? value : fallback;
}

void AppendInferenceTrace(const char* stage, const char* detail = nullptr) {
  zsoda::ae::AppendDiagnosticsTrace("InferenceTrace", stage, detail);
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
    const bool requires_local_model_assets = !WantsRemoteBackendLocked();
    model_file_exists_ = requires_local_model_assets ? AreModelAssetsPresent(active_model_assets_) : true;
    if (requires_local_model_assets && !model_file_exists_) {
      const auto* model = catalog_.FindById(active_model_id_);
      if (model != nullptr) {
        MaybeQueueModelDownloadLocked(*model, active_model_assets_);
      }
    }
    TryRecoverRequestedBackendLocked();
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
                             ? "onnx runtime backend run failed"
                             : onnx_error;
      AppendInferenceTrace("onnx_run_failed",
                           onnx_error.empty() ? "<none>" : onnx_error.c_str());
    }
  }

  if (!used_fallback_path) {
    if (error != nullptr) {
      error->clear();
    }
    AppendInferenceTrace("run_exit_onnx");
    return true;
  }

  last_run_used_fallback_ = true;
  if (error != nullptr) {
    if (!fallback_reason_.empty()) {
      *error = fallback_reason_;
    } else if (!model_file_exists_) {
      *error = "selected model assets are not fully installed";
    } else {
      *error = "inference backend is unavailable";
    }
  }
  AppendInferenceTrace("run_exit_backend_unavailable",
                       fallback_reason_.empty() ? "<none>" : fallback_reason_.c_str());
  return false;
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
  if (onnx_backend_ != nullptr) {
    const std::string backend_name = SafeCStr(onnx_backend_->Name(), "<null backend>");
    if (!using_fallback_engine_ && !status.last_run_used_fallback) {
      status.engine_name = backend_name;
    } else {
      status.engine_name = "BackendUnavailable (requested=" + backend_name + ")";
    }
  } else {
    status.engine_name = "BackendUnavailable";
  }
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
  onnx_backend_.reset();

  auto activate_backend = [this](std::unique_ptr<IOnnxRuntimeBackend> backend,
                                 const std::string& selection_note) {
    onnx_backend_ = std::move(backend);
    using_fallback_engine_ = false;
    active_backend_ = onnx_backend_->ActiveBackend();
    last_run_used_fallback_ = false;
    fallback_reason_ = selection_note;
  };

  auto mark_backend_unavailable = [this](RuntimeBackend backend, std::string reason) {
    onnx_backend_.reset();
    using_fallback_engine_ = true;
    active_backend_ = backend;
    last_run_used_fallback_ = true;
    fallback_reason_ = std::move(reason);
  };

  auto make_remote_options = [this]() {
    RuntimeOptions remote_options = options_;
    remote_options.preferred_backend = RuntimeBackend::kRemote;
    remote_options.remote_inference_enabled = true;
    return remote_options;
  };

  if (WantsRemoteBackendLocked()) {
    std::string remote_error;
    auto remote_backend = CreateRemoteInferenceBackend(make_remote_options(), &remote_error);
    if (remote_backend != nullptr) {
      activate_backend(std::move(remote_backend), "");
      return;
    }

    mark_backend_unavailable(
        RuntimeBackend::kRemote,
        remote_error.empty()
            ? "remote inference backend is unavailable"
            : "remote inference backend initialization failed: " + remote_error);
    return;
  }

  std::string ort_failure_reason;

#if defined(ZSODA_WITH_ONNX_RUNTIME)
  std::string ort_error;
  auto ort_backend = CreateOnnxRuntimeBackend(options_, &ort_error);
  if (ort_backend != nullptr) {
    activate_backend(std::move(ort_backend), "");
    return;
  }

  ort_failure_reason = ort_error.empty()
                           ? "onnx runtime backend is unavailable"
                           : "onnx runtime backend initialization failed: " + ort_error;
#else
  ort_failure_reason = "onnx runtime backend is disabled at build time";
#endif

  if (AllowsRemoteFallbackLocked()) {
    std::string remote_error;
    auto remote_backend = CreateRemoteInferenceBackend(make_remote_options(), &remote_error);
    if (remote_backend != nullptr) {
      const std::string remote_selection_note =
          ort_failure_reason.empty() ? std::string()
                                     : "native_ort_unavailable: " + ort_failure_reason;
      activate_backend(std::move(remote_backend), remote_selection_note);
      return;
    }

    if (!remote_error.empty()) {
      if (!ort_failure_reason.empty()) {
        ort_failure_reason.append(" | remote_fallback=");
        ort_failure_reason.append(remote_error);
      } else {
        ort_failure_reason = "remote inference backend initialization failed: " + remote_error;
      }
    }
  }

  mark_backend_unavailable(RuntimeBackend::kCpu, std::move(ort_failure_reason));
}

bool ManagedInferenceEngine::AllowsRemoteFallbackLocked() const {
  return options_.remote_inference_enabled || !options_.remote_endpoint.empty() ||
         options_.remote_service_autostart || !options_.remote_service_python.empty() ||
         !options_.remote_service_script_path.empty();
}

bool ManagedInferenceEngine::WantsRemoteBackendLocked() const {
  return options_.preferred_backend == RuntimeBackend::kRemote;
}

bool ManagedInferenceEngine::ShouldAttemptRemoteRecoveryLocked() const {
  return WantsRemoteBackendLocked() || AllowsRemoteFallbackLocked();
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

  if (onnx_backend_ != nullptr) {
    std::string backend_error;
    using_fallback_engine_ = !onnx_backend_->SelectModel(model_id, model_path, &backend_error);
    if (using_fallback_engine_) {
      last_run_used_fallback_ = true;
      fallback_reason_ = backend_error.empty()
                            ? "onnx runtime backend rejected selected model"
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
      fallback_reason_ = "onnx runtime backend is unavailable";
    }
  }

  active_model_id_ = model_id;
  active_model_path_ = model_path;
  active_model_assets_ = std::move(model_assets);
  const bool requires_local_model_assets = !WantsRemoteBackendLocked();
  model_file_exists_ = requires_local_model_assets ? exists : true;
  if (requires_local_model_assets && !exists) {
    MaybeQueueModelDownloadLocked(*model, active_model_assets_);
  }
  TryPromoteActiveModelToOnnxLocked();
  if (error) {
    if (!requires_local_model_assets || exists) {
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

void ManagedInferenceEngine::TryRecoverRequestedBackendLocked() {
  if (!ShouldAttemptRemoteRecoveryLocked()) {
    return;
  }
  if (onnx_backend_ != nullptr && !using_fallback_engine_) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (last_backend_recovery_attempt_.time_since_epoch().count() != 0 &&
      (now - last_backend_recovery_attempt_) < kRemoteBackendRecoveryRetryInterval) {
    return;
  }
  last_backend_recovery_attempt_ = now;
  AppendInferenceTrace("recover_backend_begin",
                       active_model_id_.empty() ? "<no_model>" : active_model_id_.c_str());

  ConfigureBackend();
  if (onnx_backend_ == nullptr) {
    AppendInferenceTrace("recover_backend_unavailable",
                         fallback_reason_.empty() ? "<none>" : fallback_reason_.c_str());
    return;
  }
  if (active_model_id_.empty()) {
    AppendInferenceTrace("recover_backend_no_model");
    return;
  }

  std::string backend_error;
  if (onnx_backend_->SelectModel(active_model_id_, active_model_path_, &backend_error)) {
    using_fallback_engine_ = false;
    last_run_used_fallback_ = false;
    fallback_reason_.clear();
    active_backend_ = onnx_backend_->ActiveBackend();
    model_file_exists_ = true;
    AppendInferenceTrace("recover_backend_ok");
    return;
  }

  using_fallback_engine_ = true;
  last_run_used_fallback_ = true;
  if (!backend_error.empty()) {
    fallback_reason_ = backend_error;
  }
  AppendInferenceTrace("recover_backend_select_failed",
                       backend_error.empty() ? "<none>" : backend_error.c_str());
}

void ManagedInferenceEngine::TryPromoteActiveModelToOnnxLocked() {
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

}  // namespace zsoda::inference
