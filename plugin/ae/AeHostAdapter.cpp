#include "ae/AeHostAdapter.h"
#include "ae/AeDiagnostics.h"
#include "ae/ZSodaAeFlags.h"
#include "ae/ZSodaVersion.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

#include "core/CompatMutex.h"

#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK
#if defined(_WIN32) && !defined(AE_OS_WIN)
#define AE_OS_WIN 1
#endif
#include "AE_EffectCB.h"
#include "Param_Utils.h"
#endif

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

constexpr int kFirstRuntimeParamIndex = 1;
constexpr int kLastRuntimeParamIndex = static_cast<int>(AeParamId::kLast);

constexpr int RuntimeParamTableIndexInternal(AeParamId id) {
  switch (id) {
    case AeParamId::kQuality:
      return 1;
    case AeParamId::kPreserveRatio:
      return 2;
    case AeParamId::kOutput:
      return 3;
    case AeParamId::kColorMap:
      return 4;
    case AeParamId::kSliceMode:
      return 5;
    case AeParamId::kSlicePosition:
      return 6;
    case AeParamId::kSliceRange:
      return 7;
    case AeParamId::kSliceSoftness:
      return 8;
    default:
      return -1;
  }
}

std::optional<AeParamId> AeParamIdFromRuntimeParamIndexInternal(int runtime_index) {
  switch (runtime_index) {
    case 1:
      return AeParamId::kQuality;
    case 2:
      return AeParamId::kPreserveRatio;
    case 3:
      return AeParamId::kOutput;
    case 4:
      return AeParamId::kColorMap;
    case 5:
      return AeParamId::kSliceMode;
    case 6:
      return AeParamId::kSlicePosition;
    case 7:
      return AeParamId::kSliceRange;
    case 8:
      return AeParamId::kSliceSoftness;
    default:
      return std::nullopt;
  }
}

