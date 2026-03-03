#!/usr/bin/env bash
set -euo pipefail

echo "[1/3] build+run default test suite"
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

echo "[2/3] build+run ONNX scaffold test suite"
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

echo "[2.5/3] compile ORT dynamic loader unit"
g++ -std=c++20 -Iplugin \
  -c plugin/inference/OrtDynamicLoader.cpp \
  -o /tmp/zsoda_ort_loader.o

echo "[3/3] build+run perf harness smoke checks"
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

echo "Local CI checks passed."
