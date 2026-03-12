cmake_minimum_required(VERSION 3.21)

if(WIN32 AND ZSODA_BUILD_INTERNAL_DIAGNOSTICS)
  add_executable(zsoda_still_quality_harness
    StillQualityHarness.cpp
  )

  target_link_libraries(zsoda_still_quality_harness
    PRIVATE
      zsoda_plugin
      windowscodecs
      ole32
  )

  target_compile_features(zsoda_still_quality_harness PRIVATE cxx_std_20)
endif()
