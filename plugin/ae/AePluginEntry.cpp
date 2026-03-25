#include "ae/AeHostAdapter.h"
#include "ae/AeDiagnostics.h"
#include "ae/ZSodaAeFlags.h"
#include "ae/ZSodaVersion.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
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

// ---------------------------------------------------------------------------
// Diagnostics: thin file-append logger, used sparingly and gated by env.
// ---------------------------------------------------------------------------

namespace zsoda::ae {
namespace {

constexpr int kGrayStubChannels = 4;

const char* SafeCStr(const char* value, const char* fallback = "<null>") {
  return value != nullptr ? value : fallback;
}

#if defined(_WIN32)
void AppendDiagnosticsLine(const char* tag, const char* detail) {
  if (!AeDiagnosticsEnabled()) {
    return;
  }

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
#else
void AppendDiagnosticsLine(const char* /*tag*/, const char* /*detail*/) {}
#endif

#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK
const char* PfCmdName(PF_Cmd cmd) {
  switch (cmd) {
    case PF_Cmd_ABOUT:
      return "PF_Cmd_ABOUT";
    case PF_Cmd_GLOBAL_SETUP:
      return "PF_Cmd_GLOBAL_SETUP";
    case PF_Cmd_PARAMS_SETUP:
      return "PF_Cmd_PARAMS_SETUP";
    case PF_Cmd_RENDER:
      return "PF_Cmd_RENDER";
    default:
      return "PF_Cmd_<other>";
  }
}
#endif

// ---------------------------------------------------------------------------
// Lazy-initialized singletons, deferred until first GLOBAL_SETUP.
// ---------------------------------------------------------------------------

std::once_flag g_init_flag;
std::shared_ptr<zsoda::inference::IInferenceEngine> g_engine;
std::shared_ptr<zsoda::core::RenderPipeline> g_pipeline;
std::unique_ptr<zsoda::ae::AeCommandRouter> g_router;

void EnsureInitialized() {
  std::call_once(g_init_flag, [] {
    g_engine = zsoda::inference::CreateDefaultEngine();
    g_pipeline = std::make_shared<zsoda::core::RenderPipeline>(g_engine);
    g_router = std::make_unique<zsoda::ae::AeCommandRouter>(g_pipeline, g_engine);

    // Log engine status exactly once at initialization.
    if (g_engine != nullptr) {
      const auto managed_engine =
          std::dynamic_pointer_cast<zsoda::inference::ManagedInferenceEngine>(g_engine);
      if (managed_engine != nullptr) {
        AppendDiagnosticsLine("EngineStatus", managed_engine->BackendStatusString().c_str());
      } else {
        std::string status = std::string("engine=") + SafeCStr(g_engine->Name(), "<null>");
        AppendDiagnosticsLine("EngineStatus", status.c_str());
      }
    } else {
      AppendDiagnosticsLine("EngineStatus", "engine is null");
    }
  });
}

std::shared_ptr<zsoda::inference::IInferenceEngine> GetEngine() {
  EnsureInitialized();
  return g_engine;
}

std::shared_ptr<zsoda::core::RenderPipeline> GetPipeline() {
  EnsureInitialized();
  return g_pipeline;
}

zsoda::ae::AeCommandRouter& GetRouter() {
  EnsureInitialized();
  return *g_router;
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

int Dispatch(const AeDispatchContext& dispatch) {
  return GetRouter().Handle(dispatch.command) ? 0 : -1;
}

}  // namespace
}  // namespace zsoda::ae

// ---------------------------------------------------------------------------
// C stub entry points — single guard layer (SEH on Windows, catch-all elsewhere)
// All stubs share a uniform pattern: validate → dispatch → return
// ---------------------------------------------------------------------------

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
  // Legacy host-buffer bridge: keep the export for compatibility, but route
  // the request through the current router-owned param snapshot.
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
  // Legacy compatibility shim. Older callers still pass the pre-slice-cleanup
  // argument list, so only the knobs that still map onto the shipping router
  // schema are honored here.
  (void)invert;
  (void)cache_enabled;
  (void)tile_size;
  (void)overlap;
  (void)vram_budget_mb;
  auto params = zsoda::ae::GetRouter().CurrentParams();
  if (model_id != nullptr && model_id[0] != '\0') {
    params.model_id = model_id;
  }
  params.quality = quality;
  params.output = output_mode == 0 ? zsoda::ae::AeOutputSelection::kDepthMap
                                   : zsoda::ae::AeOutputSelection::kDepthSlice;
  min_depth = std::clamp(min_depth, 0.0F, 1.0F);
  max_depth = std::clamp(max_depth, 0.0F, 1.0F);
  if (max_depth < min_depth) {
    std::swap(max_depth, min_depth);
  }
  params.slice_mode = zsoda::ae::AeSliceModeSelection::kBand;
  params.slice_position = 0.5F * (min_depth + max_depth);
  params.slice_range = std::clamp(max_depth - min_depth, 0.0F, 1.0F);
  params.slice_softness = std::clamp(softness, 0.0F, 1.0F);

