#include "ae/AeHostAdapter.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "ae/AeParams.h"
#include "core/RenderPipeline.h"
#include "inference/InferenceEngine.h"
#include "inference/ManagedInferenceEngine.h"

namespace zsoda::ae {
namespace {

constexpr int kGrayStubChannels = 4;

const char* SafeCStr(const char* value, const char* fallback = "<null>") {
  return value != nullptr ? value : fallback;
}

void AppendDiagnosticsLine(const char* tag, const char* detail);

std::optional<zsoda::core::PixelFormat> ParseHostPixelFormat(int pixel_format) {
  switch (pixel_format) {
    case static_cast<int>(zsoda::core::PixelFormat::kRGBA8):
      return zsoda::core::PixelFormat::kRGBA8;
    case static_cast<int>(zsoda::core::PixelFormat::kRGBA16):
      return zsoda::core::PixelFormat::kRGBA16;
    case static_cast<int>(zsoda::core::PixelFormat::kRGBA32F):
      return zsoda::core::PixelFormat::kRGBA32F;
    default:
      return std::nullopt;
  }
}

std::shared_ptr<zsoda::inference::IInferenceEngine> GetEngine() {
  static std::shared_ptr<zsoda::inference::IInferenceEngine> engine =
      zsoda::inference::CreateDefaultEngine();
  return engine;
}

void LogEngineStatusOnce() {
  static bool logged = false;
  if (logged) {
    return;
  }
  logged = true;

  const auto engine = GetEngine();
  if (engine == nullptr) {
    AppendDiagnosticsLine("EngineStatus", "engine is null");
    return;
  }

  std::string status = std::string("engine=") + SafeCStr(engine->Name(), "<null>");
  const auto managed_engine =
      std::dynamic_pointer_cast<zsoda::inference::ManagedInferenceEngine>(engine);
  if (managed_engine != nullptr) {
    status = managed_engine->BackendStatusString();
  }
  AppendDiagnosticsLine("EngineStatus", status.c_str());
}

std::shared_ptr<zsoda::core::RenderPipeline> GetPipeline() {
  static auto pipeline = std::make_shared<zsoda::core::RenderPipeline>(GetEngine());
  return pipeline;
}

zsoda::ae::AeCommandRouter& GetRouter() {
  static zsoda::ae::AeCommandRouter router(GetPipeline(), GetEngine());
  return router;
}

int Dispatch(const AeDispatchContext& dispatch) {
  return GetRouter().Handle(dispatch.command) ? 0 : -1;
}

#if defined(_WIN32)
void AppendDiagnosticsLine(const char* tag, const char* detail) {
  char temp_path[MAX_PATH] = {};
  const DWORD written = ::GetTempPathA(MAX_PATH, temp_path);
  if (written == 0 || written >= MAX_PATH) {
    return;
  }

  char log_path[MAX_PATH] = {};
  std::snprintf(log_path, sizeof(log_path), "%s%s", temp_path, "ZSoda_AE_Runtime.log");
  FILE* file = std::fopen(log_path, "ab");
  if (file == nullptr) {
    return;
  }

  SYSTEMTIME now = {};
  ::GetLocalTime(&now);
  const char* safe_tag = tag != nullptr ? tag : "<null>";
  const char* safe_detail = detail != nullptr ? detail : "<null>";
  std::fprintf(file,
               "%04u-%02u-%02u %02u:%02u:%02u.%03u | %s | %s\r\n",
               static_cast<unsigned>(now.wYear),
               static_cast<unsigned>(now.wMonth),
               static_cast<unsigned>(now.wDay),
               static_cast<unsigned>(now.wHour),
               static_cast<unsigned>(now.wMinute),
               static_cast<unsigned>(now.wSecond),
               static_cast<unsigned>(now.wMilliseconds),
               safe_tag,
               safe_detail);
  std::fclose(file);
}

void LogSehException(const char* entrypoint, unsigned int code) {
  char message[128] = {};
  std::snprintf(message, sizeof(message), "SEH exception code=0x%08X", code);
  AppendDiagnosticsLine(entrypoint, message);
}
#else
void AppendDiagnosticsLine(const char* /*tag*/, const char* /*detail*/) {}
void LogSehException(const char* /*entrypoint*/, unsigned int /*code*/) {}
#endif

}  // namespace
}  // namespace zsoda::ae

