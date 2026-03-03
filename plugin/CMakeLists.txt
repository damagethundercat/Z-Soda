cmake_minimum_required(VERSION 3.21)

option(ZSODA_WITH_ONNX_RUNTIME "Enable ONNX Runtime backend scaffold" OFF)
option(ZSODA_WITH_ONNX_RUNTIME_API "Enable ONNX Runtime C++ API execution path" OFF)
option(ZSODA_WITH_AE_SDK "Enable Adobe After Effects SDK entrypoint scaffolding" OFF)
option(ZSODA_AE_LOADER_ONLY_MODE "Build AE entrypoint in loader-only minimal mode (diagnostic)" OFF)
set(AE_SDK_ROOT "" CACHE PATH "Path to Adobe After Effects SDK root")
set(AE_SDK_INCLUDE_DIR "" CACHE PATH "Directory containing AE_Effect.h")
set(ONNXRUNTIME_INCLUDE_DIR "" CACHE PATH "Directory containing onnxruntime_cxx_api.h")
set(ONNXRUNTIME_LIBRARY "" CACHE FILEPATH "Path to ONNX Runtime library binary")
set(ZSODA_ONNXRUNTIME_DIRECT_LINK_MODE "OFF" CACHE STRING "ONNX Runtime direct link mode: AUTO, ON, OFF")
set_property(CACHE ZSODA_ONNXRUNTIME_DIRECT_LINK_MODE PROPERTY STRINGS AUTO ON OFF)
set(ZSODA_ONNXRUNTIME_DLL_PATH_HINT "" CACHE STRING "Optional runtime path hint for onnxruntime.dll")

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

string(TOUPPER "${ZSODA_ONNXRUNTIME_DIRECT_LINK_MODE}" ZSODA_ONNXRUNTIME_DIRECT_LINK_MODE_UPPER)
if(NOT ZSODA_ONNXRUNTIME_DIRECT_LINK_MODE_UPPER MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "ZSODA_ONNXRUNTIME_DIRECT_LINK_MODE must be AUTO, ON, or OFF")
endif()

set(ZSODA_ONNXRUNTIME_DIRECT_LINK OFF)
if(ZSODA_ONNXRUNTIME_DIRECT_LINK_MODE_UPPER STREQUAL "ON")
  set(ZSODA_ONNXRUNTIME_DIRECT_LINK ON)
elseif(ZSODA_ONNXRUNTIME_DIRECT_LINK_MODE_UPPER STREQUAL "AUTO")
  if(ZSODA_WITH_ONNX_RUNTIME_API)
    set(ZSODA_ONNXRUNTIME_DIRECT_LINK ON)
  endif()
endif()

set(ZSODA_ENABLE_ONNXRUNTIME_API OFF)
if(ZSODA_WITH_ONNX_RUNTIME_API)
  set(ZSODA_ENABLE_ONNXRUNTIME_API ON)
endif()

if(ZSODA_ENABLE_ONNXRUNTIME_API)
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
  if(ZSODA_ONNXRUNTIME_DIRECT_LINK)
    if(NOT ONNXRUNTIME_LIBRARY)
      message(FATAL_ERROR "Direct-link mode requires ONNXRUNTIME_LIBRARY")
    endif()
    if(NOT EXISTS "${ONNXRUNTIME_LIBRARY}")
      message(FATAL_ERROR "ONNXRUNTIME_LIBRARY not found: ${ONNXRUNTIME_LIBRARY}")
    endif()
  else()
    message(STATUS "ONNX Runtime API enabled with structural dynamic-load mode (no direct link).")
  endif()
endif()

