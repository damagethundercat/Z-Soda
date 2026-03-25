#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ae/AeCommandRouter.h"
#include "ae/AeHostAdapter.h"
#include "ae/ZSodaAeFlags.h"
#include "inference/ManagedInferenceEngine.h"

namespace {

zsoda::core::FrameBuffer MakeFrame() {
  zsoda::core::FrameDesc desc;
  desc.width = 8;
  desc.height = 8;
  desc.channels = 1;
  desc.format = zsoda::core::PixelFormat::kGray32F;
  zsoda::core::FrameBuffer frame(desc);
  for (int y = 0; y < desc.height; ++y) {
    for (int x = 0; x < desc.width; ++x) {
      frame.at(x, y, 0) = static_cast<float>(x + y);
    }
  }
  return frame;
}

bool SetEnvironmentValue(const std::string& name, const std::string& value) {
#if defined(_WIN32)
  return _putenv_s(name.c_str(), value.c_str()) == 0;
#else
  return setenv(name.c_str(), value.c_str(), 1) == 0;
#endif
}

bool UnsetEnvironmentValue(const std::string& name) {
#if defined(_WIN32)
  return _putenv_s(name.c_str(), "") == 0;
#else
  return unsetenv(name.c_str()) == 0;
#endif
}

class ScopedEnvironmentOverride {
 public:
  ScopedEnvironmentOverride(std::string key, std::string value) : key_(std::move(key)) {
    const char* existing = std::getenv(key_.c_str());
    if (existing != nullptr) {
      had_existing_value_ = true;
      existing_value_ = existing;
    }
    assert(SetEnvironmentValue(key_, value));
  }

  ~ScopedEnvironmentOverride() {
    if (had_existing_value_) {
      assert(SetEnvironmentValue(key_, existing_value_));
    } else {
      assert(UnsetEnvironmentValue(key_));
    }
  }

