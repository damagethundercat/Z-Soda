#include "ae/AeHostAdapter.h"
#include "ae/ZSodaAeFlags.h"
#include "ae/ZSodaVersion.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK
#include "AE_PluginData.h"
#endif

namespace zsoda::ae {
namespace {

constexpr int kGrayStubChannels = 4;

const char* SafeCStr(const char* value, const char* fallback = "<null>") {
  return value != nullptr ? value : fallback;
}

void AppendDiagnosticsLine(const char* tag, const char* detail);

std::atomic<std::uint64_t> g_render_trace_seq{0U};

std::uint64_t NextRenderTraceId() {
  return g_render_trace_seq.fetch_add(1U, std::memory_order_relaxed) + 1U;
}

void LogRenderStage(std::uint64_t trace_id, const char* stage, const char* detail = nullptr) {
  char message[768] = {};
#if defined(_WIN32)
  const unsigned long thread_id = static_cast<unsigned long>(::GetCurrentThreadId());
#else
  const unsigned long thread_id = 0UL;
#endif
  const char* safe_stage = SafeCStr(stage, "<null>");
  const char* safe_detail = (detail != nullptr && detail[0] != '\0') ? detail : "<none>";
  std::snprintf(message,
                sizeof(message),
                "trace=%llu, tid=%lu, stage=%s, detail=%s",
                static_cast<unsigned long long>(trace_id),
                thread_id,
                safe_stage,
                safe_detail);
  AppendDiagnosticsLine("EffectMainRenderTrace", message);
}

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

#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK
bool TryCopyRenderInputToOutput(PF_ParamDef* params[], PF_LayerDef* output) {
  if (params == nullptr || params[0] == nullptr || output == nullptr || output->data == nullptr) {
    return false;
  }

  const PF_LayerDef* input = &params[0]->u.ld;
  if (input == nullptr || input->data == nullptr) {
    return false;
  }
  if (input->height <= 0 || input->rowbytes <= 0 || output->height <= 0 || output->rowbytes <= 0) {
    return false;
  }

  const A_long rows = std::min(input->height, output->height);
  const A_long bytes_to_copy = std::min(input->rowbytes, output->rowbytes);
  if (rows <= 0 || bytes_to_copy <= 0) {
    return false;
  }

  auto* dst = reinterpret_cast<std::uint8_t*>(output->data);
  const auto* src = reinterpret_cast<const std::uint8_t*>(input->data);
  for (A_long y = 0; y < rows; ++y) {
    std::memcpy(dst, src, static_cast<std::size_t>(bytes_to_copy));
    dst += output->rowbytes;
    src += input->rowbytes;
  }
  return true;
}

void TryApplyRenderPassThrough(PF_Cmd cmd,
                               PF_ParamDef* params[],
                               PF_LayerDef* output,
                               const char* reason) {
  if (cmd != PF_Cmd_RENDER) {
    return;
  }
  const bool copied = TryCopyRenderInputToOutput(params, output);
  if (copied) {
    std::string detail = "render fallback pass-through applied";
    if (reason != nullptr && reason[0] != '\0') {
      detail += ": ";
      detail += reason;
    }
    zsoda::ae::AppendDiagnosticsLine("EffectMain", detail.c_str());
  } else {
    std::string detail = "render fallback pass-through skipped (invalid input/output)";
    if (reason != nullptr && reason[0] != '\0') {
      detail += ": ";
      detail += reason;
    }
    zsoda::ae::AppendDiagnosticsLine("EffectMain", detail.c_str());
  }
}

PF_Err RunLoaderOnlyEffectMain(PF_Cmd cmd,
                               PF_InData* in_data,
                               PF_OutData* out_data,
                               PF_ParamDef* params[],
                               PF_LayerDef* output,
                               void* extra) {
  (void)in_data;
  (void)extra;

  switch (cmd) {
    case PF_Cmd_ABOUT:
      if (out_data != nullptr) {
        std::snprintf(out_data->return_msg,
                      sizeof(out_data->return_msg),
                      "ZSoda Loader-Only Mode");
      }
      return PF_Err_NONE;
    case PF_Cmd_GLOBAL_SETUP:
      if (out_data != nullptr) {
        out_data->my_version = static_cast<A_u_long>(ZSODA_EFFECT_VERSION_HEX);
        out_data->out_flags = static_cast<A_long>(ZSODA_AE_GLOBAL_OUTFLAGS);
        out_data->out_flags2 = static_cast<A_long>(ZSODA_AE_GLOBAL_OUTFLAGS2);
      }
      return PF_Err_NONE;
    case PF_Cmd_PARAMS_SETUP:
      if (out_data != nullptr) {
        out_data->my_version = static_cast<A_u_long>(ZSODA_EFFECT_VERSION_HEX);
        out_data->out_flags = static_cast<A_long>(ZSODA_AE_GLOBAL_OUTFLAGS);
        out_data->out_flags2 = static_cast<A_long>(ZSODA_AE_GLOBAL_OUTFLAGS2);
        out_data->num_params = 1;
      }
      return PF_Err_NONE;
    case PF_Cmd_RENDER: {
      (void)TryCopyRenderInputToOutput(params, output);
      return PF_Err_NONE;
    }
    default:
      return PF_Err_NONE;
  }
}
#endif

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

extern "C" DllExport PF_Err PluginDataEntryFunction2(PF_PluginDataPtr in_ptr,
                                                      PF_PluginDataCB2 in_plugin_data_callback_ptr,
                                                      SPBasicSuite* in_sp_basic_suite_ptr,
                                                      const char* in_host_name,
                                                      const char* in_host_version) {
  constexpr A_long kAeReservedInfo = 8;
  (void)in_sp_basic_suite_ptr;
  (void)in_host_name;
  (void)in_host_version;

  if (in_plugin_data_callback_ptr == nullptr) {
    return PF_Err_INVALID_CALLBACK;
  }

  const A_Err result =
      (*in_plugin_data_callback_ptr)(in_ptr,
                                     reinterpret_cast<const A_u_char*>("ZSoda"),
                                     reinterpret_cast<const A_u_char*>("ZSoda Depth"),
                                     reinterpret_cast<const A_u_char*>("Z-Soda"),
                                     reinterpret_cast<const A_u_char*>("EffectMain"),
                                     'eFKT',
                                     PF_AE_PLUG_IN_VERSION,
                                     PF_AE_PLUG_IN_SUBVERS,
                                     kAeReservedInfo,
                                     reinterpret_cast<const A_u_char*>("https://example.com/zsoda"));
  return static_cast<PF_Err>(result);
}

PF_Err EffectMainImpl(PF_Cmd cmd,
                      PF_InData* in_data,
                      PF_OutData* out_data,
                      PF_ParamDef* params[],
                      PF_LayerDef* output,
                      void* extra) {
#if defined(ZSODA_AE_LOADER_ONLY_MODE) && ZSODA_AE_LOADER_ONLY_MODE
  return RunLoaderOnlyEffectMain(cmd, in_data, out_data, params, output, extra);
#else
  const bool is_render_cmd = (cmd == PF_Cmd_RENDER);
  const std::uint64_t render_trace_id = is_render_cmd ? zsoda::ae::NextRenderTraceId() : 0U;
  if (cmd == PF_Cmd_RENDER) {
    zsoda::ae::LogRenderStage(render_trace_id, "enter");
    zsoda::ae::LogEngineStatusOnce();
  }
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
    if (is_render_cmd) {
      zsoda::ae::LogRenderStage(render_trace_id,
                                "build_dispatch_failed",
                                error.empty() ? "<none>" : error.c_str());
    }
    const std::string detail =
        "BuildSdkDispatch failed: cmd=" + std::to_string(static_cast<int>(cmd)) +
        ", error=" + (error.empty() ? "<none>" : error);
    zsoda::ae::AppendDiagnosticsLine("EffectMain", detail.c_str());
    TryApplyRenderPassThrough(cmd, params, output, "BuildSdkDispatch failed");
    return PF_Err_NONE;
  }
  if (!error.empty()) {
    if (cmd == PF_Cmd_RENDER) {
      zsoda::ae::LogRenderStage(render_trace_id, "build_dispatch_warning", error.c_str());
      static std::string last_render_warning;
      if (error != last_render_warning) {
        const std::string detail =
            "BuildSdkDispatch warning: cmd=" + std::to_string(static_cast<int>(cmd)) +
            ", detail=" + error;
        zsoda::ae::AppendDiagnosticsLine("EffectMain", detail.c_str());
        last_render_warning = error;
      }
    } else {
      const std::string detail =
          "BuildSdkDispatch warning: cmd=" + std::to_string(static_cast<int>(cmd)) +
          ", detail=" + error;
      zsoda::ae::AppendDiagnosticsLine("EffectMain", detail.c_str());
    }
  }

