cmake_minimum_required(VERSION 3.21)

add_executable(zsoda_tests
  test_ae_params.cpp
  test_ae_router.cpp
  test_cache.cpp
  test_depth_ops.cpp
  test_inference_engine.cpp
  test_render_pipeline.cpp
  test_tiler.cpp
)

target_link_libraries(zsoda_tests PRIVATE zsoda_plugin)
target_compile_features(zsoda_tests PRIVATE cxx_std_20)

add_test(NAME zsoda_tests COMMAND zsoda_tests)