 private:
  std::string key_;
  bool had_existing_value_ = false;
  std::string existing_value_;
};

int RuntimeParamSlot(zsoda::ae::AeParamId id) {
  return zsoda::ae::RuntimeParamTableIndex(id);
}

void TraceTest(const char* name) {
  const char* enabled = std::getenv("ZSODA_TEST_TRACE");
  if (enabled == nullptr || enabled[0] == '\0') {
    return;
  }
  std::fprintf(stderr, "ZSODA_TEST_TRACE %s\n", name != nullptr ? name : "<null>");
  std::fflush(stderr);
}

void TestStubCommandAndDispatchMapping() {
  assert(zsoda::ae::MapStubCommandId(0) == zsoda::ae::AeCommand::kAbout);
  assert(zsoda::ae::MapStubCommandId(1) == zsoda::ae::AeCommand::kGlobalSetup);
  assert(zsoda::ae::MapStubCommandId(2) == zsoda::ae::AeCommand::kParamsSetup);
  assert(zsoda::ae::MapStubCommandId(3) == zsoda::ae::AeCommand::kRender);
  assert(zsoda::ae::MapStubCommandId(404) == zsoda::ae::AeCommand::kUnknown);

  std::string error;
  assert(!zsoda::ae::BuildStubDispatch(1, nullptr, &error));
  assert(error == "missing dispatch output");

  zsoda::ae::AeDispatchContext render_dispatch;
  error.clear();
  assert(zsoda::ae::BuildStubDispatch(3, &render_dispatch, &error));
  assert(render_dispatch.command.command == zsoda::ae::AeCommand::kRender);
}

void TestSafeFrameHashSeed() {
  zsoda::ae::AeFrameHashSeed seed;
  seed.current_time = 120;
  seed.time_step = 2;
  seed.time_scale = 24;
  seed.width = 8;
  seed.height = 4;
  seed.source_row_bytes = 8U * 4U;
  seed.output_row_bytes = 8U * 4U;

  std::vector<std::uint8_t> source_pixels(seed.source_row_bytes * static_cast<std::size_t>(seed.height), 11);
  std::vector<std::uint8_t> output_pixels(seed.output_row_bytes * static_cast<std::size_t>(seed.height), 22);
  seed.source_pixels = source_pixels.data();
  seed.output_pixels = output_pixels.data();

  const std::uint64_t hash_a = zsoda::ae::ComputeSafeFrameHash(seed);
  const std::uint64_t hash_b = zsoda::ae::ComputeSafeFrameHash(seed);
  assert(hash_a != 0);
  assert(hash_a == hash_b);

  seed.current_time += 1;
  const std::uint64_t hash_c = zsoda::ae::ComputeSafeFrameHash(seed);
  assert(hash_c != hash_a);
}

void TestPixelFormatHelpers() {
  std::array<zsoda::core::PixelFormat, zsoda::ae::kAePixelFormatCandidateCapacity> candidates{};
  const std::size_t count =
      zsoda::ae::BuildHostRenderPixelFormatCandidates(8, 8U * 16U, &candidates);
  assert(count == 3U);
  assert(zsoda::ae::InferHostRenderPixelFormatFromStride(8, 8U * 4U) ==
         zsoda::core::PixelFormat::kRGBA8);
}

void TestAeGlobalOutFlagsDoNotAdvertiseFloatColorAwareWithoutSmartFx() {
#if defined(PF_OutFlag2_FLOAT_COLOR_AWARE)
  assert((ZSODA_AE_GLOBAL_OUTFLAGS2 & PF_OutFlag2_FLOAT_COLOR_AWARE) == 0);
#endif
#if defined(PF_OutFlag2_SUPPORTS_SMART_RENDER)
  assert((ZSODA_AE_GLOBAL_OUTFLAGS2 & PF_OutFlag2_SUPPORTS_SMART_RENDER) == 0);
#endif
}

void TestParamSetupAndModelMenu() {
  TraceTest("TestParamSetupAndModelMenu/create_engine");
  auto engine = std::make_shared<zsoda::inference::ManagedInferenceEngine>("models");
  std::string error;
  TraceTest("TestParamSetupAndModelMenu/initialize");
  assert(engine->Initialize("distill-any-depth-base", &error));
  TraceTest("TestParamSetupAndModelMenu/create_pipeline");
  auto pipeline = std::make_shared<zsoda::core::RenderPipeline>(engine);
  zsoda::ae::AeCommandRouter router(pipeline, engine);

  zsoda::ae::AeHostCommandContext host;
  host.command_id = 1;
  zsoda::ae::AeCommandContext setup_context;
  setup_context.command = zsoda::ae::AeCommand::kGlobalSetup;
  setup_context.host = &host;
  setup_context.error = &error;
  TraceTest("TestParamSetupAndModelMenu/global_setup");
  assert(router.Handle(setup_context));
  TraceTest("TestParamSetupAndModelMenu/model_menu_nonempty");
  assert(!router.ModelMenu().empty());

  TraceTest("TestParamSetupAndModelMenu/copy_params");
  auto params = router.CurrentParams();
  params.model_id = router.ModelMenu().front();
  params.output = zsoda::ae::AeOutputSelection::kDepthSlice;
  params.color_map = zsoda::ae::AeDepthColorMapSelection::kTurbo;
  params.slice_mode = zsoda::ae::AeSliceModeSelection::kBand;
  params.slice_position = 0.68F;
  params.slice_range = 0.02F;
  params.slice_softness = 0.05F;
  zsoda::ae::AeCommandContext update_context;
  update_context.command = zsoda::ae::AeCommand::kUpdateParams;
  update_context.params_update = &params;
  update_context.error = &error;
  TraceTest("TestParamSetupAndModelMenu/update_params");
  assert(router.Handle(update_context));
  TraceTest("TestParamSetupAndModelMenu/verify");
  assert(router.CurrentParams().model_id == router.ModelMenu().front());
  assert(router.CurrentParams().output == zsoda::ae::AeOutputSelection::kDepthSlice);
  assert(router.CurrentParams().color_map == zsoda::ae::AeDepthColorMapSelection::kTurbo);
  assert(router.CurrentParams().slice_mode == zsoda::ae::AeSliceModeSelection::kBand);
}

void TestRenderUsesCurrentAndOverrideParams() {
  auto engine = std::make_shared<zsoda::inference::ManagedInferenceEngine>("models");
  std::string error;
  assert(engine->Initialize("distill-any-depth-base", &error));
  auto pipeline = std::make_shared<zsoda::core::RenderPipeline>(engine);
  zsoda::ae::AeCommandRouter router(pipeline, engine);

  zsoda::ae::AeHostCommandContext host;
  host.command_id = 1;
  zsoda::ae::AeCommandContext setup_context;
  setup_context.command = zsoda::ae::AeCommand::kGlobalSetup;
  setup_context.host = &host;
  setup_context.error = &error;
  assert(router.Handle(setup_context));

  zsoda::ae::RenderRequest request;
  request.source = MakeFrame();
  request.frame_hash = 9090;

  zsoda::ae::RenderResponse response;
  zsoda::ae::AeCommandContext render_context;
  render_context.command = zsoda::ae::AeCommand::kRender;
  render_context.host = &host;
  render_context.render_request = &request;
  render_context.render_response = &response;
  render_context.error = &error;
  assert(router.Handle(render_context));

  zsoda::ae::RenderRequest override_request = request;
  override_request.frame_hash = 9091;
  auto override_params = router.CurrentParams();
  override_params.quality = 5;
  override_params.output = zsoda::ae::AeOutputSelection::kDepthSlice;
  override_params.color_map = zsoda::ae::AeDepthColorMapSelection::kTurbo;
  override_params.slice_mode = zsoda::ae::AeSliceModeSelection::kBand;
  override_params.slice_position = 0.44F;
  override_params.slice_range = 0.08F;
  override_request.params_override = override_params;

  zsoda::ae::RenderResponse override_response;
  render_context.render_request = &override_request;
  render_context.render_response = &override_response;
  assert(router.Handle(render_context));
  assert(!override_response.message.empty());
}

void TestRuntimeParamSlotMapping() {
  assert(RuntimeParamSlot(zsoda::ae::AeParamId::kQuality) == 1);
  assert(RuntimeParamSlot(zsoda::ae::AeParamId::kPreserveRatio) == 2);
  assert(RuntimeParamSlot(zsoda::ae::AeParamId::kOutput) == 3);
  assert(RuntimeParamSlot(zsoda::ae::AeParamId::kColorMap) == 4);
  assert(RuntimeParamSlot(zsoda::ae::AeParamId::kSliceMode) == 5);
  assert(RuntimeParamSlot(zsoda::ae::AeParamId::kSlicePosition) == 6);
  assert(RuntimeParamSlot(zsoda::ae::AeParamId::kSliceRange) == 7);
  assert(RuntimeParamSlot(zsoda::ae::AeParamId::kSliceSoftness) == 8);
  assert(zsoda::ae::AeParamIdFromRuntimeParamIndex(1) == zsoda::ae::AeParamId::kQuality);
  assert(zsoda::ae::AeParamIdFromRuntimeParamIndex(3) == zsoda::ae::AeParamId::kOutput);
  assert(zsoda::ae::AeParamIdFromRuntimeParamIndex(4) == zsoda::ae::AeParamId::kColorMap);
  assert(zsoda::ae::AeParamIdFromRuntimeParamIndex(8) == zsoda::ae::AeParamId::kSliceSoftness);
  assert(!zsoda::ae::AeParamIdFromRuntimeParamIndex(9).has_value());
}

void TestRenderBridgeFrameHashCacheBehavior() {
  ScopedEnvironmentOverride force_temporal_alpha("ZSODA_TEMPORAL_ALPHA", "1");

  auto engine = std::make_shared<zsoda::inference::ManagedInferenceEngine>("models");
  std::string error;
  assert(engine->Initialize("distill-any-depth-base", &error));
  auto pipeline = std::make_shared<zsoda::core::RenderPipeline>(engine);
  zsoda::ae::AeCommandRouter router(pipeline, engine);

  zsoda::ae::RenderRequest request;
  request.source = MakeFrame();
  request.frame_hash = 5001;

  zsoda::ae::AeHostCommandContext render_host;
  render_host.command_id = 3;
  zsoda::ae::AeCommandContext render_context;
  render_context.command = zsoda::ae::AeCommand::kRender;
  render_context.host = &render_host;
  render_context.render_request = &request;
  render_context.error = &error;

  zsoda::ae::RenderResponse first;
  render_context.render_response = &first;
  assert(router.Handle(render_context));

  zsoda::ae::RenderResponse second;
  render_context.render_response = &second;
  assert(router.Handle(render_context));
  assert(second.status == zsoda::core::RenderStatus::kCacheHit);
}

#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK
void ResetSdkAdapterState() {
  PF_OutData out_data{};
  zsoda::ae::AeSdkEntryPayload payload{};
  payload.command = PF_Cmd_GLOBAL_SETUP;
  payload.out_data = &out_data;

  zsoda::ae::AeDispatchContext dispatch;
  std::string error;
  assert(zsoda::ae::BuildSdkDispatch(payload, &dispatch, &error));
}

void TestSdkRenderDispatchReadsCoreParams() {
  ResetSdkAdapterState();

  constexpr int kWidth = 4;
  constexpr int kHeight = 2;
  constexpr int kRowBytes = kWidth * 4;
  constexpr int kParamCount = static_cast<int>(zsoda::ae::AeParamId::kLast) + 1;

  std::array<std::uint8_t, kRowBytes * kHeight> src_pixels{};
  std::array<std::uint8_t, kRowBytes * kHeight> out_pixels{};
  PF_LayerDef src_world{};
  src_world.width = kWidth;
  src_world.height = kHeight;
  src_world.rowbytes = kRowBytes;
  src_world.data = reinterpret_cast<PF_PixelPtr>(src_pixels.data());

  PF_LayerDef out_world{};
  out_world.width = kWidth;
  out_world.height = kHeight;
  out_world.rowbytes = kRowBytes;
  out_world.data = reinterpret_cast<PF_PixelPtr>(out_pixels.data());

  std::array<PF_ParamDef, kParamCount> params{};
  std::array<PF_ParamDef*, kParamCount> param_ptrs{};
  for (int i = 0; i < kParamCount; ++i) {
    param_ptrs[static_cast<std::size_t>(i)] = &params[static_cast<std::size_t>(i)];
  }

  PF_ParamDef input_param{};
  input_param.u.ld = src_world;
  param_ptrs[0] = &input_param;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kQuality)].u.pd.value = 6;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kPreserveRatio)].u.bd.value = 1;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kOutput)].u.pd.value = 2;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kColorMap)].u.pd.value = 2;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kSliceMode)].u.pd.value = 3;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kSlicePosition)].u.fs_d.value = 68.0;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kSliceRange)].u.fs_d.value = 2.0;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kSliceSoftness)].u.fs_d.value = 5.0;

  PF_InData render_in_data{};
  render_in_data.num_params = kParamCount;
  PF_OutData render_out_data{};
  render_out_data.num_params = kParamCount;

  zsoda::ae::AeSdkEntryPayload render_payload{};
  render_payload.command = PF_Cmd_RENDER;
  render_payload.in_data = &render_in_data;
  render_payload.out_data = &render_out_data;
  render_payload.params = param_ptrs.data();
  render_payload.output = &out_world;

  zsoda::ae::AeSdkRenderPayloadScaffold scaffold{};
  std::string error;
  assert(zsoda::ae::TryExtractPfCmdRenderPayload(render_payload, &scaffold, &error));
  assert(scaffold.has_params_override);
  assert(scaffold.host_render.params_override.has_value());
  assert(scaffold.host_render.params_override->output == zsoda::ae::AeOutputSelection::kDepthSlice);
  assert(scaffold.host_render.params_override->color_map == zsoda::ae::AeDepthColorMapSelection::kTurbo);
  assert(scaffold.host_render.params_override->slice_mode == zsoda::ae::AeSliceModeSelection::kBand);
}

