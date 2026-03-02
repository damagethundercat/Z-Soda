cmake_minimum_required(VERSION 3.21)

option(ZSODA_WITH_ONNX_RUNTIME "Enable ONNX Runtime backend scaffold" OFF)
option(ZSODA_WITH_AE_SDK "Enable Adobe After Effects SDK entrypoint scaffolding" OFF)
set(AE_SDK_ROOT "" CACHE PATH "Path to Adobe After Effects SDK root")
set(AE_SDK_INCLUDE_DIR "" CACHE PATH "Directory containing AE_Effect.h")

add_library(zsoda_plugin
  ae/AeHostAdapter.cpp
  ae/AeCommandRouter.cpp
  ae/AeParams.cpp
  ae/AePluginEntry.cpp
  core/BufferPool.cpp
  core/Cache.cpp
  core/DepthOps.cpp
  core/RenderPipeline.cpp
  core/Tiler.cpp
  inference/DummyInferenceEngine.cpp
  inference/EngineFactory.cpp
  inference/ManagedInferenceEngine.cpp
  inference/ModelCatalog.cpp
)

if(ZSODA_WITH_ONNX_RUNTIME)
  target_sources(zsoda_plugin
    PRIVATE
      inference/OnnxRuntimeBackend.cpp
  )
  target_compile_definitions(zsoda_plugin PUBLIC ZSODA_WITH_ONNX_RUNTIME=1)
endif()

if(ZSODA_WITH_AE_SDK)
  if(NOT AE_SDK_INCLUDE_DIR)
    if(AE_SDK_ROOT AND EXISTS "${AE_SDK_ROOT}/Examples/Headers/AE_Effect.h")
      set(AE_SDK_INCLUDE_DIR "${AE_SDK_ROOT}/Examples/Headers")
    elseif(AE_SDK_ROOT AND EXISTS "${AE_SDK_ROOT}/Headers/AE_Effect.h")
      set(AE_SDK_INCLUDE_DIR "${AE_SDK_ROOT}/Headers")
    endif()
  endif()

  if(NOT AE_SDK_INCLUDE_DIR)
    message(FATAL_ERROR "ZSODA_WITH_AE_SDK=ON requires AE_SDK_INCLUDE_DIR (or AE_SDK_ROOT) containing AE_Effect.h")
  endif()
endif()

target_include_directories(zsoda_plugin
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    $<$<BOOL:${ZSODA_WITH_AE_SDK}>:${AE_SDK_INCLUDE_DIR}>
)

target_compile_definitions(zsoda_plugin
  PUBLIC
    ZSODA_WITH_AE_SDK=$<BOOL:${ZSODA_WITH_AE_SDK}>
)

target_compile_features(zsoda_plugin PUBLIC cxx_std_20)