namespace {

int ZSodaRenderHostBufferStubImpl(const void* src,
                                  int width,
                                  int height,
                                  int src_row_bytes,
                                  int pixel_format,
                                  std::uint64_t frame_hash,
                                  void* out,
                                  int out_row_bytes);

int ZSodaEffectMainStubImpl(int command_id) {
  std::string error;
  zsoda::ae::AeDispatchContext dispatch;
  if (!zsoda::ae::BuildStubDispatch(command_id, &dispatch, &error)) {
    return -1;
  }
  return zsoda::ae::Dispatch(dispatch);
}

int ZSodaSetModelIdStubImpl(const char* model_id) {
  if (model_id == nullptr) {
    return -1;
  }
  auto params = zsoda::ae::GetRouter().CurrentParams();
  params.model_id = model_id;
  std::string error;
  zsoda::ae::AeCommandContext context;
  context.command = zsoda::ae::AeCommand::kUpdateParams;
  context.params_update = &params;
  context.error = &error;
  return zsoda::ae::GetRouter().Handle(context) ? 0 : -1;
}

int ZSodaSetParamsStubImpl(const char* model_id,
                           int quality,
                           int output_mode,
                           int invert,
                           float min_depth,
                           float max_depth,
                           float softness,
                           int cache_enabled,
                           int tile_size,
                           int overlap,
                           int vram_budget_mb) {
  auto params = zsoda::ae::GetRouter().CurrentParams();
  if (model_id != nullptr && model_id[0] != '\0') {
    params.model_id = model_id;
  }
  params.quality = quality;
  params.output_mode =
      output_mode == static_cast<int>(zsoda::ae::AeOutputMode::kSlicing)
          ? zsoda::ae::AeOutputMode::kSlicing
          : zsoda::ae::AeOutputMode::kDepthMap;
  params.invert = invert != 0;
  params.min_depth = min_depth;
  params.max_depth = max_depth;
  params.softness = softness;
  params.cache_enabled = cache_enabled != 0;
  params.tile_size = tile_size;
  params.overlap = overlap;
  params.vram_budget_mb = vram_budget_mb;

  std::string error;
  zsoda::ae::AeCommandContext context;
  context.command = zsoda::ae::AeCommand::kUpdateParams;
  context.params_update = &params;
  context.error = &error;
  return zsoda::ae::GetRouter().Handle(context) ? 0 : -1;
}

int ZSodaRenderGrayFrameStubImpl(const float* src,
                                 int width,
                                 int height,
                                 std::uint64_t frame_hash,
                                 float* out,
                                 int out_size) {
  if (src == nullptr || out == nullptr || width <= 0 || height <= 0) {
    return -1;
  }
  const std::size_t pixel_count =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (out_size < 0 || static_cast<std::size_t>(out_size) < pixel_count) {
    return -1;
  }

  std::vector<float> rgba_source(pixel_count * zsoda::ae::kGrayStubChannels, 0.0F);
  std::vector<float> rgba_output(pixel_count * zsoda::ae::kGrayStubChannels, 0.0F);
  for (std::size_t i = 0; i < pixel_count; ++i) {
    const float gray = src[i];
    const std::size_t offset = i * zsoda::ae::kGrayStubChannels;
    rgba_source[offset + 0] = gray;
    rgba_source[offset + 1] = gray;
    rgba_source[offset + 2] = gray;
    rgba_source[offset + 3] = 1.0F;
  }

  const std::size_t row_bytes_size =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(zsoda::ae::kGrayStubChannels) *
      sizeof(float);
  if (row_bytes_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return -1;
  }
  const int row_bytes = static_cast<int>(row_bytes_size);
  if (ZSodaRenderHostBufferStubImpl(rgba_source.data(),
                                    width,
                                    height,
                                    row_bytes,
                                    static_cast<int>(zsoda::core::PixelFormat::kRGBA32F),
                                    frame_hash,
                                    rgba_output.data(),
                                    row_bytes) != 0) {
    return -1;
  }

  for (std::size_t i = 0; i < pixel_count; ++i) {
    out[i] = rgba_output[i * zsoda::ae::kGrayStubChannels];
  }
  return 0;
}

int ZSodaRenderHostBufferStubImpl(const void* src,
                                  int width,
                                  int height,
                                  int src_row_bytes,
                                  int pixel_format,
                                  std::uint64_t frame_hash,
                                  void* out,
                                  int out_row_bytes) {
  if (src == nullptr || out == nullptr || width <= 0 || height <= 0 || src_row_bytes <= 0 ||
      out_row_bytes <= 0) {
    return -1;
  }

  const auto format = zsoda::ae::ParseHostPixelFormat(pixel_format);
  if (!format.has_value()) {
    return -1;
  }

  zsoda::ae::AeHostRenderBridgePayload payload;
  payload.source.pixels = src;
  payload.source.width = width;
  payload.source.height = height;
  payload.source.row_bytes = static_cast<std::size_t>(src_row_bytes);
  payload.source.format = *format;
  payload.destination.pixels = out;
  payload.destination.width = width;
  payload.destination.height = height;
  payload.destination.row_bytes = static_cast<std::size_t>(out_row_bytes);
  payload.destination.format = *format;
  payload.frame_hash = frame_hash;

  std::string error;
  return zsoda::ae::ExecuteHostBufferRenderBridge(&zsoda::ae::GetRouter(), payload, nullptr, &error)
             ? 0
             : -1;
}

}  // namespace