void TestSdkUserChangedParamDispatchRequestsRerenderWithoutMutatingChangeFlags() {
  ResetSdkAdapterState();

  constexpr int kParamCount = static_cast<int>(zsoda::ae::AeParamId::kLast) + 1;
  std::array<PF_ParamDef, kParamCount> params{};
  std::array<PF_ParamDef*, kParamCount> param_ptrs{};
  for (int i = 0; i < kParamCount; ++i) {
    param_ptrs[static_cast<std::size_t>(i)] = &params[static_cast<std::size_t>(i)];
  }

  params[RuntimeParamSlot(zsoda::ae::AeParamId::kQuality)].u.pd.value = 6;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kPreserveRatio)].u.bd.value = 1;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kOutput)].u.pd.value = 2;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kColorMap)].u.pd.value = 2;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kSliceMode)].u.pd.value = 3;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kSlicePosition)].u.fs_d.value = 76.0;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kSliceRange)].u.fs_d.value = 14.0;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kSliceSoftness)].u.fs_d.value = 9.0;

  PF_UserChangedParamExtra extra{};
  extra.param_index = RuntimeParamSlot(zsoda::ae::AeParamId::kSlicePosition);

  PF_InData in_data{};
  in_data.num_params = kParamCount;
  PF_OutData out_data{};
  out_data.num_params = kParamCount;

  zsoda::ae::AeSdkEntryPayload payload{};
  payload.command = PF_Cmd_USER_CHANGED_PARAM;
  payload.in_data = &in_data;
  payload.out_data = &out_data;
  payload.params = param_ptrs.data();
  payload.extra = &extra;

  zsoda::ae::AeDispatchContext dispatch;
  std::string error;
  assert(zsoda::ae::BuildSdkDispatch(payload, &dispatch, &error));
  assert(dispatch.command.command == zsoda::ae::AeCommand::kUpdateParams);
  assert(dispatch.command.params_update == &dispatch.params_update);
  assert(dispatch.params_update.output == zsoda::ae::AeOutputSelection::kDepthSlice);
  assert(dispatch.params_update.color_map == zsoda::ae::AeDepthColorMapSelection::kTurbo);
  assert(dispatch.params_update.slice_mode == zsoda::ae::AeSliceModeSelection::kBand);
  assert(std::fabs(dispatch.params_update.slice_position - 0.76F) < 1.0e-6F);
  assert(std::fabs(dispatch.params_update.slice_range - 0.14F) < 1.0e-6F);
  assert(std::fabs(dispatch.params_update.slice_softness - 0.09F) < 1.0e-6F);
