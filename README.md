# Z-Soda

Depth Scanner-style After Effects plugin scaffold.

## Project Operations
- Team role split: `TEAM.md`
- Execution plan: `PLAN.md`
- Live progress (Korean): `PROGRESS.md`
- Research references: `docs/research/`

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

Current runtime note:
- The model/session management path is implemented.
- ONNX Runtime execution backend is not wired yet in this scaffold; if model files are missing, safe fallback depth path is used.

## Build & Test
Preferred (when `cmake` is available):
```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Fallback (current environment):
```bash
g++ -std=c++20 -Iplugin \
  plugin/ae/AeCommandRouter.cpp plugin/ae/AePluginEntry.cpp \
  plugin/core/BufferPool.cpp plugin/core/Cache.cpp plugin/core/DepthOps.cpp \
  plugin/core/RenderPipeline.cpp plugin/core/Tiler.cpp \
  plugin/inference/DummyInferenceEngine.cpp plugin/inference/EngineFactory.cpp \
  tests/test_cache.cpp tests/test_depth_ops.cpp tests/test_tiler.cpp \
  -o /tmp/zsoda_tests
/tmp/zsoda_tests
```
