cmake_minimum_required(VERSION 3.21)

if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()

if(APPLE)
  if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET
        "12.0"
        CACHE STRING
        "Minimum macOS deployment target for Z-Soda builds")
  endif()
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
option(ZSODA_BUILD_TOOLS "Build developer tools" ON)
option(ZSODA_BUILD_INTERNAL_DIAGNOSTICS "Build internal diagnostics utilities" OFF)

add_subdirectory(plugin)

if(ZSODA_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(ZSODA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