int ZSodaEffectMainStubGuarded(int command_id) {
  try {
    return ZSodaEffectMainStubImpl(command_id);
  } catch (const std::exception& ex) {
    zsoda::ae::AppendDiagnosticsLine("ZSodaEffectMainStub", ex.what());
    return -1;
  } catch (...) {
    zsoda::ae::AppendDiagnosticsLine("ZSodaEffectMainStub", "unknown c++ exception");
    return -1;
  }
}

int ZSodaSetModelIdStubGuarded(const char* model_id) {
  try {
    return ZSodaSetModelIdStubImpl(model_id);
  } catch (const std::exception& ex) {
    zsoda::ae::AppendDiagnosticsLine("ZSodaSetModelIdStub", ex.what());
    return -1;
  } catch (...) {
    zsoda::ae::AppendDiagnosticsLine("ZSodaSetModelIdStub", "unknown c++ exception");
    return -1;
  }
}

int ZSodaSetParamsStubGuarded(const char* model_id,
                              int quality,
                              int output_mode,
                              int invert,
                              float min_depth,
                              float max_depth,
                              float softness,
                              int cache_enabled,
                              int tile_size,
                              int overlap,
                              int vram_budget_mb) {
  try {
    return ZSodaSetParamsStubImpl(model_id,
                                  quality,
                                  output_mode,
                                  invert,
                                  min_depth,
                                  max_depth,
                                  softness,
                                  cache_enabled,
                                  tile_size,
                                  overlap,
                                  vram_budget_mb);
  } catch (const std::exception& ex) {
    zsoda::ae::AppendDiagnosticsLine("ZSodaSetParamsStub", ex.what());
    return -1;
  } catch (...) {
    zsoda::ae::AppendDiagnosticsLine("ZSodaSetParamsStub", "unknown c++ exception");
    return -1;
  }
}

int ZSodaRenderGrayFrameStubGuarded(const float* src,
                                    int width,
                                    int height,
                                    std::uint64_t frame_hash,
                                    float* out,
                                    int out_size) {
  try {
    return ZSodaRenderGrayFrameStubImpl(src, width, height, frame_hash, out, out_size);
  } catch (const std::exception& ex) {
    zsoda::ae::AppendDiagnosticsLine("ZSodaRenderGrayFrameStub", ex.what());
    return -1;
  } catch (...) {
    zsoda::ae::AppendDiagnosticsLine("ZSodaRenderGrayFrameStub", "unknown c++ exception");
    return -1;
  }
}