#if defined(PF_OutFlag_FORCE_RERENDER)
  assert((out_data.out_flags & PF_OutFlag_FORCE_RERENDER) != 0);
#endif
  assert(params[RuntimeParamSlot(zsoda::ae::AeParamId::kSlicePosition)].uu.change_flags == 0);
}

void TestSdkRenderDispatchSkipsOverrideWhenParamsUnavailable() {
  ResetSdkAdapterState();

  constexpr int kWidth = 4;
  constexpr int kHeight = 2;
  constexpr int kRowBytes = kWidth * 4;

  std::array<std::uint8_t, kRowBytes * kHeight> src_pixels{};
  std::array<std::uint8_t, kRowBytes * kHeight> out_pixels{};
  PF_LayerDef src_world{};
  src_world.width = kWidth;
  src_world.height = kHeight;
  src_world.rowbytes = kRowBytes;
  src_world.data = reinterpret_cast<PF_PixelPtr>(src_pixels.data());

  PF_LayerDef out_world{};
  out_world.width = kWidth;
  out_world.height = kHeight;
  out_world.rowbytes = kRowBytes;
  out_world.data = reinterpret_cast<PF_PixelPtr>(out_pixels.data());

  constexpr int kParamCount = static_cast<int>(zsoda::ae::AeParamId::kLast) + 1;
  std::array<PF_ParamDef, kParamCount> params{};
  std::array<PF_ParamDef*, kParamCount> param_ptrs{};
  for (int i = 0; i < kParamCount; ++i) {
    param_ptrs[static_cast<std::size_t>(i)] = nullptr;
  }

  PF_ParamDef input_param{};
  input_param.u.ld = src_world;
  param_ptrs[0] = &input_param;

  PF_InData render_in_data{};
  render_in_data.num_params = 0;
  PF_OutData render_out_data{};
  render_out_data.num_params = 0;

  zsoda::ae::AeSdkEntryPayload render_payload{};
  render_payload.command = PF_Cmd_RENDER;
  render_payload.in_data = &render_in_data;
  render_payload.out_data = &render_out_data;
  render_payload.params = param_ptrs.data();
  render_payload.output = &out_world;

  zsoda::ae::AeSdkRenderPayloadScaffold scaffold{};
  std::string error;
  assert(zsoda::ae::TryExtractPfCmdRenderPayload(render_payload, &scaffold, &error));
  assert(!scaffold.has_params_override);
  assert(!scaffold.host_render.params_override.has_value());
}

