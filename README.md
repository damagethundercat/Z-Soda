# Z-Soda

Depth Scanner-style After Effects plugin scaffold.

## Project Operations
- Team role split: `TEAM.md`
- Execution plan: `PLAN.md`
- Live progress (Korean): `PROGRESS.md`
- Research references: `docs/research/`
- Perf/QA harness guide: `docs/perf/README.md`
- Final-mile packaging guide (.aex/.plugin): `docs/build/README.md`

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

Custom model root example:
```bash
export ZSODA_MODEL_ROOT=/path/to/models
```

Optional runtime options:
```bash
export ZSODA_INFERENCE_BACKEND=cpu      # auto|cpu|cuda|directml|metal|coreml
export ZSODA_MODEL_MANIFEST=models/models.manifest
```

Current runtime note:
- The model/session management path is implemented.
- Default build keeps scaffold mode (`ZSODA_WITH_ONNX_RUNTIME=ON`, `ZSODA_WITH_ONNX_RUNTIME_API` unset): ORT API is not used and safe fallback depth path is used on run.
- Optional API mode (`ZSODA_WITH_ONNX_RUNTIME_API=ON`) enables real ONNX Runtime C++ session create/select/run (CPU-first path).

## AE Packaging Status
- Final-mile `.aex/.plugin` packaging path is documented in `docs/build/README.md`.
- Current workspace is Linux/WSL2 without `cmake`, AE SDK, MSBuild, or Xcode, so native packaging commands are not executable here.

## Build & Test
Preferred (when `cmake` is available):
```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
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
  plugin/inference/DummyInferenceEngine.cpp plugin/inference/EngineFactory.cpp \
  plugin/inference/ManagedInferenceEngine.cpp plugin/inference/ModelCatalog.cpp \
  tests/test_ae_params.cpp tests/test_ae_router.cpp tests/test_cache.cpp \
  tests/test_depth_ops.cpp tests/test_inference_engine.cpp \
  tests/test_render_pipeline.cpp tests/test_tiler.cpp \
  -o /tmp/zsoda_tests
/tmp/zsoda_tests
```

Performance harness fallback:
```bash
g++ -std=c++20 -Iplugin \
  plugin/ae/AeHostAdapter.cpp plugin/ae/AeCommandRouter.cpp plugin/ae/AeParams.cpp plugin/ae/AePluginEntry.cpp \
  plugin/core/BufferPool.cpp plugin/core/Cache.cpp plugin/core/DepthOps.cpp \
  plugin/core/RenderPipeline.cpp plugin/core/Tiler.cpp \
  plugin/inference/DummyInferenceEngine.cpp plugin/inference/EngineFactory.cpp \
  plugin/inference/ManagedInferenceEngine.cpp plugin/inference/ModelCatalog.cpp \
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
  plugin/inference/DummyInferenceEngine.cpp plugin/inference/EngineFactory.cpp \
  plugin/inference/ManagedInferenceEngine.cpp plugin/inference/ModelCatalog.cpp \
  plugin/inference/OnnxRuntimeBackend.cpp \
  tests/test_ae_params.cpp tests/test_ae_router.cpp tests/test_cache.cpp \
  tests/test_depth_ops.cpp tests/test_inference_engine.cpp \
  tests/test_render_pipeline.cpp tests/test_tiler.cpp \
  -o /tmp/zsoda_tests_ort
/tmp/zsoda_tests_ort
```
