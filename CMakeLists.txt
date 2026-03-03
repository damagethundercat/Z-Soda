cmake_minimum_required(VERSION 3.21)

if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()

project(z_soda LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  set(ZSODA_MSVC_RUNTIME_LIBRARY
      "MultiThreaded$<$<CONFIG:Debug>:Debug>"
      CACHE STRING
      "MSVC runtime library mode for Z-Soda targets (e.g. MultiThreaded, MultiThreadedDLL)")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "${ZSODA_MSVC_RUNTIME_LIBRARY}")
endif()

option(ZSODA_BUILD_TESTS "Build unit tests" ON)

add_subdirectory(plugin)

if(ZSODA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
