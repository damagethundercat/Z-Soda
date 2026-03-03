# Z-Soda

Depth Scanner-style After Effects plugin scaffold.

## Project Operations
- Team role split: `TEAM.md`
- Execution plan: `PLAN.md`
- Live progress (Korean): `PROGRESS.md`
- Research references: `docs/research/`
- Perf/QA harness guide: `docs/perf/README.md`
- Final-mile packaging guide (.aex/.plugin): `docs/build/README.md`
- Leader review note (acceptance/remaining gates): `docs/build/2026-03-02-leader-review-note.md`
- Local Windows agent handoff guide: `docs/build/LOCAL_AGENT_HANDOFF.md`
- AE smoke test guide: `docs/build/AE_SMOKE_TEST.md`
- ORT runtime deploy note: `docs/build/ORT_RUNTIME_DEPLOY.md`
- ORT runtime isolation plan: `docs/build/ORT_RUNTIME_ISOLATION_PLAN.md`
- Windows `.aex` build helper script: `tools/build_aex.ps1`

## Current Layout
- `plugin/ae`: AE command routing and plugin entry stub
- `plugin/core`: depth processing core (cache, tiling, render pipeline)
- `plugin/inference`: inference engine interface + dummy engine
- `models`: model installation guide and expected layout
- `tools`: helper scripts (model downloader)
- `plugin/backends`: backend type definitions
- `tests`: core unit tests

## Model Workflow
- Default priority model: `Depth Anything v3` (`depth-anything-v3-small`)
- You can switch model per render via `RenderParams.model_id`
- Supported model IDs:
  - `depth-anything-v3-small`
  - `depth-anything-v3-base`
  - `depth-anything-v3-large`
  - `midas-dpt-large`

Download example:
```bash
bash tools/download_model.sh depth-anything-v3-small
```

Windows PowerShell:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\download_model.ps1 -ModelId depth-anything-v3-small
```

Custom model root example:
```bash
export ZSODA_MODEL_ROOT=/path/to/models
```

Runtime default path note:
- `ZSODA_MODEL_ROOT` 미설정 시 `.aex` 인접 `models/`를 우선 탐색하고, 없으면 상대 경로 `models/`를 사용합니다.
- `ZSODA_ONNXRUNTIME_LIBRARY` 미설정 시 `.aex` 인접 `runtime/onnxruntime.dll` -> `.aex` 인접 `onnxruntime.dll` 순으로 탐색합니다.
- DA3 계열 모델은 `model.onnx` 외에 `model.onnx_data` 자산이 함께 필요하며, 매니페스트 `auxiliary_assets` 열로 관리합니다.
- 모델 파일이 없으면 기본값에서 백그라운드 다운로드를 자동 요청하고, 다운로드 전까지는 안전 폴백 렌더를 사용합니다.

Optional runtime options:
```bash
export ZSODA_INFERENCE_BACKEND=cpu      # auto|cpu|cuda|directml|metal|coreml
export ZSODA_MODEL_MANIFEST=models/models.manifest
export ZSODA_AUTO_DOWNLOAD_MODELS=1     # 1:on(default), 0:off
export HF_TOKEN=...                     # optional: Hugging Face 인증 토큰
```

Current runtime note:
- The model/session management path is implemented.
- Windows `tools/build_aex.ps1` 기본값은 ORT API 활성화이며, ONNX Runtime C++ session create/select/run (CPU-first path)로 동작합니다.
- ORT API를 비활성화하려면 `tools/build_aex.ps1 -DisableOrtApi`를 사용합니다.

## AE Packaging Status
- Final-mile `.aex/.plugin` packaging path is documented in `docs/build/README.md`.
- With `ZSODA_WITH_AE_SDK=ON`, CMake exposes native packaging targets:
  - Windows: `zsoda_aex` (`ZSoda.aex`)
  - macOS: `zsoda_plugin_bundle` (`ZSoda.plugin` bundle target with `Info.plist`)
- Packaging helper scripts:
  - `tools/package_plugin.sh`
  - `tools/package_plugin.ps1`
- `.aex` 즉시 실행 가이드는 `docs/build/README.md`의 아래 섹션을 순서대로 확인:
  - `Windows 빠른 시작 10단계`
  - `실패 시 점검 5항목`
  - `산출물 확인 명령`
- Current workspace is Linux/WSL2 without `cmake`, AE SDK, MSBuild, or Xcode, so native packaging commands are not executable here.

## 사용자 테스트 가능 시점
- Windows 네이티브 환경(MSVC + AE SDK + CMake + ONNX Runtime 경로) 준비 즉시 사용자 테스트를 시작할 수 있습니다.
- 시작 절차는 `docs/build/README.md`의 `Windows 빠른 시작 10단계`를 기준으로 진행하면 됩니다.

## Build & Test
Preferred (when `cmake` is available):
```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Quick local CI check:
```bash
bash tools/run_local_ci.sh
```