void TestSdkSequenceResetupKeepsSequenceDataDisabled() {
  ResetSdkAdapterState();

  constexpr int kParamCount = static_cast<int>(zsoda::ae::AeParamId::kLast) + 1;
  std::array<PF_ParamDef, kParamCount> params{};
  std::array<PF_ParamDef*, kParamCount> param_ptrs{};
  for (int i = 0; i < kParamCount; ++i) {
    param_ptrs[static_cast<std::size_t>(i)] = &params[static_cast<std::size_t>(i)];
  }

  params[RuntimeParamSlot(zsoda::ae::AeParamId::kQuality)].u.pd.value = 4;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kPreserveRatio)].u.bd.value = 0;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kOutput)].u.pd.value = 2;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kColorMap)].u.pd.value = 2;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kSliceMode)].u.pd.value = 3;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kSlicePosition)].u.fs_d.value = 50.0;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kSliceRange)].u.fs_d.value = 20.0;
  params[RuntimeParamSlot(zsoda::ae::AeParamId::kSliceSoftness)].u.fs_d.value = 15.0;

  PF_InData in_data{};
  in_data.num_params = 1;
  PF_OutData out_data{};
  out_data.num_params = kParamCount;

  zsoda::ae::AeSdkEntryPayload payload{};
  payload.command = PF_Cmd_SEQUENCE_RESETUP;
  payload.in_data = &in_data;
  payload.out_data = &out_data;
  payload.params = param_ptrs.data();

  zsoda::ae::AeDispatchContext dispatch;
  std::string error;
  assert(zsoda::ae::BuildSdkDispatch(payload, &dispatch, &error));
  assert(dispatch.command.command == zsoda::ae::AeCommand::kUnknown);
  assert(out_data.sequence_data == nullptr);
}

