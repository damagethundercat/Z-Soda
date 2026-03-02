cmake_minimum_required(VERSION 3.21)

option(ZSODA_WITH_ONNX_RUNTIME "Enable ONNX Runtime backend scaffold" OFF)
option(ZSODA_WITH_ONNX_RUNTIME_API "Enable ONNX Runtime C++ API execution path" OFF)
option(ZSODA_WITH_AE_SDK "Enable Adobe After Effects SDK entrypoint scaffolding" OFF)
set(AE_SDK_ROOT "" CACHE PATH "Path to Adobe After Effects SDK root")
set(AE_SDK_INCLUDE_DIR "" CACHE PATH "Directory containing AE_Effect.h")
set(ONNXRUNTIME_INCLUDE_DIR "" CACHE PATH "Directory containing onnxruntime_cxx_api.h")
set(ONNXRUNTIME_LIBRARY "" CACHE FILEPATH "Path to ONNX Runtime library binary")

set(ZSODA_PLUGIN_SOURCES
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

add_library(zsoda_plugin STATIC ${ZSODA_PLUGIN_SOURCES})
set(ZSODA_PLUGIN_TARGETS zsoda_plugin)

if(ZSODA_WITH_ONNX_RUNTIME_API)
  if(NOT ZSODA_WITH_ONNX_RUNTIME)
    message(FATAL_ERROR "ZSODA_WITH_ONNX_RUNTIME_API=ON requires ZSODA_WITH_ONNX_RUNTIME=ON")
  endif()
  if(NOT ONNXRUNTIME_INCLUDE_DIR)
    message(FATAL_ERROR "ZSODA_WITH_ONNX_RUNTIME_API=ON requires ONNXRUNTIME_INCLUDE_DIR")
  endif()
  set(ONNXRUNTIME_HEADER_DIR "")
  if(EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_cxx_api.h")
    set(ONNXRUNTIME_HEADER_DIR "${ONNXRUNTIME_INCLUDE_DIR}")
  elseif(EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime/core/session/onnxruntime_cxx_api.h")
    set(ONNXRUNTIME_HEADER_DIR "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime/core/session")
  endif()
  if(NOT ONNXRUNTIME_HEADER_DIR)
    message(FATAL_ERROR "ONNXRUNTIME_INCLUDE_DIR must contain onnxruntime_cxx_api.h (directly or under onnxruntime/core/session): ${ONNXRUNTIME_INCLUDE_DIR}")
  endif()
  if(NOT ONNXRUNTIME_LIBRARY)
    message(FATAL_ERROR "ZSODA_WITH_ONNX_RUNTIME_API=ON requires ONNXRUNTIME_LIBRARY")
  endif()
  if(NOT EXISTS "${ONNXRUNTIME_LIBRARY}")
    message(FATAL_ERROR "ONNXRUNTIME_LIBRARY not found: ${ONNXRUNTIME_LIBRARY}")
  endif()
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

  if(WIN32)
    add_library(zsoda_aex MODULE ${ZSODA_PLUGIN_SOURCES})
    set_target_properties(zsoda_aex PROPERTIES OUTPUT_NAME "ZSoda" PREFIX "" SUFFIX ".aex")
    list(APPEND ZSODA_PLUGIN_TARGETS zsoda_aex)
  elseif(APPLE)
    add_library(zsoda_plugin_bundle MODULE ${ZSODA_PLUGIN_SOURCES})
    set_target_properties(zsoda_plugin_bundle PROPERTIES OUTPUT_NAME "ZSoda" PREFIX "")
    list(APPEND ZSODA_PLUGIN_TARGETS zsoda_plugin_bundle)
  endif()
endif()

foreach(target_name IN LISTS ZSODA_PLUGIN_TARGETS)
  if(ZSODA_WITH_ONNX_RUNTIME)
    target_sources(${target_name}
      PRIVATE
        inference/OnnxRuntimeBackend.cpp
    )
    target_compile_definitions(${target_name} PUBLIC ZSODA_WITH_ONNX_RUNTIME=1)
  endif()

  if(ZSODA_WITH_ONNX_RUNTIME_API)
    target_include_directories(${target_name} PUBLIC "${ONNXRUNTIME_HEADER_DIR}")
    target_link_libraries(${target_name} PUBLIC "${ONNXRUNTIME_LIBRARY}")
    target_compile_definitions(${target_name} PUBLIC ZSODA_WITH_ONNX_RUNTIME_API=1)
  endif()

  target_include_directories(${target_name}
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}
      $<$<BOOL:${ZSODA_WITH_AE_SDK}>:${AE_SDK_INCLUDE_DIR}>
  )

  target_compile_definitions(${target_name}
    PUBLIC
      ZSODA_WITH_AE_SDK=$<BOOL:${ZSODA_WITH_AE_SDK}>
  )

  target_compile_features(${target_name} PUBLIC cxx_std_20)
endforeach()