  // Keep host loader handshake deterministic: setup commands rely on the
  // SDK scaffold (BuildSdkDispatch) and should not depend on router state.
  if (cmd == PF_Cmd_GLOBAL_SETUP || cmd == PF_Cmd_PARAMS_SETUP) {
    if (out_data != nullptr) {
      const std::string detail =
          "setup ok: cmd=" + std::to_string(static_cast<int>(cmd)) +
          ", my_version=" + std::to_string(static_cast<unsigned long>(out_data->my_version)) +
          ", out_flags=0x" + [] (A_long value) {
            char buffer[16] = {};
            std::snprintf(buffer, sizeof(buffer), "%08X", static_cast<unsigned int>(value));
            return std::string(buffer);
          }(out_data->out_flags) +
          ", out_flags2=0x" + [] (A_long value) {
            char buffer[16] = {};
            std::snprintf(buffer, sizeof(buffer), "%08X", static_cast<unsigned int>(value));
            return std::string(buffer);
          }(out_data->out_flags2) +
          ", num_params=" + std::to_string(static_cast<int>(out_data->num_params));
      zsoda::ae::AppendDiagnosticsLine("EffectMainSetup", detail.c_str());
    } else {
      zsoda::ae::AppendDiagnosticsLine("EffectMainSetup", "setup ok with null out_data");
    }
    return PF_Err_NONE;
  }

