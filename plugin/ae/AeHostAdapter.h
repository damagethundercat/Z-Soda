#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "ae/AeCommandRouter.h"
#include "core/PixelConversion.h"

#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK
#include "AE_Effect.h"
#endif

namespace zsoda::ae {

struct AeDispatchContext {
  AeHostCommandContext host{};
  AeCommandContext command{};

  // Optional payload storage for future SDK wiring.
  AeParamValues params_update{};
  RenderRequest render_request{};
  RenderResponse render_response{};
};

struct AeHostRenderBridgePayload {
  zsoda::core::HostBufferView source{};
  zsoda::core::MutableHostBufferView destination{};
  std::optional<AeParamValues> params_override{};
  std::uint64_t frame_hash = 0;
};

struct AeHostRenderBridgeResult {
  zsoda::core::RenderStatus status = zsoda::core::RenderStatus::kSafeOutput;
  std::string message;
};

constexpr std::size_t kAePixelFormatCandidateCapacity = 3;

struct AeFrameHashSeed {
  std::int64_t current_time = 0;
  std::int64_t time_step = 0;
  std::int64_t time_scale = 0;
  int width = 0;
  int height = 0;
  std::size_t source_row_bytes = 0;
  std::size_t output_row_bytes = 0;
  const void* source_pixels = nullptr;
  const void* output_pixels = nullptr;
  const void* host_in_data = nullptr;
  const void* host_output = nullptr;
};

std::uint64_t ComputeSafeFrameHash(const AeFrameHashSeed& seed);
std::size_t BuildHostRenderPixelFormatCandidates(
    int width,
    std::size_t row_bytes,
    std::array<zsoda::core::PixelFormat, kAePixelFormatCandidateCapacity>* candidates);
std::optional<zsoda::core::PixelFormat> InferHostRenderPixelFormatFromStride(int width,
                                                                              std::size_t row_bytes);
std::optional<zsoda::core::PixelFormat> SelectHostRenderPixelFormat(
    const std::array<zsoda::core::PixelFormat, kAePixelFormatCandidateCapacity>& candidates,
    std::size_t candidate_count,
    std::optional<zsoda::core::PixelFormat> source_stride_hint,
    std::optional<zsoda::core::PixelFormat> output_stride_hint);

AeCommand MapStubCommandId(int command_id);
bool BuildStubDispatch(int command_id, AeDispatchContext* dispatch, std::string* error);
bool BuildHostBufferRenderDispatch(const AeHostRenderBridgePayload& payload,
                                   AeDispatchContext* dispatch,
                                   std::string* error);
bool ExecuteHostBufferRenderBridge(AeCommandRouter* router,
                                   const AeHostRenderBridgePayload& payload,
                                   AeHostRenderBridgeResult* result,
                                   std::string* error);

#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK

struct AeSdkEntryPayload {
  PF_Cmd command = static_cast<PF_Cmd>(0);
  PF_InData* in_data = nullptr;
  PF_OutData* out_data = nullptr;
  PF_ParamDef** params = nullptr;
  PF_LayerDef* output = nullptr;
  void* extra = nullptr;
};

struct AeSdkRenderPayloadScaffold {
  bool command_is_render = false;
  bool source_is_valid = false;
  bool output_is_valid = false;
  bool dimensions_match = false;
  AeHostRenderBridgePayload host_render{};
  bool has_host_buffers = false;
  std::uint64_t frame_hash = 0;
  std::array<zsoda::core::PixelFormat, kAePixelFormatCandidateCapacity> pixel_format_candidates{};
  std::size_t pixel_format_candidate_count = 0;
};

AeCommand MapPfCommand(PF_Cmd command);
bool BuildSdkDispatch(const AeSdkEntryPayload& payload,
                      AeDispatchContext* dispatch,
                      std::string* error);
bool TryExtractPfCmdRenderPayload(const AeSdkEntryPayload& payload,
                                  AeSdkRenderPayloadScaffold* scaffold,
                                  std::string* error);

#endif

}  // namespace zsoda::ae