Optional ONNX Runtime API build (requires local ORT headers + library):
```bash
cmake -S . -B build-ort \
  -DZSODA_WITH_ONNX_RUNTIME=ON \
  -DZSODA_WITH_ONNX_RUNTIME_API=ON \
  -DONNXRUNTIME_INCLUDE_DIR=/abs/path/to/onnxruntime/include \
  -DONNXRUNTIME_LIBRARY=/abs/path/to/libonnxruntime.so
cmake --build build-ort -j
ctest --test-dir build-ort --output-on-failure
```

Fallback (current environment):
```bash
g++ -std=c++20 -Iplugin \
  plugin/ae/AeHostAdapter.cpp plugin/ae/AeCommandRouter.cpp plugin/ae/AeParams.cpp plugin/ae/AePluginEntry.cpp \
  plugin/core/BufferPool.cpp plugin/core/Cache.cpp plugin/core/DepthOps.cpp \
  plugin/core/RenderPipeline.cpp plugin/core/Tiler.cpp \
  plugin/inference/DummyInferenceEngine.cpp plugin/inference/EngineFactory.cpp plugin/inference/ModelAutoDownloader.cpp \
  plugin/inference/ManagedInferenceEngine.cpp plugin/inference/ModelCatalog.cpp plugin/inference/RuntimePathResolver.cpp \
  tests/test_ae_params.cpp tests/test_ae_router.cpp tests/test_cache.cpp \
  tests/test_depth_ops.cpp tests/test_inference_engine.cpp \
  tests/test_render_pipeline.cpp tests/test_runtime_path_resolver.cpp tests/test_tiler.cpp \
  -o /tmp/zsoda_tests
/tmp/zsoda_tests
```

Performance harness fallback:
```bash
g++ -std=c++20 -Iplugin \
  plugin/ae/AeHostAdapter.cpp plugin/ae/AeCommandRouter.cpp plugin/ae/AeParams.cpp plugin/ae/AePluginEntry.cpp \
  plugin/core/BufferPool.cpp plugin/core/Cache.cpp plugin/core/DepthOps.cpp \
  plugin/core/RenderPipeline.cpp plugin/core/Tiler.cpp \
  plugin/inference/DummyInferenceEngine.cpp plugin/inference/EngineFactory.cpp plugin/inference/ModelAutoDownloader.cpp \
  plugin/inference/ManagedInferenceEngine.cpp plugin/inference/ModelCatalog.cpp plugin/inference/RuntimePathResolver.cpp \
  tests/perf_harness.cpp \
  -o /tmp/zsoda_perf_harness
/tmp/zsoda_perf_harness --mode benchmark --quiet
/tmp/zsoda_perf_harness --mode stability --frames 1000 --quiet
```

Optional ORT-scaffold compile check (API off):
```bash
g++ -std=c++20 -DZSODA_WITH_ONNX_RUNTIME=1 -Iplugin \
  plugin/ae/AeHostAdapter.cpp plugin/ae/AeCommandRouter.cpp plugin/ae/AeParams.cpp plugin/ae/AePluginEntry.cpp \
  plugin/core/BufferPool.cpp plugin/core/Cache.cpp plugin/core/DepthOps.cpp \
  plugin/core/RenderPipeline.cpp plugin/core/Tiler.cpp \
  plugin/inference/DummyInferenceEngine.cpp plugin/inference/EngineFactory.cpp plugin/inference/ModelAutoDownloader.cpp \
  plugin/inference/ManagedInferenceEngine.cpp plugin/inference/ModelCatalog.cpp plugin/inference/RuntimePathResolver.cpp \
  plugin/inference/OnnxRuntimeBackend.cpp \
  tests/test_ae_params.cpp tests/test_ae_router.cpp tests/test_cache.cpp \
  tests/test_depth_ops.cpp tests/test_inference_engine.cpp \
  tests/test_render_pipeline.cpp tests/test_runtime_path_resolver.cpp tests/test_tiler.cpp \
  -o /tmp/zsoda_tests_ort
/tmp/zsoda_tests_ort
```