int ZSodaRenderHostBufferStubGuarded(const void* src,
                                     int width,
                                     int height,
                                     int src_row_bytes,
                                     int pixel_format,
                                     std::uint64_t frame_hash,
                                     void* out,
                                     int out_row_bytes) {
  try {
    return ZSodaRenderHostBufferStubImpl(
        src, width, height, src_row_bytes, pixel_format, frame_hash, out, out_row_bytes);
  } catch (const std::exception& ex) {
    zsoda::ae::AppendDiagnosticsLine("ZSodaRenderHostBufferStub", ex.what());
    return -1;
  } catch (...) {
    zsoda::ae::AppendDiagnosticsLine("ZSodaRenderHostBufferStub", "unknown c++ exception");
    return -1;
  }
}

extern "C" int ZSodaEffectMainStub(int command_id) {
#if defined(_WIN32) && defined(_MSC_VER)
  __try {
    return ZSodaEffectMainStubGuarded(command_id);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    zsoda::ae::LogSehException("ZSodaEffectMainStub", static_cast<unsigned>(GetExceptionCode()));
    return -1;
  }
#else
  return ZSodaEffectMainStubGuarded(command_id);
#endif
}

extern "C" int ZSodaSetModelIdStub(const char* model_id) {
#if defined(_WIN32) && defined(_MSC_VER)
  __try {
    return ZSodaSetModelIdStubGuarded(model_id);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    zsoda::ae::LogSehException("ZSodaSetModelIdStub", static_cast<unsigned>(GetExceptionCode()));
    return -1;
  }
#else
  return ZSodaSetModelIdStubGuarded(model_id);
#endif
}

extern "C" int ZSodaSetParamsStub(const char* model_id,
                                  int quality,
                                  int output_mode,
                                  int invert,
                                  float min_depth,
                                  float max_depth,
                                  float softness,
                                  int cache_enabled,
                                  int tile_size,
                                  int overlap,
                                  int vram_budget_mb) {
#if defined(_WIN32) && defined(_MSC_VER)
  __try {
    return ZSodaSetParamsStubGuarded(model_id,
                                     quality,
                                     output_mode,
                                     invert,
                                     min_depth,
                                     max_depth,
                                     softness,
                                     cache_enabled,
                                     tile_size,
                                     overlap,
                                     vram_budget_mb);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    zsoda::ae::LogSehException("ZSodaSetParamsStub", static_cast<unsigned>(GetExceptionCode()));
    return -1;
  }
#else
  return ZSodaSetParamsStubGuarded(model_id,
                                   quality,
                                   output_mode,
                                   invert,
                                   min_depth,
                                   max_depth,
                                   softness,
                                   cache_enabled,
                                   tile_size,
                                   overlap,
                                   vram_budget_mb);
#endif
}

extern "C" int ZSodaRenderGrayFrameStub(const float* src,
                                        int width,
                                        int height,
                                        std::uint64_t frame_hash,
                                        float* out,
                                        int out_size) {
#if defined(_WIN32) && defined(_MSC_VER)
  __try {
    return ZSodaRenderGrayFrameStubGuarded(src, width, height, frame_hash, out, out_size);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    zsoda::ae::LogSehException("ZSodaRenderGrayFrameStub", static_cast<unsigned>(GetExceptionCode()));
    return -1;
  }
#else
  return ZSodaRenderGrayFrameStubGuarded(src, width, height, frame_hash, out, out_size);
#endif
}

extern "C" int ZSodaRenderHostBufferStub(const void* src,
                                         int width,
                                         int height,
                                         int src_row_bytes,
                                         int pixel_format,
                                         std::uint64_t frame_hash,
                                         void* out,
                                         int out_row_bytes) {
#if defined(_WIN32) && defined(_MSC_VER)
  __try {
    return ZSodaRenderHostBufferStubGuarded(
        src, width, height, src_row_bytes, pixel_format, frame_hash, out, out_row_bytes);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    zsoda::ae::LogSehException("ZSodaRenderHostBufferStub",
                               static_cast<unsigned>(GetExceptionCode()));
    return -1;
  }
#else
  return ZSodaRenderHostBufferStubGuarded(
      src, width, height, src_row_bytes, pixel_format, frame_hash, out, out_row_bytes);
#endif
}

