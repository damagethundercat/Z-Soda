cmake_minimum_required(VERSION 3.21)

function(add_zsoda_test target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE zsoda_plugin)
  target_compile_features(${target} PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(${target} PRIVATE "$<$<CONFIG:Release>:/UNDEBUG>")
  else()
    target_compile_options(${target} PRIVATE "$<$<CONFIG:Release>:-UNDEBUG>")
  endif()
  add_test(NAME ${target} COMMAND ${target})
endfunction()

set(ZSODA_TEST_SUPPORT
  TestSupport.cpp
)

add_zsoda_test(zsoda_core_tests
  ${ZSODA_TEST_SUPPORT}
  test_core_main.cpp
  test_cache.cpp
  test_depth_ops.cpp
  test_runtime_path_resolver.cpp
  test_tiler.cpp
)

add_zsoda_test(zsoda_ae_params_tests
  ${ZSODA_TEST_SUPPORT}
  test_ae_params_main.cpp
  test_ae_params.cpp
)

add_zsoda_test(zsoda_ae_router_tests
  ${ZSODA_TEST_SUPPORT}
  test_ae_router_main.cpp
  test_ae_router.cpp
)

add_zsoda_test(zsoda_inference_tests
  ${ZSODA_TEST_SUPPORT}
  test_inference_main.cpp
  test_inference_engine.cpp
)

add_zsoda_test(zsoda_render_tests
  ${ZSODA_TEST_SUPPORT}
  test_render_main.cpp
  test_render_pipeline.cpp
)

set_tests_properties(
  zsoda_core_tests
  zsoda_ae_params_tests
  zsoda_ae_router_tests
  zsoda_inference_tests
  zsoda_render_tests
  PROPERTIES LABELS "unit"
)

add_executable(zsoda_embedded_payload_tests
  test_embedded_payload.cpp
  test_embedded_payload_main.cpp
)

target_link_libraries(zsoda_embedded_payload_tests PRIVATE zsoda_plugin)
target_compile_features(zsoda_embedded_payload_tests PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(zsoda_embedded_payload_tests PRIVATE "$<$<CONFIG:Release>:/UNDEBUG>")
else()
  target_compile_options(zsoda_embedded_payload_tests PRIVATE "$<$<CONFIG:Release>:-UNDEBUG>")
endif()

add_test(NAME zsoda_embedded_payload_tests COMMAND zsoda_embedded_payload_tests)

add_executable(zsoda_python_autostart_tests
  test_python_autostart.cpp
)

target_link_libraries(zsoda_python_autostart_tests PRIVATE zsoda_plugin)
target_compile_features(zsoda_python_autostart_tests PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(zsoda_python_autostart_tests PRIVATE "$<$<CONFIG:Release>:/UNDEBUG>")
else()
  target_compile_options(zsoda_python_autostart_tests PRIVATE "$<$<CONFIG:Release>:-UNDEBUG>")
endif()

add_test(NAME zsoda_python_autostart_tests COMMAND zsoda_python_autostart_tests)

set_tests_properties(
  zsoda_embedded_payload_tests
  zsoda_python_autostart_tests
  PROPERTIES LABELS "unit"
)

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