  std::string error;
  zsoda::ae::AeCommandContext context;
  context.command = zsoda::ae::AeCommand::kUpdateParams;
  context.params_update = &params;
  context.error = &error;
  return zsoda::ae::GetRouter().Handle(context) ? 0 : -1;
}

int ZSodaSetFreezeStubImpl(int freeze_enabled, int request_extract) {
  // Legacy ABI placeholder. Freeze/extract no longer have a shipping AE route.
  (void)freeze_enabled;
  (void)request_extract;
  return 0;
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

// ---------------------------------------------------------------------------
// C++ guard for extern "C" stubs — SEH is not needed here because these
// stubs are called from managed C++ code, not from the AE DLL loader.
// SEH is reserved for EffectMain (the actual AE plugin entry point).
// ---------------------------------------------------------------------------

#define ZSODA_CPP_GUARD(tag, fail_val, expr) \
  try { return (expr); } \
  catch (const std::exception& ex) { \
    zsoda::ae::AppendDiagnosticsLine(tag, ex.what()); \
    return (fail_val); \
  } catch (...) { \
    zsoda::ae::AppendDiagnosticsLine(tag, "unknown c++ exception"); \
    return (fail_val); \
  }

extern "C" int ZSodaEffectMainStub(int command_id) {
  ZSODA_CPP_GUARD("ZSodaEffectMainStub", -1, ZSodaEffectMainStubImpl(command_id))
}

extern "C" int ZSodaSetModelIdStub(const char* model_id) {
  ZSODA_CPP_GUARD("ZSodaSetModelIdStub", -1, ZSodaSetModelIdStubImpl(model_id))
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
  ZSODA_CPP_GUARD("ZSodaSetParamsStub", -1,
                  ZSodaSetParamsStubImpl(model_id, quality, output_mode, invert, min_depth,
                                        max_depth, softness, cache_enabled, tile_size, overlap,
                                        vram_budget_mb))
}

extern "C" int ZSodaRenderGrayFrameStub(const float* src,
                                        int width,
                                        int height,
                                        std::uint64_t frame_hash,
                                        float* out,
                                        int out_size) {
  ZSODA_CPP_GUARD("ZSodaRenderGrayFrameStub", -1,
                  ZSodaRenderGrayFrameStubImpl(src, width, height, frame_hash, out, out_size))
}

extern "C" int ZSodaRenderHostBufferStub(const void* src,
                                         int width,
                                         int height,
                                         int src_row_bytes,
                                         int pixel_format,
                                         std::uint64_t frame_hash,
                                         void* out,
                                         int out_row_bytes) {
  ZSODA_CPP_GUARD("ZSodaRenderHostBufferStub", -1,
                  ZSodaRenderHostBufferStubImpl(src, width, height, src_row_bytes, pixel_format,
                                                frame_hash, out, out_row_bytes))
}

extern "C" int ZSodaSetFreezeStub(int freeze_enabled, int request_extract) {
  ZSODA_CPP_GUARD("ZSodaSetFreezeStub", -1,
                  ZSodaSetFreezeStubImpl(freeze_enabled, request_extract))
}

// ---------------------------------------------------------------------------
// AE SDK EffectMain — only compiled when ZSODA_WITH_AE_SDK=1
// ---------------------------------------------------------------------------

#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK

#ifndef DllExport
#if defined(_WIN32)
#define DllExport __declspec(dllexport)
#else
#define DllExport
#endif
#endif

namespace {

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

void RenderPassThrough(PF_Cmd cmd, PF_ParamDef* params[], PF_LayerDef* output) {
  if (cmd == PF_Cmd_RENDER) {
    (void)TryCopyRenderInputToOutput(params, output);
  }
}

}  // namespace

extern "C" DllExport PF_Err PluginDataEntryFunction2(PF_PluginDataPtr in_ptr,
                                                      PF_PluginDataCB2 in_plugin_data_callback_ptr,
                                                      SPBasicSuite* in_sp_basic_suite_ptr,
                                                      const char* in_host_name,
                                                      const char* in_host_version) {
  constexpr A_long kAeReservedInfo = 8;
  (void)in_sp_basic_suite_ptr;
  (void)in_host_name;
  (void)in_host_version;

  zsoda::ae::AppendDiagnosticsLine(
      "PluginDataEntryFunction2Enter",
      in_plugin_data_callback_ptr != nullptr ? "callback=present" : "callback=null");

  if (in_plugin_data_callback_ptr == nullptr) {
    return PF_Err_INVALID_CALLBACK;
  }

  const A_Err result =
      (*in_plugin_data_callback_ptr)(in_ptr,
                                     reinterpret_cast<const A_u_char*>(ZSODA_EFFECT_DISPLAY_NAME),
                                     reinterpret_cast<const A_u_char*>(ZSODA_EFFECT_MATCH_NAME),
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
  // Loader-only mode: pass-through for diagnostics
  switch (cmd) {
    case PF_Cmd_ABOUT:
      if (out_data != nullptr) {
        std::snprintf(out_data->return_msg, sizeof(out_data->return_msg),
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
    case PF_Cmd_RENDER:
      (void)TryCopyRenderInputToOutput(params, output);
      return PF_Err_NONE;
    default:
      return PF_Err_NONE;
  }
#else
  // -----------------------------------------------------------------------
  // Normal EffectMain: delegate to SDK dispatch, opt-in diagnostics only
  // -----------------------------------------------------------------------

  std::string error;
  zsoda::ae::AeDispatchContext dispatch;
  zsoda::ae::AeSdkEntryPayload payload;
  payload.command = cmd;
  payload.in_data = in_data;
  payload.out_data = out_data;
  payload.params = params;
  payload.output = output;
  payload.extra = extra;

  zsoda::ae::AppendDiagnosticsLine("EffectMainEnter", zsoda::ae::PfCmdName(cmd));

  if (!zsoda::ae::BuildSdkDispatch(payload, &dispatch, &error)) {
    zsoda::ae::AppendDiagnosticsLine("EffectMainBuildDispatchFail", error.c_str());
    // BuildSdkDispatch handles GLOBAL_SETUP and PARAMS_SETUP internally. If it fails,
    // fall back to pass-through for render, no-op for others.
    RenderPassThrough(cmd, params, output);
    return PF_Err_NONE;
  }

  // Setup commands (GLOBAL_SETUP, PARAMS_SETUP) are handled entirely by
  // BuildSdkDispatch above already handled these setup commands.
  if (cmd == PF_Cmd_GLOBAL_SETUP || cmd == PF_Cmd_PARAMS_SETUP) {
    return PF_Err_NONE;
  }

  // Unknown or unhandled commands remain a no-op with render pass-through safety.
  if (dispatch.command.command == zsoda::ae::AeCommand::kUnknown) {
    RenderPassThrough(cmd, params, output);
    return PF_Err_NONE;
  }

  // Defer engine/service initialization until after loader metadata has been
  // returned to AE so startup probe failures cannot zero version/outflags/schema.
  zsoda::ae::EnsureInitialized();

  // Dispatch to router
  const int dispatch_result = zsoda::ae::Dispatch(dispatch);
  if (dispatch_result != 0) {
    const std::string dispatch_detail = "dispatch_result=" + std::to_string(dispatch_result);
    zsoda::ae::AppendDiagnosticsLine("EffectMainDispatchFail", dispatch_detail.c_str());
    RenderPassThrough(cmd, params, output);
    return PF_Err_NONE;
  }

  // For render: commit depth output back to AE host buffer
  if (cmd == PF_Cmd_RENDER) {
    std::string commit_error;
    if (!zsoda::ae::CommitSdkRenderOutput(payload, dispatch, &commit_error)) {
      zsoda::ae::AppendDiagnosticsLine("EffectMainCommitFail", commit_error.c_str());
      // Commit failed: prefer pass-through over black or transparent output.
      RenderPassThrough(cmd, params, output);
    }
  }
  return PF_Err_NONE;
#endif
}

#if defined(_WIN32) && defined(_MSC_VER)
// On MSVC, __try and try/catch cannot coexist in the same function (C2712).
// Split into two functions: CppGuard does try/catch, EffectMain does __try.
static PF_Err EffectMainCppGuard(PF_Cmd cmd,
                                 PF_InData* in_data,
                                 PF_OutData* out_data,
                                 PF_ParamDef* params[],
                                 PF_LayerDef* output,
                                 void* extra) {
  try {
    return EffectMainImpl(cmd, in_data, out_data, params, output, extra);
  } catch (const std::exception& ex) {
    zsoda::ae::AppendDiagnosticsLine("EffectMain", ex.what());
    if (cmd == PF_Cmd_RENDER && params != nullptr && output != nullptr) {
      (void)TryCopyRenderInputToOutput(params, output);
    }
    return PF_Err_NONE;
  } catch (...) {
    zsoda::ae::AppendDiagnosticsLine("EffectMain", "unknown c++ exception");
    if (cmd == PF_Cmd_RENDER && params != nullptr && output != nullptr) {
      (void)TryCopyRenderInputToOutput(params, output);
    }
    return PF_Err_NONE;
  }
}

extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd,
                                       PF_InData* in_data,
                                       PF_OutData* out_data,
                                       PF_ParamDef* params[],
                                       PF_LayerDef* output,
                                       void* extra) {
  __try {
    return EffectMainCppGuard(cmd, in_data, out_data, params, output, extra);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    char seh_msg[64] = {};
    std::snprintf(seh_msg, sizeof(seh_msg), "SEH 0x%08X", static_cast<unsigned>(GetExceptionCode()));
    zsoda::ae::AppendDiagnosticsLine("EffectMain", seh_msg);
    return PF_Err_NONE;
  }
}
#else
extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd,
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
#endif

#endif