#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK

#ifndef DllExport
#if defined(_WIN32)
#define DllExport __declspec(dllexport)
#else
#define DllExport
#endif
#endif

PF_Err EffectMainImpl(PF_Cmd cmd,
                      PF_InData* in_data,
                      PF_OutData* out_data,
                      PF_ParamDef* params[],
                      PF_LayerDef* output,
                      void* extra) {
  LogEngineStatusOnce();
  if (cmd != PF_Cmd_RENDER) {
    const std::string cmd_detail = "cmd=" + std::to_string(static_cast<int>(cmd));
    zsoda::ae::AppendDiagnosticsLine("EffectMainCmd", cmd_detail.c_str());
  }

  std::string error;

  zsoda::ae::AeDispatchContext dispatch;
  zsoda::ae::AeSdkEntryPayload payload;
  payload.command = cmd;
  payload.in_data = in_data;
  payload.out_data = out_data;
  payload.params = params;
  payload.output = output;
  payload.extra = extra;

  if (!zsoda::ae::BuildSdkDispatch(payload, &dispatch, &error)) {
    const std::string detail =
        "BuildSdkDispatch failed: cmd=" + std::to_string(static_cast<int>(cmd)) +
        ", error=" + (error.empty() ? "<none>" : error);
    zsoda::ae::AppendDiagnosticsLine("EffectMain", detail.c_str());
    return PF_Err_NONE;
  }
  if (!error.empty() && cmd != PF_Cmd_RENDER) {
    const std::string detail =
        "BuildSdkDispatch warning: cmd=" + std::to_string(static_cast<int>(cmd)) +
        ", detail=" + error;
    zsoda::ae::AppendDiagnosticsLine("EffectMain", detail.c_str());
  }

  if (dispatch.command.command == zsoda::ae::AeCommand::kUnknown) {
    // Skeleton entrypoint ignores unsupported commands until individual handlers
    // are wired.
    return PF_Err_NONE;
  }

  if (zsoda::ae::Dispatch(dispatch) == 0) {
    return PF_Err_NONE;
  }

  const std::string detail =
      "Dispatch failed: cmd=" + std::to_string(static_cast<int>(cmd)) +
      ", mapped=" + std::to_string(static_cast<int>(dispatch.command.command)) +
      ", error=" + (error.empty() ? "<none>" : error);
  zsoda::ae::AppendDiagnosticsLine("EffectMain", detail.c_str());
  return PF_Err_NONE;
}

PF_Err EffectMainGuarded(PF_Cmd cmd,
                         PF_InData* in_data,
                         PF_OutData* out_data,
                         PF_ParamDef* params[],
                         PF_LayerDef* output,
                         void* extra) {
  try {
    return EffectMainImpl(cmd, in_data, out_data, params, output, extra);
  } catch (const std::exception& ex) {
    zsoda::ae::AppendDiagnosticsLine("EffectMain", ex.what());
    return PF_Err_NONE;
  } catch (...) {
    zsoda::ae::AppendDiagnosticsLine("EffectMain", "unknown c++ exception");
    return PF_Err_NONE;
  }
}

extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd,
                                       PF_InData* in_data,
                                       PF_OutData* out_data,
                                       PF_ParamDef* params[],
                                       PF_LayerDef* output,
                                       void* extra) {
#if defined(_WIN32) && defined(_MSC_VER)
  __try {
    return EffectMainGuarded(cmd, in_data, out_data, params, output, extra);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    zsoda::ae::LogSehException("EffectMain", static_cast<unsigned>(GetExceptionCode()));
    return PF_Err_NONE;
  }
#else
  return EffectMainGuarded(cmd, in_data, out_data, params, output, extra);
#endif
}

#endif