set(ZSODA_ONNXRUNTIME_DLL_PATH_HINT_ESCAPED "${ZSODA_ONNXRUNTIME_DLL_PATH_HINT}")
string(REPLACE "\\" "\\\\" ZSODA_ONNXRUNTIME_DLL_PATH_HINT_ESCAPED "${ZSODA_ONNXRUNTIME_DLL_PATH_HINT_ESCAPED}")
string(REPLACE "\"" "\\\"" ZSODA_ONNXRUNTIME_DLL_PATH_HINT_ESCAPED "${ZSODA_ONNXRUNTIME_DLL_PATH_HINT_ESCAPED}")

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
    if(MSVC)
      target_compile_options(zsoda_aex PRIVATE /Zi)
      target_link_options(
        zsoda_aex
        PRIVATE
          /DEBUG:FULL
          /MAP:$<TARGET_FILE_DIR:zsoda_aex>/ZSoda.map
      )
    endif()

    set(ZSODA_PIPL_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/ae/ZSodaPiPL.r")
    if(NOT EXISTS "${ZSODA_PIPL_SOURCE}")
      message(FATAL_ERROR "Missing PiPL source file: ${ZSODA_PIPL_SOURCE}")
    endif()

    get_filename_component(AE_SDK_EXAMPLES_DIR "${AE_SDK_INCLUDE_DIR}" DIRECTORY)
    set(AE_PIPL_TOOL "${AE_SDK_EXAMPLES_DIR}/Resources/PiPLtool.exe")
    if(NOT EXISTS "${AE_PIPL_TOOL}")
      message(FATAL_ERROR "PiPLtool.exe not found under AE SDK examples resources: ${AE_PIPL_TOOL}")
    endif()

    set(ZSODA_PIPL_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/pipl")
    file(MAKE_DIRECTORY "${ZSODA_PIPL_BUILD_DIR}")
    set(ZSODA_PIPL_RR "${ZSODA_PIPL_BUILD_DIR}/ZSodaPiPL.rr")
    set(ZSODA_PIPL_RRC "${ZSODA_PIPL_BUILD_DIR}/ZSodaPiPL.rrc")
    set(ZSODA_PIPL_RC "${ZSODA_PIPL_BUILD_DIR}/ZSodaPiPL.rc")

    add_custom_command(
      OUTPUT "${ZSODA_PIPL_RC}"
      COMMAND "${CMAKE_CXX_COMPILER}" /nologo /P /EP /D AE_OS_WIN=1 /D MSWindows /D AE_PROC_INTELx64=1 /I "${AE_SDK_INCLUDE_DIR}" /Fi${ZSODA_PIPL_RR} "${ZSODA_PIPL_SOURCE}"
      COMMAND "${AE_PIPL_TOOL}" "${ZSODA_PIPL_RR}" "${ZSODA_PIPL_RRC}"
      COMMAND "${CMAKE_CXX_COMPILER}" /nologo /P /EP /D AE_OS_WIN=1 /D MSWindows /D AE_PROC_INTELx64=1 /Fi${ZSODA_PIPL_RC} "${ZSODA_PIPL_RRC}"
      DEPENDS "${ZSODA_PIPL_SOURCE}"
      VERBATIM
      COMMENT "Generating ZSoda PiPL resource"
    )
    set_source_files_properties("${ZSODA_PIPL_RC}" PROPERTIES GENERATED TRUE)
    target_sources(zsoda_aex PRIVATE "${ZSODA_PIPL_RC}")

    list(APPEND ZSODA_PLUGIN_TARGETS zsoda_aex)

    set(ZSODA_LOADER_PROBE_SOURCES
      ae/LoaderProbeEntry.cpp
    )
    add_library(zsoda_loader_probe_aex MODULE ${ZSODA_LOADER_PROBE_SOURCES})
    set_target_properties(zsoda_loader_probe_aex PROPERTIES OUTPUT_NAME "ZSodaLoaderProbe" PREFIX "" SUFFIX ".aex")
    if(MSVC)
      target_compile_options(zsoda_loader_probe_aex PRIVATE /Zi)
      target_link_options(
        zsoda_loader_probe_aex
        PRIVATE
          /DEBUG:FULL
          /MAP:$<TARGET_FILE_DIR:zsoda_loader_probe_aex>/ZSodaLoaderProbe.map
      )
    endif()

    set(ZSODA_LOADER_PROBE_PIPL_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/ae/ZSodaLoaderProbePiPL.r")
    if(NOT EXISTS "${ZSODA_LOADER_PROBE_PIPL_SOURCE}")
      message(FATAL_ERROR "Missing loader probe PiPL source file: ${ZSODA_LOADER_PROBE_PIPL_SOURCE}")
    endif()

    set(ZSODA_LOADER_PROBE_PIPL_RR "${ZSODA_PIPL_BUILD_DIR}/ZSodaLoaderProbePiPL.rr")
    set(ZSODA_LOADER_PROBE_PIPL_RRC "${ZSODA_PIPL_BUILD_DIR}/ZSodaLoaderProbePiPL.rrc")
    set(ZSODA_LOADER_PROBE_PIPL_RC "${ZSODA_PIPL_BUILD_DIR}/ZSodaLoaderProbePiPL.rc")

    add_custom_command(
      OUTPUT "${ZSODA_LOADER_PROBE_PIPL_RC}"
      COMMAND "${CMAKE_CXX_COMPILER}" /nologo /P /EP /D AE_OS_WIN=1 /D MSWindows /D AE_PROC_INTELx64=1 /I "${AE_SDK_INCLUDE_DIR}" /Fi${ZSODA_LOADER_PROBE_PIPL_RR} "${ZSODA_LOADER_PROBE_PIPL_SOURCE}"
      COMMAND "${AE_PIPL_TOOL}" "${ZSODA_LOADER_PROBE_PIPL_RR}" "${ZSODA_LOADER_PROBE_PIPL_RRC}"
      COMMAND "${CMAKE_CXX_COMPILER}" /nologo /P /EP /D AE_OS_WIN=1 /D MSWindows /D AE_PROC_INTELx64=1 /Fi${ZSODA_LOADER_PROBE_PIPL_RC} "${ZSODA_LOADER_PROBE_PIPL_RRC}"
      DEPENDS "${ZSODA_LOADER_PROBE_PIPL_SOURCE}"
      VERBATIM
      COMMENT "Generating ZSoda Loader Probe PiPL resource"
    )
    set_source_files_properties("${ZSODA_LOADER_PROBE_PIPL_RC}" PROPERTIES GENERATED TRUE)
    target_sources(zsoda_loader_probe_aex PRIVATE "${ZSODA_LOADER_PROBE_PIPL_RC}")

    target_include_directories(zsoda_loader_probe_aex
      PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${AE_SDK_INCLUDE_DIR}
    )
    target_compile_definitions(zsoda_loader_probe_aex PRIVATE ZSODA_WITH_AE_SDK=1)
    target_compile_features(zsoda_loader_probe_aex PRIVATE cxx_std_20)
  elseif(APPLE)
    add_library(zsoda_plugin_bundle MODULE ${ZSODA_PLUGIN_SOURCES})
    set_target_properties(
      zsoda_plugin_bundle
      PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "plugin"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ae/Info.plist.in"
        OUTPUT_NAME "ZSoda"
        PREFIX ""
    )
    list(APPEND ZSODA_PLUGIN_TARGETS zsoda_plugin_bundle)
  endif()
endif()

foreach(target_name IN LISTS ZSODA_PLUGIN_TARGETS)
  if(ZSODA_WITH_ONNX_RUNTIME)
    target_sources(${target_name}
      PRIVATE
        inference/OnnxRuntimeBackend.cpp
        inference/OrtDynamicLoader.cpp
    )
    target_compile_definitions(${target_name} PUBLIC ZSODA_WITH_ONNX_RUNTIME=1)
  endif()

  if(ZSODA_ENABLE_ONNXRUNTIME_API)
    target_include_directories(${target_name} PUBLIC "${ONNXRUNTIME_HEADER_DIR}")
    if(ZSODA_ONNXRUNTIME_DIRECT_LINK)
      target_link_libraries(${target_name} PUBLIC "${ONNXRUNTIME_LIBRARY}")
    endif()
    target_compile_definitions(${target_name} PUBLIC ZSODA_WITH_ONNX_RUNTIME_API=1)
  else()
    target_compile_definitions(${target_name} PUBLIC ZSODA_WITH_ONNX_RUNTIME_API=0)
  endif()

  target_compile_definitions(
    ${target_name}
    PUBLIC
      ZSODA_ONNXRUNTIME_DIRECT_LINK=$<BOOL:${ZSODA_ONNXRUNTIME_DIRECT_LINK}>
      ZSODA_ONNXRUNTIME_DLL_PATH_HINT=\"${ZSODA_ONNXRUNTIME_DLL_PATH_HINT_ESCAPED}\"
      ZSODA_ONNXRUNTIME_DLL_PATH_HINT_SET=$<BOOL:${ZSODA_ONNXRUNTIME_DLL_PATH_HINT}>
  )

  target_include_directories(${target_name}
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}
      $<$<BOOL:${ZSODA_WITH_AE_SDK}>:${AE_SDK_INCLUDE_DIR}>
  )

  target_compile_definitions(${target_name}
    PUBLIC
      ZSODA_WITH_AE_SDK=$<BOOL:${ZSODA_WITH_AE_SDK}>
      ZSODA_AE_LOADER_ONLY_MODE=$<BOOL:${ZSODA_AE_LOADER_ONLY_MODE}>
  )

  target_compile_features(${target_name} PUBLIC cxx_std_20)
endforeach()