void TestSdkFrameLifecycleKeepsFrameDataDisabled() {
  ResetSdkAdapterState();

  PF_InData in_data{};
  PF_OutData out_data{};
  out_data.frame_data = reinterpret_cast<PF_Handle>(static_cast<uintptr_t>(0x1));

  zsoda::ae::AeSdkEntryPayload payload{};
  payload.in_data = &in_data;
  payload.out_data = &out_data;

  zsoda::ae::AeDispatchContext dispatch;
  std::string error;

  payload.command = PF_Cmd_FRAME_SETUP;
  assert(zsoda::ae::BuildSdkDispatch(payload, &dispatch, &error));
  assert(dispatch.command.command == zsoda::ae::AeCommand::kUnknown);
  assert(out_data.frame_data == nullptr);

  out_data.frame_data = reinterpret_cast<PF_Handle>(static_cast<uintptr_t>(0x1));
  payload.command = PF_Cmd_FRAME_SETDOWN;
  assert(zsoda::ae::BuildSdkDispatch(payload, &dispatch, &error));
  assert(dispatch.command.command == zsoda::ae::AeCommand::kUnknown);
  assert(out_data.frame_data == nullptr);
}
#endif

void TestExecuteHostBufferRenderBridge() {
  ScopedEnvironmentOverride force_temporal_alpha("ZSODA_TEMPORAL_ALPHA", "1");

  auto engine = std::make_shared<zsoda::inference::ManagedInferenceEngine>("models");
  std::string error;
  assert(engine->Initialize("distill-any-depth-base", &error));
  auto pipeline = std::make_shared<zsoda::core::RenderPipeline>(engine);
  zsoda::ae::AeCommandRouter router(pipeline, engine);

  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  constexpr int kRowBytes = kWidth * 4;
  std::vector<std::uint8_t> rgba_src(kRowBytes * kHeight, 0);
  std::vector<std::uint8_t> rgba_out(kRowBytes * kHeight, 0);

  zsoda::ae::AeHostRenderBridgePayload payload;
  payload.source.pixels = rgba_src.data();
  payload.source.width = kWidth;
  payload.source.height = kHeight;
  payload.source.row_bytes = kRowBytes;
  payload.source.format = zsoda::core::PixelFormat::kRGBA8;
  payload.destination.pixels = rgba_out.data();
  payload.destination.width = kWidth;
  payload.destination.height = kHeight;
  payload.destination.row_bytes = kRowBytes;
  payload.destination.format = zsoda::core::PixelFormat::kRGBA8;
  payload.frame_hash = 8301;

  zsoda::ae::AeHostRenderBridgeResult result;
  assert(zsoda::ae::ExecuteHostBufferRenderBridge(&router, payload, &result, &error));
}