  if (dispatch.command.command == zsoda::ae::AeCommand::kUnknown) {
    if (is_render_cmd) {
      zsoda::ae::LogRenderStage(render_trace_id, "mapped_unknown");
    }
    // Skeleton entrypoint ignores unsupported commands until individual handlers
    // are wired.
    TryApplyRenderPassThrough(cmd, params, output, "mapped command is unknown");
    return PF_Err_NONE;
  }

  if (is_render_cmd) {
    const std::string dispatch_begin_detail =
        "mapped=" + std::to_string(static_cast<int>(dispatch.command.command)) +
        ", request_ptr=" + (dispatch.command.render_request != nullptr ? std::string("set")
                                                                       : std::string("null")) +
        ", response_ptr=" + (dispatch.command.render_response != nullptr ? std::string("set")
                                                                         : std::string("null"));
    zsoda::ae::LogRenderStage(render_trace_id, "dispatch_begin", dispatch_begin_detail.c_str());
  }
  const int dispatch_result = zsoda::ae::Dispatch(dispatch);
  if (dispatch_result == 0) {
    if (is_render_cmd) {
      const int status_code = static_cast<int>(dispatch.render_response.status);
      const std::string dispatch_ok_detail =
          "status=" + std::to_string(status_code) +
          ", message=" +
          (dispatch.render_response.message.empty() ? std::string("<none>")
                                                    : dispatch.render_response.message);
      zsoda::ae::LogRenderStage(render_trace_id, "dispatch_end_ok", dispatch_ok_detail.c_str());
    }
    if (cmd == PF_Cmd_RENDER) {
      zsoda::ae::LogRenderStage(render_trace_id, "commit_begin");
      std::string commit_error;
      if (!zsoda::ae::CommitSdkRenderOutput(payload, dispatch, &commit_error)) {
        zsoda::ae::LogRenderStage(render_trace_id,
                                  "commit_failed",
                                  commit_error.empty() ? "<none>" : commit_error.c_str());
        const std::string detail = "CommitSdkRenderOutput failed: " +
                                   (commit_error.empty() ? std::string("<none>") : commit_error);
        zsoda::ae::AppendDiagnosticsLine("EffectMain", detail.c_str());
        TryApplyRenderPassThrough(cmd, params, output, "render output commit failed");
      } else {
        zsoda::ae::LogRenderStage(render_trace_id, "commit_ok");
      }

      const int render_status = static_cast<int>(dispatch.render_response.status);
      const std::string render_message = dispatch.render_response.message;
      static int last_render_status = std::numeric_limits<int>::min();
      static std::string last_render_message;
      if (render_status != last_render_status || render_message != last_render_message) {
        const std::string detail =
            "render status=" + std::to_string(render_status) +
            ", message=" + (render_message.empty() ? "<none>" : render_message);
        zsoda::ae::AppendDiagnosticsLine("EffectMainRender", detail.c_str());
        last_render_status = render_status;
        last_render_message = render_message;
      }
      zsoda::ae::LogRenderStage(render_trace_id, "return_ok");
    }
    return PF_Err_NONE;
  }

  if (is_render_cmd) {
    zsoda::ae::LogRenderStage(render_trace_id,
                              "dispatch_failed",
                              error.empty() ? "<none>" : error.c_str());
  }
  const std::string detail =
      "Dispatch failed: cmd=" + std::to_string(static_cast<int>(cmd)) +
      ", mapped=" + std::to_string(static_cast<int>(dispatch.command.command)) +
      ", error=" + (error.empty() ? "<none>" : error);
  zsoda::ae::AppendDiagnosticsLine("EffectMain", detail.c_str());
  TryApplyRenderPassThrough(cmd, params, output, "router dispatch failed");
  return PF_Err_NONE;
#endif
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
    TryApplyRenderPassThrough(cmd, params, output, "caught c++ exception");
    return PF_Err_NONE;
  } catch (...) {
    zsoda::ae::AppendDiagnosticsLine("EffectMain", "unknown c++ exception");
    TryApplyRenderPassThrough(cmd, params, output, "caught unknown c++ exception");
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
