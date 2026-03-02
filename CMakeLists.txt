cmake_minimum_required(VERSION 3.21)
project(z_soda LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ZSODA_BUILD_TESTS "Build unit tests" ON)

add_subdirectory(plugin)

if(ZSODA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