void TestRouterPayloadValidation() {
  auto engine = std::make_shared<zsoda::inference::ManagedInferenceEngine>("models");
  std::string error;
  assert(engine->Initialize("distill-any-depth-base", &error));
  auto pipeline = std::make_shared<zsoda::core::RenderPipeline>(engine);
  zsoda::ae::AeCommandRouter router(pipeline, engine);

  zsoda::ae::AeCommandContext update_context;
  update_context.command = zsoda::ae::AeCommand::kUpdateParams;
  update_context.error = &error;
  assert(!router.Handle(update_context));
  assert(error == "missing params update payload");
}

}  // namespace

void RunAeRouterTests() {
  TraceTest("TestStubCommandAndDispatchMapping");
  TestStubCommandAndDispatchMapping();
  TraceTest("TestSafeFrameHashSeed");
  TestSafeFrameHashSeed();
  TraceTest("TestPixelFormatHelpers");
  TestPixelFormatHelpers();
  TraceTest("TestAeGlobalOutFlagsDoNotAdvertiseFloatColorAwareWithoutSmartFx");
  TestAeGlobalOutFlagsDoNotAdvertiseFloatColorAwareWithoutSmartFx();
  TraceTest("TestParamSetupAndModelMenu");
  TestParamSetupAndModelMenu();
  TraceTest("TestRuntimeParamSlotMapping");
  TestRuntimeParamSlotMapping();
  TraceTest("TestRenderUsesCurrentAndOverrideParams");
  TestRenderUsesCurrentAndOverrideParams();
  TraceTest("TestRenderBridgeFrameHashCacheBehavior");
  TestRenderBridgeFrameHashCacheBehavior();
#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK
  TraceTest("TestSdkRenderDispatchReadsCoreParams");
  TestSdkRenderDispatchReadsCoreParams();
  TraceTest("TestSdkUserChangedParamDispatchRequestsRerenderWithoutMutatingChangeFlags");
  TestSdkUserChangedParamDispatchRequestsRerenderWithoutMutatingChangeFlags();
  TraceTest("TestSdkRenderDispatchSkipsOverrideWhenParamsUnavailable");
  TestSdkRenderDispatchSkipsOverrideWhenParamsUnavailable();
  TraceTest("TestSdkSequenceResetupKeepsSequenceDataDisabled");
  TestSdkSequenceResetupKeepsSequenceDataDisabled();
  TraceTest("TestSdkFrameLifecycleKeepsFrameDataDisabled");
  TestSdkFrameLifecycleKeepsFrameDataDisabled();
#endif
  TraceTest("TestExecuteHostBufferRenderBridge");
  TestExecuteHostBufferRenderBridge();
  TraceTest("TestRouterPayloadValidation");
  TestRouterPayloadValidation();
}
