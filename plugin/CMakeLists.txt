cmake_minimum_required(VERSION 3.21)

add_library(zsoda_plugin
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

target_include_directories(zsoda_plugin
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(zsoda_plugin PUBLIC cxx_std_20)