void MixSourceBufferFingerprint(const AeFrameHashSeed& seed, std::uint64_t* hash) {
  if (hash == nullptr || seed.source_pixels == nullptr || seed.source_row_bytes == 0 || seed.width <= 0 ||
      seed.height <= 0) {
    return;
  }

  const auto* base = static_cast<const std::uint8_t*>(seed.source_pixels);
  const std::size_t row_bytes = seed.source_row_bytes;
  const int sample_rows = std::min(seed.height, 8);
  const std::size_t fingerprint_width = std::min<std::size_t>(row_bytes, 64);
  const std::size_t bytes_per_row_sample = std::min<std::size_t>(fingerprint_width, 16);

  for (int row_index = 0; row_index < sample_rows; ++row_index) {
    const int y = sample_rows == 1 ? 0 : (row_index * (seed.height - 1)) / (sample_rows - 1);
    const auto* row = base + static_cast<std::size_t>(y) * row_bytes;
    for (std::size_t sample_index = 0; sample_index < bytes_per_row_sample; ++sample_index) {
      const std::size_t x =
          bytes_per_row_sample == 1
              ? 0
              : (sample_index * (fingerprint_width - 1)) / (bytes_per_row_sample - 1);
      MixHash(static_cast<std::uint64_t>(row[x]), hash);
    }
  }
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
constexpr int kAeSdkNumParams = static_cast<int>(AeParamId::kLast) + 1;
constexpr int kQualityPopupChoices = 8;
constexpr int kOutputPopupChoices = 2;
constexpr int kColorMapPopupChoices = 5;
constexpr int kSliceModePopupChoices = 3;
constexpr std::uint32_t kAeGlobalOutFlags = ZSODA_AE_GLOBAL_OUTFLAGS;
constexpr std::uint32_t kAeGlobalOutFlags2 = ZSODA_AE_GLOBAL_OUTFLAGS2;

constexpr char kQualityPopupLabels[] = "256 px|512 px|768 px|1024 px|1280 px|1536 px|1920 px|2048 px";
constexpr char kOutputPopupLabels[] = "Depth Map|Depth Slice";
constexpr char kColorMapPopupLabels[] = "Gray|Turbo|Viridis|Inferno|Magma";
constexpr char kSliceModePopupLabels[] = "Near|Far|Band";

#if defined(PF_Precision_HUNDREDTHS)
constexpr int kSliderPrecisionFractional = PF_Precision_HUNDREDTHS;
#elif defined(PF_Precision_TENTHS)
constexpr int kSliderPrecisionFractional = PF_Precision_TENTHS;
#else
constexpr int kSliderPrecisionFractional = 0;
#endif

#if defined(PF_Precision_WHOLE)
constexpr int kSliderPrecisionWhole = PF_Precision_WHOLE;
#else
constexpr int kSliderPrecisionWhole = 0;
#endif

struct AeParamExtractionMeta {
  int count_hint = 0;
  bool used_sdk_table_fallback = false;
  bool used_checkout_fallback = false;
  bool used_default_fallback = false;
  bool any_param_read = false;
  bool has_quality_popup = false;
  int raw_quality_popup = 0;
};

bool TryExtractPfCmdParamValues(const AeSdkEntryPayload& payload,
                                AeParamValues* values_out,
                                std::string* error,
                                AeParamExtractionMeta* meta);
int GetSdkParamCountHint(const AeSdkEntryPayload& payload);

void AppendSdkTrace(const char* stage, const std::string& detail) {
  AppendDiagnosticsTrace("SdkTrace", stage, detail.empty() ? nullptr : detail.c_str());
}

std::optional<zsoda::core::PixelFormat> ParseSdkPixelFormatHint(std::int64_t pixel_format_hint) {
#if defined(PF_PixelFormat_ARGB32)
  if (pixel_format_hint == static_cast<std::int64_t>(PF_PixelFormat_ARGB32)) {
    return zsoda::core::PixelFormat::kRGBA8;
  }
#endif
#if defined(PF_PixelFormat_ARGB64)
  if (pixel_format_hint == static_cast<std::int64_t>(PF_PixelFormat_ARGB64)) {
    return zsoda::core::PixelFormat::kRGBA16;
  }
#endif
#if defined(PF_PixelFormat_ARGB128)
  if (pixel_format_hint == static_cast<std::int64_t>(PF_PixelFormat_ARGB128)) {
    return zsoda::core::PixelFormat::kRGBA32F;
  }
#endif
  return std::nullopt;
}

template <typename T>
std::optional<zsoda::core::PixelFormat> ParseSdkPixelFormatMemberHint(const T* value) {
  if (value == nullptr) {
    return std::nullopt;
  }

  if constexpr (requires(const T& item) { item.pixel_format; }) {
    const auto parsed = ParseSdkPixelFormatHint(static_cast<std::int64_t>(value->pixel_format));
    if (parsed.has_value()) {
      return parsed;
    }
  }
  if constexpr (requires(const T& item) { item.pix_format; }) {
    const auto parsed = ParseSdkPixelFormatHint(static_cast<std::int64_t>(value->pix_format));
    if (parsed.has_value()) {
      return parsed;
    }
  }
  return std::nullopt;
}

std::optional<zsoda::core::PixelFormat> InferHostRenderPixelFormatFromSdkInData(
    const PF_InData* in_data) {
  return ParseSdkPixelFormatMemberHint(in_data);
}

std::optional<zsoda::core::PixelFormat> InferHostRenderPixelFormatFromSdkWorldMeta(
    const PF_LayerDef* world) {
  return ParseSdkPixelFormatMemberHint(world);
}

std::optional<zsoda::core::PixelFormat> InferHostRenderPixelFormatFromSdkWorldFlags(
    const PF_LayerDef* world) {
  if (world == nullptr) {
    return std::nullopt;
  }
#if defined(PF_WorldFlag_DEEP)
  if ((world->world_flags & PF_WorldFlag_DEEP) != 0) {
    return zsoda::core::PixelFormat::kRGBA16;
  }
#endif
  return std::nullopt;
}

std::optional<zsoda::core::PixelFormat> InferHostRenderPixelFormatFromSdkAccessors(
    const PF_LayerDef* world) {
  if (world == nullptr) {
    return std::nullopt;
  }

#if defined(PF_GET_PIXEL_DATA8) && defined(PF_Pixel8)
  PF_Pixel8* pixel8 = nullptr;
  (void)PF_GET_PIXEL_DATA8(const_cast<PF_LayerDef*>(world), nullptr, &pixel8);
  if (pixel8 != nullptr) {
    return zsoda::core::PixelFormat::kRGBA8;
  }
#endif

#if defined(PF_GET_PIXEL_DATA16) && defined(PF_Pixel16)
  PF_Pixel16* pixel16 = nullptr;
  (void)PF_GET_PIXEL_DATA16(const_cast<PF_LayerDef*>(world), nullptr, &pixel16);
  if (pixel16 != nullptr) {
    return zsoda::core::PixelFormat::kRGBA16;
  }
#endif

  return std::nullopt;
}

std::optional<zsoda::core::PixelFormat> InferHostRenderPixelFormatFromSdkWorld(
    const PF_LayerDef* world) {
  const auto meta_hint = InferHostRenderPixelFormatFromSdkWorldMeta(world);
  if (meta_hint.has_value()) {
    return meta_hint;
  }
  const auto accessor_hint = InferHostRenderPixelFormatFromSdkAccessors(world);
  if (accessor_hint.has_value()) {
    return accessor_hint;
  }
  return InferHostRenderPixelFormatFromSdkWorldFlags(world);
}

std::optional<zsoda::core::PixelFormat> SelectBestPixelFormatHint(
    std::optional<zsoda::core::PixelFormat> sdk_world_hint,
    std::optional<zsoda::core::PixelFormat> stride_hint,
    std::optional<zsoda::core::PixelFormat> in_data_hint) {
  if (sdk_world_hint.has_value()) {
    return sdk_world_hint;
  }
  if (stride_hint.has_value()) {
    return stride_hint;
  }
  return in_data_hint;
}

PF_Err RegisterParamsSetupScaffold(const AeSdkEntryPayload& payload) {
  if (payload.in_data == nullptr || payload.out_data == nullptr) {
    return PF_Err_NONE;
  }

#if defined(PF_ADD_POPUP) && defined(PF_ADD_FLOAT_SLIDERX) && \
    (defined(PF_ADD_CHECKBOXX) || defined(PF_ADD_CHECKBOX))
  PF_Err err = PF_Err_NONE;
  PF_InData* in_data = payload.in_data;
  PF_OutData* out_data = payload.out_data;
  PF_ParamDef** params = payload.params;
  (void)out_data;
  (void)params;
  PF_ParamDef def;

  const auto clear_def = [&def]() {
#if defined(AEFX_CLR_STRUCT)
    AEFX_CLR_STRUCT(def);
#else
    def = {};
#endif
  };
  (void)in_data;

  clear_def();
  PF_ADD_POPUP("Quality",
               kQualityPopupChoices,
               2,
               kQualityPopupLabels,
               static_cast<int>(AeParamId::kQuality));

  clear_def();
  #if defined(PF_ADD_CHECKBOXX)
  PF_ADD_CHECKBOXX("Preserve Ratio", 1, 0, static_cast<int>(AeParamId::kPreserveRatio));
  #else
  PF_ADD_CHECKBOX("Preserve Ratio", "Preserve Ratio", 1, 0,
                  static_cast<int>(AeParamId::kPreserveRatio));
  #endif

  clear_def();
  PF_ADD_POPUP("Output",
               kOutputPopupChoices,
               static_cast<int>(AeOutputSelection::kDepthMap),
               kOutputPopupLabels,
               static_cast<int>(AeParamId::kOutput));

  clear_def();
  PF_ADD_POPUP("Color Map",
               kColorMapPopupChoices,
               static_cast<int>(AeDepthColorMapSelection::kGray),
               kColorMapPopupLabels,
               static_cast<int>(AeParamId::kColorMap));

  clear_def();
  PF_ADD_POPUP("Slice Mode",
               kSliceModePopupChoices,
               static_cast<int>(AeSliceModeSelection::kBand),
               kSliceModePopupLabels,
               static_cast<int>(AeParamId::kSliceMode));

  clear_def();
  // PF_ADD_FLOAT_SLIDERX takes DFLT before PREC; do not insert the older
  // curve-tolerance slot used by PF_ADD_FLOAT_SLIDER.
  PF_ADD_FLOAT_SLIDERX("Position (%)",
                       0,
                       100,
                       0,
                       100,
                       50.0,
                       kSliderPrecisionFractional,
                       0,
                       0,
                       static_cast<int>(AeParamId::kSlicePosition));

  clear_def();
  PF_ADD_FLOAT_SLIDERX("Range (%)",
                       0,
                       100,
                       0,
                       100,
                       10.0,
                       kSliderPrecisionFractional,
                       0,
                       0,
                       static_cast<int>(AeParamId::kSliceRange));

  clear_def();
  PF_ADD_FLOAT_SLIDERX("Soft Border (%)",
                       0,
                       100,
                       0,
                       100,
                       5.0,
                       kSliderPrecisionFractional,
                       0,
                       0,
                       static_cast<int>(AeParamId::kSliceSoftness));

  return err;
#else
  (void)payload;
  return PF_Err_NONE;
#endif
}

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

bool TryCopyLayerWorldPassThrough(const PF_LayerDef* source_world, PF_LayerDef* output_world) {
  if (source_world == nullptr || output_world == nullptr || source_world->data == nullptr ||
      output_world->data == nullptr) {
    return false;
  }

  if (source_world->width <= 0 || source_world->height <= 0 || source_world->rowbytes <= 0 ||
      output_world->width <= 0 || output_world->height <= 0 || output_world->rowbytes <= 0) {
    return false;
  }

  const A_long rows = std::min(source_world->height, output_world->height);
  const A_long bytes_to_copy = std::min(source_world->rowbytes, output_world->rowbytes);
  if (rows <= 0 || bytes_to_copy <= 0) {
    return false;
  }

  auto* dst = reinterpret_cast<std::uint8_t*>(output_world->data);
  const auto* src = reinterpret_cast<const std::uint8_t*>(source_world->data);
  for (A_long y = 0; y < rows; ++y) {
    std::memcpy(dst, src, static_cast<std::size_t>(bytes_to_copy));
    dst += output_world->rowbytes;
    src += source_world->rowbytes;
  }
  return true;
}

std::string FormatRuntimeStateKeys(const AeSdkEntryPayload& payload) {
  (void)payload;
  return "sequence_data=disabled";
}
bool ParamsAffectRenderedOutput(const AeParamValues& previous, const AeParamValues& current) {
  return previous.model_id != current.model_id || previous.quality != current.quality ||
         previous.preserve_ratio != current.preserve_ratio || previous.output != current.output ||
         (previous.output == AeOutputSelection::kDepthMap &&
          current.output == AeOutputSelection::kDepthMap && previous.color_map != current.color_map) ||
         previous.slice_mode != current.slice_mode ||
         std::fabs(previous.slice_position - current.slice_position) > 1e-4F ||
         std::fabs(previous.slice_range - current.slice_range) > 1e-4F ||
         std::fabs(previous.slice_softness - current.slice_softness) > 1e-4F;
}

bool ShouldRequestForceRerender(PF_Cmd command) {
  return command == PF_Cmd_USER_CHANGED_PARAM;
}

void RequestForceRerender(const AeSdkEntryPayload& payload) {
#if defined(PF_OutFlag_FORCE_RERENDER)
  if (payload.out_data != nullptr) {
    payload.out_data->out_flags |= PF_OutFlag_FORCE_RERENDER;
  }
#else
  (void)payload;
#endif
}

void ClearSequenceDataOutput(const AeSdkEntryPayload& payload) {
  if (payload.out_data != nullptr) {
    payload.out_data->sequence_data = nullptr;
  }
}

void ClearFrameDataOutput(const AeSdkEntryPayload& payload) {
  if (payload.out_data != nullptr) {
    payload.out_data->frame_data = nullptr;
  }
}

bool AllowsSdkParamTableCount(PF_Cmd command) {
  bool allowed = command == PF_Cmd_PARAMS_SETUP;
  allowed = allowed || command == PF_Cmd_USER_CHANGED_PARAM;
  allowed = allowed || command == PF_Cmd_SEQUENCE_SETUP;
  allowed = allowed || command == PF_Cmd_SEQUENCE_RESETUP;
  // Some AE host paths report unreliable in_data->num_params during render, while
  // out_data->num_params can still carry the full schema count.
  allowed = allowed || command == PF_Cmd_RENDER;
  return allowed;
}

bool AllowsSdkParamTableFallback(PF_Cmd command) {
  return command == PF_Cmd_USER_CHANGED_PARAM;
}

bool AllowsParamCheckoutFallback(PF_Cmd command) {
  return command == PF_Cmd_RENDER;
}

int GetSdkParamCountHint(const AeSdkEntryPayload& payload) {
  int count = 0;
  if (payload.in_data != nullptr && payload.in_data->num_params > 0) {
    count = static_cast<int>(payload.in_data->num_params);
  }
  if (AllowsSdkParamTableCount(payload.command) && payload.out_data != nullptr &&
      payload.out_data->num_params > 0) {
    count = std::max(count, static_cast<int>(payload.out_data->num_params));
  }
  return count;
}

const PF_ParamDef* GetParam(const AeSdkEntryPayload& payload, AeParamId id) {
  if (payload.params == nullptr) {
    return nullptr;
  }
  const int index = RuntimeParamTableIndexInternal(id);
  if (index < 0) {
    return nullptr;
  }

  const int count_hint = GetSdkParamCountHint(payload);

  if (count_hint > 0 && index < count_hint) {
    return payload.params[index];
  }

  if (count_hint > 0) {
    return nullptr;
  }

  if (AllowsSdkParamTableFallback(payload.command)) {
    return payload.params[index];
  }

  // Setup fallback: at minimum input layer param is expected.
  if (index == 0) {
    return payload.params[0];
  }
  return nullptr;
}

bool TryCheckoutParamDef(const AeSdkEntryPayload& payload,
                         AeParamId id,
                         PF_ParamDef* checked_out) {
#if defined(PF_CHECKOUT_PARAM) && defined(PF_CHECKIN_PARAM)
  if (checked_out == nullptr || payload.in_data == nullptr) {
    return false;
  }

  const int index = RuntimeParamTableIndexInternal(id);
  if (index <= 0) {
    return false;
  }

  if (payload.in_data->inter.checkout_param == nullptr || payload.in_data->inter.checkin_param == nullptr) {
    return false;
  }

  PF_ParamDef param{};
#if defined(AEFX_CLR_STRUCT)
  AEFX_CLR_STRUCT(param);
#endif
  AppendSdkTrace("checkout_param_begin",
                 "cmd=" + std::to_string(static_cast<int>(payload.command)) + ", index=" +
                     std::to_string(index));
  const PF_Err checkout_err =
      PF_CHECKOUT_PARAM(payload.in_data,
                        index,
                        payload.in_data->current_time,
                        payload.in_data->time_step,
                        payload.in_data->time_scale,
                        &param);
  if (checkout_err != PF_Err_NONE) {
    AppendSdkTrace("checkout_param_fail",
                   "cmd=" + std::to_string(static_cast<int>(payload.command)) + ", index=" +
                       std::to_string(index) + ", err=" + std::to_string(static_cast<int>(checkout_err)));
    return false;
  }

  *checked_out = param;
  AppendSdkTrace("checkout_param_ok",
                 "cmd=" + std::to_string(static_cast<int>(payload.command)) + ", index=" +
                     std::to_string(index));
  (void)PF_CHECKIN_PARAM(payload.in_data, &param);
  AppendSdkTrace("checkin_param_ok",
                 "cmd=" + std::to_string(static_cast<int>(payload.command)) + ", index=" +
                     std::to_string(index));
  return true;
#else
  (void)payload;
  (void)id;
  (void)checked_out;
  return false;
#endif
}

const PF_ParamDef* ResolveParamForRead(const AeSdkEntryPayload& payload,
                                       AeParamId id,
                                       PF_ParamDef* checked_out,
                                       bool* used_checkout) {
  if (used_checkout != nullptr) {
    *used_checkout = false;
  }

  const PF_ParamDef* param = GetParam(payload, id);
  if (param != nullptr) {
    return param;
  }

  if (checked_out != nullptr && AllowsParamCheckoutFallback(payload.command) &&
      TryCheckoutParamDef(payload, id, checked_out)) {
    if (used_checkout != nullptr) {
      *used_checkout = true;
    }
    return checked_out;
  }

  return nullptr;
}

bool TryReadPopupValue(const PF_ParamDef* param, int* value_out) {
  if (param == nullptr || value_out == nullptr) {
    return false;
  }
  *value_out = static_cast<int>(param->u.pd.value);
  return true;
}

bool TryReadCheckboxValue(const PF_ParamDef* param, bool* value_out) {
  if (param == nullptr || value_out == nullptr) {
    return false;
  }
  *value_out = param->u.bd.value != 0;
  return true;
}

bool TryReadFloatSliderValue(const PF_ParamDef* param, float* value_out) {
  if (param == nullptr || value_out == nullptr) {
    return false;
  }
  *value_out = static_cast<float>(param->u.fs_d.value);
  return std::isfinite(*value_out);
}

bool TryExtractPfCmdParamValues(const AeSdkEntryPayload& payload,
                                AeParamValues* values_out,
                                std::string* error,
                                AeParamExtractionMeta* meta = nullptr) {
  if (values_out == nullptr) {
    SetError(error, "missing sdk params output");
    return false;
  }
  AeParamValues values = DefaultAeParams();
  bool any_param_read = false;
  if (meta != nullptr) {
    *meta = {};
    meta->count_hint = GetSdkParamCountHint(payload);
    meta->used_sdk_table_fallback = AllowsSdkParamTableFallback(payload.command);
  }
  if (payload.params == nullptr) {
    if (meta != nullptr) {
      meta->used_default_fallback = true;
    }
    *values_out = values;
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  int popup_value = 0;
  PF_ParamDef checked_out_param{};
  bool used_checkout = false;
  if (TryReadPopupValue(ResolveParamForRead(payload, AeParamId::kQuality, &checked_out_param, &used_checkout),
                        &popup_value)) {
    any_param_read = true;
    if (meta != nullptr && used_checkout) {
      meta->used_checkout_fallback = true;
    }
    values.quality = ClampQualitySelection(popup_value);
    if (meta != nullptr) {
      meta->has_quality_popup = true;
      meta->raw_quality_popup = popup_value;
    }
  }

  bool checkbox_value = false;
  if (TryReadCheckboxValue(
          ResolveParamForRead(payload, AeParamId::kPreserveRatio, &checked_out_param, &used_checkout),
          &checkbox_value)) {
    any_param_read = true;
    if (meta != nullptr && used_checkout) {
      meta->used_checkout_fallback = true;
    }
    values.preserve_ratio = checkbox_value;
  }

  if (TryReadPopupValue(ResolveParamForRead(payload, AeParamId::kOutput, &checked_out_param, &used_checkout),
                        &popup_value)) {
    any_param_read = true;
    if (meta != nullptr && used_checkout) {
      meta->used_checkout_fallback = true;
    }
    values.output = ClampOutputSelection(popup_value);
  }

  if (TryReadPopupValue(
          ResolveParamForRead(payload, AeParamId::kColorMap, &checked_out_param, &used_checkout),
          &popup_value)) {
    any_param_read = true;
    if (meta != nullptr && used_checkout) {
      meta->used_checkout_fallback = true;
    }
    values.color_map = ClampDepthColorMapSelection(popup_value);
  }

  if (TryReadPopupValue(
          ResolveParamForRead(payload, AeParamId::kSliceMode, &checked_out_param, &used_checkout),
          &popup_value)) {
    any_param_read = true;
    if (meta != nullptr && used_checkout) {
      meta->used_checkout_fallback = true;
    }
    values.slice_mode = ClampSliceModeSelection(popup_value);
  }

  float slider_value = 0.0F;
  if (TryReadFloatSliderValue(
          ResolveParamForRead(payload, AeParamId::kSlicePosition, &checked_out_param, &used_checkout),
          &slider_value)) {
    any_param_read = true;
    if (meta != nullptr && used_checkout) {
      meta->used_checkout_fallback = true;
    }
    values.slice_position = std::clamp(slider_value / 100.0F, 0.0F, 1.0F);
  }
  if (TryReadFloatSliderValue(
          ResolveParamForRead(payload, AeParamId::kSliceRange, &checked_out_param, &used_checkout),
          &slider_value)) {
    any_param_read = true;
    if (meta != nullptr && used_checkout) {
      meta->used_checkout_fallback = true;
    }
    values.slice_range = std::clamp(slider_value / 100.0F, 0.0F, 1.0F);
  }
  if (TryReadFloatSliderValue(
          ResolveParamForRead(payload, AeParamId::kSliceSoftness, &checked_out_param, &used_checkout),
          &slider_value)) {
    any_param_read = true;
    if (meta != nullptr && used_checkout) {
      meta->used_checkout_fallback = true;
    }
    values.slice_softness = std::clamp(slider_value / 100.0F, 0.0F, 1.0F);
  }

  if (meta != nullptr) {
    meta->any_param_read = any_param_read;
    meta->used_default_fallback = !any_param_read;
  }
  *values_out = values;
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool WireParamSetupPayload(const AeSdkEntryPayload& payload,
                           AeDispatchContext* dispatch,
                           std::string* error) {
  if (payload.out_data != nullptr) {
    // Keep setup metadata deterministic and lock to the full schema count.
    payload.out_data->my_version = static_cast<A_u_long>(ZSODA_EFFECT_VERSION_HEX);
    payload.out_data->out_flags = static_cast<A_long>(kAeGlobalOutFlags);
    payload.out_data->out_flags2 = static_cast<A_long>(kAeGlobalOutFlags2);
    payload.out_data->num_params = 1;
  }

  if (payload.in_data == nullptr || payload.out_data == nullptr) {
    SetError(error, "params setup payload is incomplete; keep fixed param schema");
    (void)dispatch;
    return true;
  }

  const PF_Err register_err = RegisterParamsSetupScaffold(payload);
  if (register_err != PF_Err_NONE) {
    SetError(error,
             "params setup scaffold registration failed (PF_Err=" +
                 std::to_string(static_cast<int>(register_err)) + ")");
    return false;
  }
  payload.out_data->num_params = kAeSdkNumParams;

  (void)dispatch;
  return true;
}

bool WireGlobalSetupPayload(const AeSdkEntryPayload& payload,
                            AeDispatchContext* dispatch,
                            std::string* error) {
  if (payload.out_data != nullptr) {
    payload.out_data->my_version = static_cast<A_u_long>(ZSODA_EFFECT_VERSION_HEX);
    payload.out_data->out_flags = static_cast<A_long>(kAeGlobalOutFlags);
    payload.out_data->out_flags2 = static_cast<A_long>(kAeGlobalOutFlags2);
  }

  (void)dispatch;
  (void)error;
  return true;
}

bool WireParamUpdatePayload(const AeSdkEntryPayload& payload,
                            AeDispatchContext* dispatch,
                            std::string* error) {
  InitializeSdkHostDispatch(payload, AeCommand::kUpdateParams, dispatch, error);
  AeParamValues params = DefaultAeParams();
  AeParamExtractionMeta meta{};
  if (!TryExtractPfCmdParamValues(payload, &params, error, &meta)) {
    // Keep host responsiveness: skip unsupported payloads without failing host command.
    dispatch->command.command = AeCommand::kUnknown;
    return true;
  }

  const std::string trace_detail =
      "cmd=" + std::to_string(static_cast<int>(payload.command)) +
      ", count_hint=" + std::to_string(meta.count_hint) +
      ", sdk_fallback=" + std::to_string(meta.used_sdk_table_fallback ? 1 : 0) +
      ", checkout_fallback=" + std::to_string(meta.used_checkout_fallback ? 1 : 0) +
      ", default_fallback=" + std::to_string(meta.used_default_fallback ? 1 : 0) +
      ", any_read=" + std::to_string(meta.any_param_read ? 1 : 0) +
      ", quality_popup=" +
      std::string(meta.has_quality_popup ? std::to_string(meta.raw_quality_popup) : "<none>") +
      ", resolved_quality=" + std::to_string(params.quality) +
      ", output=" + std::to_string(static_cast<int>(params.output)) +
      ", color_map=" + std::to_string(static_cast<int>(params.color_map)) +
      ", slice_mode=" + std::to_string(static_cast<int>(params.slice_mode)) +
      ", slice_position=" + std::to_string(params.slice_position) +
      ", slice_range=" + std::to_string(params.slice_range) +
      ", slice_softness=" + std::to_string(params.slice_softness) + ", " +
      FormatRuntimeStateKeys(payload);
  AppendSdkTrace("param_update_extract", trace_detail);
  if (payload.command != PF_Cmd_USER_CHANGED_PARAM || !meta.any_param_read) {
    dispatch->command.command = AeCommand::kUnknown;
    return true;
  }
  if (ShouldRequestForceRerender(payload.command)) {
    RequestForceRerender(payload);
    AppendSdkTrace("param_update_force_rerender", trace_detail);
  }
  dispatch->params_update = params;
  dispatch->command.params_update = &dispatch->params_update;
  return true;
}

bool WireRenderPayload(const AeSdkEntryPayload& payload,
                       AeDispatchContext* dispatch,
                       std::string* error) {
  const PF_LayerDef* source_world = nullptr;
  if (payload.params != nullptr && payload.params[0] != nullptr) {
    source_world = &payload.params[0]->u.ld;
  }
  const auto try_passthrough = [&]() {
    if (TryCopyLayerWorldPassThrough(source_world, payload.output)) {
      SetError(error, "render scaffold fallback: pass-through copy");
    }
  };

  InitializeSdkHostDispatch(payload, AeCommand::kRender, dispatch, error);

  AeSdkRenderPayloadScaffold scaffold{};
  if (!TryExtractPfCmdRenderPayload(payload, &scaffold, error)) {
    // Fall back to source pass-through on extraction failures.
    try_passthrough();
    return true;
  }
  if (!scaffold.has_host_buffers) {
    // Fall back to source pass-through when host buffer extraction is incomplete.
    try_passthrough();
    return true;
  }

  if (!BuildHostBufferRenderDispatch(scaffold.host_render, dispatch, error)) {
    // Any conversion failure falls back to source pass-through, never failing PF_Cmd_RENDER dispatch.
    InitializeSdkHostDispatch(payload, AeCommand::kRender, dispatch, error);
    try_passthrough();
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
  MixSourceBufferFingerprint(seed, &hash);

  if (hash == 0) {
    hash = kFNV1a64Offset;
  }
  return hash;
}

int RuntimeParamTableIndex(AeParamId id) {
  return RuntimeParamTableIndexInternal(id);
}

std::optional<AeParamId> AeParamIdFromRuntimeParamIndex(int runtime_index) {
  return AeParamIdFromRuntimeParamIndexInternal(runtime_index);
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

  push_if_fits(zsoda::core::PixelFormat::kRGBA8, sizeof(std::uint8_t) * 4U);
  push_if_fits(zsoda::core::PixelFormat::kRGBA16, sizeof(std::uint16_t) * 4U);
  push_if_fits(zsoda::core::PixelFormat::kRGBA32F, sizeof(float) * 4U);

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
  return candidates[0];
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
      zsoda::core::ConvertHostToRgb32F(payload.source, &dispatch->render_request.source);
  if (source_status != zsoda::core::PixelConversionStatus::kOk) {
    SetError(error,
             std::string("host->rgb conversion failed: ") +
                 zsoda::core::PixelConversionStatusString(source_status));
    return false;
  }

  dispatch->render_request.params_override = payload.params_override;
  dispatch->render_request.pipeline_state = payload.pipeline_state;
  dispatch->render_request.frame_hash = payload.frame_hash;
  dispatch->render_request.render_state_token = payload.render_state_token;
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

  const auto& output_frame = dispatch.render_response.output;
  const auto output_status =
      (output_frame.desc().format == zsoda::core::PixelFormat::kRGBA32F &&
       output_frame.desc().channels >= 4)
          ? zsoda::core::ConvertRgba32FToHost(output_frame, payload.destination)
          : zsoda::core::ConvertGray32FToHost(output_frame, payload.destination);
  if (output_status != zsoda::core::PixelConversionStatus::kOk) {
    SetError(error,
             std::string("output->host conversion failed: ") +
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
    case PF_Cmd_USER_CHANGED_PARAM:
      return AeCommand::kUpdateParams;
    case PF_Cmd_UPDATE_PARAMS_UI:
      return AeCommand::kUnknown;
    case PF_Cmd_SEQUENCE_SETUP:
      return AeCommand::kUnknown;
    case PF_Cmd_SEQUENCE_RESETUP:
      return AeCommand::kUnknown;
    case PF_Cmd_SEQUENCE_SETDOWN:
      return AeCommand::kUnknown;
    case PF_Cmd_SEQUENCE_FLATTEN:
      return AeCommand::kUnknown;
    case PF_Cmd_SMART_PRE_RENDER:
      return AeCommand::kUnknown;
    case PF_Cmd_SMART_RENDER:
      return AeCommand::kUnknown;
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
  if (mapped != AeCommand::kRender) {
    const std::string detail =
        "cmd=" + std::to_string(static_cast<int>(payload.command)) +
        ", mapped=" + std::to_string(static_cast<int>(mapped)) +
        ", in_num_params=" +
        std::to_string(payload.in_data != nullptr ? static_cast<int>(payload.in_data->num_params) : 0) +
        ", out_num_params=" +
        std::to_string(payload.out_data != nullptr ? static_cast<int>(payload.out_data->num_params) : 0);
    AppendSdkTrace("dispatch_enter", detail);
  }

  if (payload.command == PF_Cmd_SEQUENCE_SETUP) {
    ClearSequenceDataOutput(payload);
    AppendSdkTrace("sequence_setup",
                   "cmd=" + std::to_string(static_cast<int>(payload.command)) +
                       ", source=disabled");
    return true;
  }
  if (payload.command == PF_Cmd_SEQUENCE_RESETUP) {
    ClearSequenceDataOutput(payload);
    AppendSdkTrace("sequence_resetup",
                   "cmd=" + std::to_string(static_cast<int>(payload.command)) +
                       ", source=disabled");
    return true;
  }
  if (payload.command == PF_Cmd_SEQUENCE_SETDOWN) {
    ClearSequenceDataOutput(payload);
    AppendSdkTrace("sequence_setdown",
                   "cmd=" + std::to_string(static_cast<int>(payload.command)) + ", " +
                       FormatRuntimeStateKeys(payload));
    return true;
  }
  if (payload.command == PF_Cmd_GET_FLATTENED_SEQUENCE_DATA) {
    (void)error;
    AppendSdkTrace("sequence_get_flattened",
                   "cmd=" + std::to_string(static_cast<int>(payload.command)) +
                       ", source=disabled");
    return true;
  }
  if (payload.command == PF_Cmd_SEQUENCE_FLATTEN) {
    (void)error;
    AppendSdkTrace("sequence_flatten",
                   "cmd=" + std::to_string(static_cast<int>(payload.command)) +
                       ", source=disabled");
    return true;
  }
  if (payload.command == PF_Cmd_FRAME_SETUP) {
    ClearFrameDataOutput(payload);
    AppendSdkTrace("frame_setup",
                   "cmd=" + std::to_string(static_cast<int>(payload.command)) +
                       ", source=disabled");
    return true;
  }
  if (payload.command == PF_Cmd_FRAME_SETDOWN) {
    ClearFrameDataOutput(payload);
    AppendSdkTrace("frame_setdown",
                   "cmd=" + std::to_string(static_cast<int>(payload.command)) +
                       ", source=disabled");
    return true;
  }

  switch (mapped) {
    case AeCommand::kGlobalSetup:
      return WireGlobalSetupPayload(payload, dispatch, error);
    case AeCommand::kParamsSetup:
      return WireParamSetupPayload(payload, dispatch, error);
    case AeCommand::kUpdateParams:
      return WireParamUpdatePayload(payload, dispatch, error);
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
  scaffold->host_render.pipeline_state.reset();
  // Shipping AE renders do not persist sequence-backed state between callbacks.
  scaffold->host_render.render_state_token = 0;

  scaffold->has_params_override = false;
  AeParamExtractionMeta params_meta{};
  AeParamValues params_override = DefaultAeParams();
  const bool params_extract_ok =
      TryExtractPfCmdParamValues(payload, &params_override, nullptr, &params_meta);
  const bool sdk_params_available = params_extract_ok && params_meta.any_param_read;
  if (params_meta.any_param_read) {
    scaffold->host_render.params_override = params_override;
    scaffold->has_params_override = true;
  } else {
    scaffold->host_render.params_override.reset();
    scaffold->has_params_override = false;
  }
  const std::string render_param_detail =
      "cmd=" + std::to_string(static_cast<int>(payload.command)) +
      ", count_hint=" + std::to_string(params_meta.count_hint) +
      ", sdk_fallback=" + std::to_string(params_meta.used_sdk_table_fallback ? 1 : 0) +
      ", checkout_fallback=" + std::to_string(params_meta.used_checkout_fallback ? 1 : 0) +
      ", default_fallback=" + std::to_string(params_meta.used_default_fallback ? 1 : 0) +
      ", extract_ok=" + std::to_string(params_extract_ok ? 1 : 0) +
      ", sdk_params_available=" + std::to_string(sdk_params_available ? 1 : 0) +
      ", any_read=" + std::to_string(params_meta.any_param_read ? 1 : 0) +
      ", quality_popup=" +
      std::string(params_meta.has_quality_popup ? std::to_string(params_meta.raw_quality_popup)
                                                : "<none>") +
      ", output=" + std::to_string(static_cast<int>(params_override.output)) +
      ", color_map=" + std::to_string(static_cast<int>(params_override.color_map)) +
      ", slice_mode=" + std::to_string(static_cast<int>(params_override.slice_mode)) +
      ", slice_position=" + std::to_string(params_override.slice_position) +
      ", slice_range=" + std::to_string(params_override.slice_range) +
      ", slice_softness=" + std::to_string(params_override.slice_softness) +
      ", override_source=" +
      std::string(params_meta.any_param_read ? "sdk_params" : "router_current") +
      ", using_override=" + std::to_string(scaffold->has_params_override ? 1 : 0) +
      ", render_state_token=" + std::to_string(scaffold->host_render.render_state_token) + ", " +
      FormatRuntimeStateKeys(payload);
  AppendSdkTrace("render_param_extract", render_param_detail);

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

  const auto in_data_sdk_hint = InferHostRenderPixelFormatFromSdkInData(payload.in_data);
  const auto source_sdk_hint = InferHostRenderPixelFormatFromSdkWorld(source_world);
  const auto output_sdk_hint = InferHostRenderPixelFormatFromSdkWorld(output_world);
  const auto source_stride_hint =
      InferHostRenderPixelFormatFromStride(source_width, source_row_bytes);
  const auto output_stride_hint =
      InferHostRenderPixelFormatFromStride(output_width, output_row_bytes);
  const auto source_hint =
      SelectBestPixelFormatHint(source_sdk_hint, source_stride_hint, in_data_sdk_hint);
  const auto output_hint =
      SelectBestPixelFormatHint(output_sdk_hint, output_stride_hint, in_data_sdk_hint);
  const auto selected_format = SelectHostRenderPixelFormat(scaffold->pixel_format_candidates,
                                                            scaffold->pixel_format_candidate_count,
                                                            source_hint,
                                                            output_hint);

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
  scaffold->host_render.source.channel_order = zsoda::core::HostChannelOrder::kARGB;
  scaffold->host_render.destination.pixels = output_pixels;
  scaffold->host_render.destination.width = output_width;
  scaffold->host_render.destination.height = output_height;
  scaffold->host_render.destination.row_bytes = output_row_bytes;
  scaffold->host_render.destination.format = *selected_format;
  scaffold->host_render.destination.channel_order = zsoda::core::HostChannelOrder::kARGB;
  scaffold->has_host_buffers = true;
  return true;
}

bool CommitSdkRenderOutput(const AeSdkEntryPayload& payload,
                           const AeDispatchContext& dispatch,
                           std::string* error,
                           const AeSdkRenderPayloadScaffold* cached_scaffold) {
  if (payload.command != PF_Cmd_RENDER) {
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  // Use cached scaffold if provided, otherwise re-extract.
  AeSdkRenderPayloadScaffold scaffold{};
  if (cached_scaffold != nullptr) {
    scaffold = *cached_scaffold;
  } else {
    std::string extract_error;
    if (!TryExtractPfCmdRenderPayload(payload, &scaffold, &extract_error)) {
      SetError(error, "render output commit failed: " + extract_error);
      return false;
    }
  }

  if (!scaffold.has_host_buffers) {
    SetError(error, "render output commit failed: host buffers unavailable");
    return false;
  }

  const auto& output_frame = dispatch.render_response.output;
  const auto convert_status =
      (output_frame.desc().format == zsoda::core::PixelFormat::kRGBA32F &&
       output_frame.desc().channels >= 4)
          ? zsoda::core::ConvertRgba32FToHost(output_frame, scaffold.host_render.destination)
          : zsoda::core::ConvertGray32FToHost(output_frame, scaffold.host_render.destination);
  if (convert_status != zsoda::core::PixelConversionStatus::kOk) {
    SetError(error,
             std::string("render output commit failed: output->host conversion failed: ") +
                 zsoda::core::PixelConversionStatusString(convert_status));
    return false;
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

#endif

}  // namespace zsoda::ae
