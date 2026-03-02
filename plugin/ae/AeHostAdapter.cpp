#include "ae/AeHostAdapter.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace zsoda::ae {
namespace {

constexpr int kLegacyRenderCommandId = 3;
constexpr std::uint64_t kFNV1a64Offset = 14695981039346656037ULL;
constexpr std::uint64_t kFNV1a64Prime = 1099511628211ULL;

void SetError(std::string* error, const std::string& message) {
  if (error) {
    *error = message;
  }
}

AeCommand MapLegacyCommandId(int command_id) {
  switch (command_id) {
    case 0:
      return AeCommand::kAbout;
    case 1:
      return AeCommand::kGlobalSetup;
    case 2:
      return AeCommand::kParamsSetup;
    case kLegacyRenderCommandId:
      return AeCommand::kRender;
    default:
      return AeCommand::kUnknown;
  }
}

void InitializeBaseContext(AeDispatchContext* dispatch, AeCommand command, std::string* error) {
  dispatch->command = {};
  dispatch->command.command = command;
  dispatch->command.host = &dispatch->host;
  dispatch->command.error = error;
}

inline void MixHash(std::uint64_t value, std::uint64_t* hash) {
  *hash ^= value;
  *hash *= kFNV1a64Prime;
}

inline std::uint64_t ToHashWord(std::int64_t value) {
  return static_cast<std::uint64_t>(value);
}

inline std::uint64_t ToHashWord(int value) {
  return static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
}

inline std::uint64_t ToHashWord(std::size_t value) {
  return static_cast<std::uint64_t>(value);
}

inline std::uint64_t PointerToHashWord(const void* pointer) {
  return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

void InitializeDefaultPixelFormatCandidates(
    std::array<zsoda::core::PixelFormat, kAePixelFormatCandidateCapacity>* candidates) {
  if (candidates == nullptr) {
    return;
  }
  *candidates = {zsoda::core::PixelFormat::kRGBA8,
                 zsoda::core::PixelFormat::kRGBA16,
                 zsoda::core::PixelFormat::kRGBA32F};
}

#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK
void InitializeSdkHostDispatch(const AeSdkEntryPayload& payload,
                               AeCommand command,
                               AeDispatchContext* dispatch,
                               std::string* error) {
  dispatch->host = {};
  dispatch->host.command_id = static_cast<int>(payload.command);
  dispatch->host.in_data = payload.in_data;
  dispatch->host.out_data = payload.out_data;
  dispatch->host.params = payload.params;
  dispatch->host.output = payload.output;
  dispatch->host.extra = payload.extra;

  InitializeBaseContext(dispatch, command, error);
  dispatch->render_request = {};
  dispatch->render_response = {};
}

void RestoreSdkHostContext(const AeSdkEntryPayload& payload, AeDispatchContext* dispatch) {
  dispatch->host.command_id = static_cast<int>(payload.command);
  dispatch->host.in_data = payload.in_data;
  dispatch->host.out_data = payload.out_data;
  dispatch->host.params = payload.params;
  dispatch->host.output = payload.output;
  dispatch->host.extra = payload.extra;
  dispatch->command.host = &dispatch->host;
}

bool TryReadLayerWorld(const PF_LayerDef* world,
                       bool mutable_pixels,
                       int* width,
                       int* height,
                       std::size_t* row_bytes,
                       const void** const_pixels,
                       void** mutable_pixels_out) {
  if (world == nullptr || width == nullptr || height == nullptr || row_bytes == nullptr ||
      const_pixels == nullptr || mutable_pixels_out == nullptr) {
    return false;
  }

  if (world->width <= 0 || world->height <= 0 || world->rowbytes <= 0) {
    return false;
  }
  if (world->data == nullptr) {
    return false;
  }

  *width = static_cast<int>(world->width);
  *height = static_cast<int>(world->height);
  *row_bytes = static_cast<std::size_t>(world->rowbytes);
  *const_pixels = world->data;
  *mutable_pixels_out = mutable_pixels ? world->data : nullptr;
  return true;
}

bool WireParamSetupPayload(const AeSdkEntryPayload& payload,
                           AeDispatchContext* dispatch,
                           std::string* error) {
  (void)payload;
  (void)dispatch;
  (void)error;

  // Extension point: decode PF_Cmd_PARAMS_SETUP host payload and populate
  // dispatch->command.params_update when parameter defaults need synchronization.
  return true;
}

bool WireRenderPayload(const AeSdkEntryPayload& payload,
                       AeDispatchContext* dispatch,
                       std::string* error) {
  InitializeSdkHostDispatch(payload, AeCommand::kRender, dispatch, error);

  AeSdkRenderPayloadScaffold scaffold{};
  if (!TryExtractPfCmdRenderPayload(payload, &scaffold, nullptr)) {
    // Keep safe no-op render behavior when payload probing fails.
    return true;
  }
  if (!scaffold.has_host_buffers) {
    // Keep safe no-op render behavior when source/output extraction is incomplete.
    return true;
  }

  if (!BuildHostBufferRenderDispatch(scaffold.host_render, dispatch, nullptr)) {
    // Any conversion failure falls back to safe no-op, never failing PF_Cmd_RENDER dispatch.
    InitializeSdkHostDispatch(payload, AeCommand::kRender, dispatch, error);
    return true;
  }

  // BuildHostBufferRenderDispatch sets stub metadata; restore the original SDK host payload.
  RestoreSdkHostContext(payload, dispatch);
  return true;
}
#endif

}  // namespace

AeCommand MapStubCommandId(int command_id) {
  return MapLegacyCommandId(command_id);
}

std::uint64_t ComputeSafeFrameHash(const AeFrameHashSeed& seed) {
  std::uint64_t hash = kFNV1a64Offset;
  MixHash(ToHashWord(seed.current_time), &hash);
  MixHash(ToHashWord(seed.time_step), &hash);
  MixHash(ToHashWord(seed.time_scale), &hash);
  MixHash(ToHashWord(seed.width), &hash);
  MixHash(ToHashWord(seed.height), &hash);
  MixHash(ToHashWord(seed.source_row_bytes), &hash);
  MixHash(ToHashWord(seed.output_row_bytes), &hash);
  MixHash(PointerToHashWord(seed.source_pixels), &hash);
  MixHash(PointerToHashWord(seed.output_pixels), &hash);
  MixHash(PointerToHashWord(seed.host_in_data), &hash);
  MixHash(PointerToHashWord(seed.host_output), &hash);

  if (hash == 0) {
    hash = kFNV1a64Offset;
  }
  return hash;
}

std::size_t BuildHostRenderPixelFormatCandidates(
    int width,
    std::size_t row_bytes,
    std::array<zsoda::core::PixelFormat, kAePixelFormatCandidateCapacity>* candidates) {
  if (candidates == nullptr) {
    return 0;
  }
  InitializeDefaultPixelFormatCandidates(candidates);

  std::size_t count = 0;
  const auto push = [&](zsoda::core::PixelFormat format) {
    if (count >= candidates->size()) {
      return;
    }
    for (std::size_t i = 0; i < count; ++i) {
      if ((*candidates)[i] == format) {
        return;
      }
    }
    (*candidates)[count++] = format;
  };

  const auto push_if_fits = [&](zsoda::core::PixelFormat format, std::size_t bytes_per_pixel) {
    if (width <= 0) {
      return;
    }
    const std::size_t width_size = static_cast<std::size_t>(width);
    const std::size_t max_size = std::numeric_limits<std::size_t>::max();
    if (width_size > max_size / bytes_per_pixel) {
      return;
    }
    const std::size_t min_row_bytes = width_size * bytes_per_pixel;
    if (row_bytes >= min_row_bytes) {
      push(format);
    }
  };

  push_if_fits(zsoda::core::PixelFormat::kRGBA32F, sizeof(float) * 4U);
  push_if_fits(zsoda::core::PixelFormat::kRGBA16, sizeof(std::uint16_t) * 4U);
  push_if_fits(zsoda::core::PixelFormat::kRGBA8, sizeof(std::uint8_t) * 4U);

  if (count == 0) {
    push(zsoda::core::PixelFormat::kRGBA8);
    push(zsoda::core::PixelFormat::kRGBA16);
    push(zsoda::core::PixelFormat::kRGBA32F);
  }
  return count;
}

std::optional<zsoda::core::PixelFormat> InferHostRenderPixelFormatFromStride(int width,
                                                                              std::size_t row_bytes) {
  if (width <= 0) {
    return std::nullopt;
  }

  const std::size_t width_size = static_cast<std::size_t>(width);
  if (width_size == 0 || row_bytes % width_size != 0) {
    return std::nullopt;
  }

  const std::size_t bytes_per_pixel = row_bytes / width_size;
  switch (bytes_per_pixel) {
    case sizeof(std::uint8_t) * 4U:
      return zsoda::core::PixelFormat::kRGBA8;
    case sizeof(std::uint16_t) * 4U:
      return zsoda::core::PixelFormat::kRGBA16;
    case sizeof(float) * 4U:
      return zsoda::core::PixelFormat::kRGBA32F;
    default:
      return std::nullopt;
  }
}

std::optional<zsoda::core::PixelFormat> SelectHostRenderPixelFormat(
    const std::array<zsoda::core::PixelFormat, kAePixelFormatCandidateCapacity>& candidates,
    std::size_t candidate_count,
    std::optional<zsoda::core::PixelFormat> source_stride_hint,
    std::optional<zsoda::core::PixelFormat> output_stride_hint) {
  const auto is_candidate = [&](zsoda::core::PixelFormat format) {
    for (std::size_t i = 0; i < candidate_count; ++i) {
      if (candidates[i] == format) {
        return true;
      }
    }
    return false;
  };

  if (candidate_count == 0) {
    return std::nullopt;
  }
  if (source_stride_hint.has_value() && output_stride_hint.has_value() &&
      source_stride_hint != output_stride_hint) {
    return std::nullopt;
  }

  if (source_stride_hint.has_value() && is_candidate(*source_stride_hint)) {
    return source_stride_hint;
  }
  if (output_stride_hint.has_value() && is_candidate(*output_stride_hint)) {
    return output_stride_hint;
  }
  if (candidate_count == 1) {
    return candidates[0];
  }
  return std::nullopt;
}

bool BuildStubDispatch(int command_id, AeDispatchContext* dispatch, std::string* error) {
  if (dispatch == nullptr) {
    SetError(error, "missing dispatch output");
    return false;
  }

  dispatch->host = {};
  dispatch->host.command_id = command_id;
  InitializeBaseContext(dispatch, MapStubCommandId(command_id), error);
  return true;
}

bool BuildHostBufferRenderDispatch(const AeHostRenderBridgePayload& payload,
                                   AeDispatchContext* dispatch,
                                   std::string* error) {
  if (dispatch == nullptr) {
    SetError(error, "missing dispatch output");
    return false;
  }

  dispatch->host = {};
  dispatch->host.command_id = kLegacyRenderCommandId;
  InitializeBaseContext(dispatch, AeCommand::kRender, error);

  dispatch->render_request = {};
  dispatch->render_response = {};
  const auto source_status =
      zsoda::core::ConvertHostToGray32F(payload.source, &dispatch->render_request.source);
  if (source_status != zsoda::core::PixelConversionStatus::kOk) {
    SetError(error,
             std::string("host->gray conversion failed: ") +
                 zsoda::core::PixelConversionStatusString(source_status));
    return false;
  }

  dispatch->render_request.params_override = payload.params_override;
  dispatch->render_request.frame_hash = payload.frame_hash;
  dispatch->command.render_request = &dispatch->render_request;
  dispatch->command.render_response = &dispatch->render_response;
  return true;
}

bool ExecuteHostBufferRenderBridge(AeCommandRouter* router,
                                   const AeHostRenderBridgePayload& payload,
                                   AeHostRenderBridgeResult* result,
                                   std::string* error) {
  if (router == nullptr) {
    SetError(error, "missing command router");
    return false;
  }

  AeDispatchContext dispatch;
  if (!BuildHostBufferRenderDispatch(payload, &dispatch, error)) {
    return false;
  }
  if (!router->Handle(dispatch.command)) {
    return false;
  }

  const auto output_status =
      zsoda::core::ConvertGray32FToHost(dispatch.render_response.output, payload.destination);
  if (output_status != zsoda::core::PixelConversionStatus::kOk) {
    SetError(error,
             std::string("gray->host conversion failed: ") +
                 zsoda::core::PixelConversionStatusString(output_status));
    return false;
  }

  if (result != nullptr) {
    result->status = dispatch.render_response.status;
    result->message = dispatch.render_response.message;
  }
  return true;
}

#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK

AeCommand MapPfCommand(PF_Cmd command) {
  switch (command) {
    case PF_Cmd_ABOUT:
      return AeCommand::kAbout;
    case PF_Cmd_GLOBAL_SETUP:
      return AeCommand::kGlobalSetup;
    case PF_Cmd_PARAMS_SETUP:
      return AeCommand::kParamsSetup;
    case PF_Cmd_RENDER:
      return AeCommand::kRender;
    default:
      return AeCommand::kUnknown;
  }
}

bool BuildSdkDispatch(const AeSdkEntryPayload& payload,
                      AeDispatchContext* dispatch,
                      std::string* error) {
  if (dispatch == nullptr) {
    if (error) {
      *error = "missing dispatch output";
    }
    return false;
  }

  const AeCommand mapped = MapPfCommand(payload.command);
  InitializeSdkHostDispatch(payload, mapped, dispatch, error);

  switch (mapped) {
    case AeCommand::kParamsSetup:
      return WireParamSetupPayload(payload, dispatch, error);
    case AeCommand::kRender:
      return WireRenderPayload(payload, dispatch, error);
    default:
      return true;
  }
}

bool TryExtractPfCmdRenderPayload(const AeSdkEntryPayload& payload,
                                  AeSdkRenderPayloadScaffold* scaffold,
                                  std::string* error) {
  if (scaffold == nullptr) {
    SetError(error, "missing sdk render payload scaffold");
    return false;
  }
  *scaffold = {};

  if (payload.command != PF_Cmd_RENDER) {
    SetError(error, "payload command is not PF_Cmd_RENDER");
    return false;
  }
  scaffold->command_is_render = true;

  const PF_LayerDef* source_world = nullptr;
  if (payload.params != nullptr && payload.params[0] != nullptr) {
    source_world = &payload.params[0]->u.ld;
  }
  const PF_LayerDef* output_world = payload.output;

  int source_width = 0;
  int source_height = 0;
  std::size_t source_row_bytes = 0;
  const void* source_pixels = nullptr;
  void* unused_source_mutable_pixels = nullptr;
  scaffold->source_is_valid =
      TryReadLayerWorld(source_world,
                        false,
                        &source_width,
                        &source_height,
                        &source_row_bytes,
                        &source_pixels,
                        &unused_source_mutable_pixels);

  int output_width = 0;
  int output_height = 0;
  std::size_t output_row_bytes = 0;
  const void* output_pixels_const = nullptr;
  void* output_pixels = nullptr;
  scaffold->output_is_valid = TryReadLayerWorld(output_world,
                                                true,
                                                &output_width,
                                                &output_height,
                                                &output_row_bytes,
                                                &output_pixels_const,
                                                &output_pixels);

  if (scaffold->source_is_valid && scaffold->output_is_valid) {
    scaffold->dimensions_match =
        source_width == output_width && source_height == output_height;
  }

  AeFrameHashSeed frame_hash_seed{};
  if (payload.in_data != nullptr) {
    frame_hash_seed.current_time = static_cast<std::int64_t>(payload.in_data->current_time);
    frame_hash_seed.time_step = static_cast<std::int64_t>(payload.in_data->time_step);
    frame_hash_seed.time_scale = static_cast<std::int64_t>(payload.in_data->time_scale);
  }
  if (scaffold->output_is_valid) {
    frame_hash_seed.width = output_width;
    frame_hash_seed.height = output_height;
  } else if (scaffold->source_is_valid) {
    frame_hash_seed.width = source_width;
    frame_hash_seed.height = source_height;
  }
  frame_hash_seed.source_row_bytes = source_row_bytes;
  frame_hash_seed.output_row_bytes = output_row_bytes;
  frame_hash_seed.source_pixels = source_pixels;
  frame_hash_seed.output_pixels = output_pixels_const;
  frame_hash_seed.host_in_data = payload.in_data;
  frame_hash_seed.host_output = payload.output;
  scaffold->frame_hash = ComputeSafeFrameHash(frame_hash_seed);
  scaffold->host_render.frame_hash = scaffold->frame_hash;

  int candidate_width = 0;
  std::size_t candidate_row_bytes = 0;
  if (scaffold->source_is_valid && scaffold->output_is_valid && scaffold->dimensions_match) {
    candidate_width = output_width;
    candidate_row_bytes = std::min(source_row_bytes, output_row_bytes);
  } else if (scaffold->output_is_valid) {
    candidate_width = output_width;
    candidate_row_bytes = output_row_bytes;
  } else if (scaffold->source_is_valid) {
    candidate_width = source_width;
    candidate_row_bytes = source_row_bytes;
  }
  scaffold->pixel_format_candidate_count =
      BuildHostRenderPixelFormatCandidates(candidate_width,
                                           candidate_row_bytes,
                                           &scaffold->pixel_format_candidates);

  const auto source_stride_hint =
      InferHostRenderPixelFormatFromStride(source_width, source_row_bytes);
  const auto output_stride_hint =
      InferHostRenderPixelFormatFromStride(output_width, output_row_bytes);
  const auto selected_format = SelectHostRenderPixelFormat(scaffold->pixel_format_candidates,
                                                            scaffold->pixel_format_candidate_count,
                                                            source_stride_hint,
                                                            output_stride_hint);

  if (!scaffold->source_is_valid || !scaffold->output_is_valid || !scaffold->dimensions_match ||
      !selected_format.has_value()) {
    // Keep extraction metadata for diagnostics, but avoid wiring host buffers
    // until pixel format detection is unambiguous.
    return true;
  }

  scaffold->host_render.source.pixels = source_pixels;
  scaffold->host_render.source.width = output_width;
  scaffold->host_render.source.height = output_height;
  scaffold->host_render.source.row_bytes = source_row_bytes;
  scaffold->host_render.source.format = *selected_format;
  scaffold->host_render.destination.pixels = output_pixels;
  scaffold->host_render.destination.width = output_width;
  scaffold->host_render.destination.height = output_height;
  scaffold->host_render.destination.row_bytes = output_row_bytes;
  scaffold->host_render.destination.format = *selected_format;
  scaffold->has_host_buffers = true;
  return true;
}

#endif

}  // namespace zsoda::ae
