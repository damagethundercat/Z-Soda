cmake_minimum_required(VERSION 3.21)

add_executable(zsoda_tests
  test_ae_params.cpp
  test_ae_router.cpp
  test_cache.cpp
  test_depth_ops.cpp
  test_inference_engine.cpp
  test_render_pipeline.cpp
  test_runtime_path_resolver.cpp
  test_tiler.cpp
)

target_link_libraries(zsoda_tests PRIVATE zsoda_plugin)
target_compile_features(zsoda_tests PRIVATE cxx_std_20)

add_test(NAME zsoda_tests COMMAND zsoda_tests)

add_executable(zsoda_perf_harness
  perf_harness.cpp
)

target_link_libraries(zsoda_perf_harness PRIVATE zsoda_plugin)
target_compile_features(zsoda_perf_harness PRIVATE cxx_std_20)

add_test(
  NAME zsoda_perf_benchmark
  COMMAND zsoda_perf_harness --mode benchmark --frames 180 --width 160 --height 90 --cache-cycle 12 --quiet
)

add_test(
  NAME zsoda_perf_stability
  COMMAND zsoda_perf_harness --mode stability --frames 1200 --width 64 --height 36 --quiet
)
