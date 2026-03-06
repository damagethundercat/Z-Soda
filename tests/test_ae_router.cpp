#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ae/AeHostAdapter.h"
#include "ae/AeCommandRouter.h"
#include "inference/ManagedInferenceEngine.h"

namespace {

extern "C" int ZSodaEffectMainStub(int command_id);
extern "C" int ZSodaSetModelIdStub(const char* model_id);
extern "C" int ZSodaRenderGrayFrameStub(const float* src,
                                        int width,
                                        int height,
                                        std::uint64_t frame_hash,
                                        float* out,
                                        int out_size);
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
                                  int vram_budget_mb);
extern "C" int ZSodaRenderHostBufferStub(const void* src,
                                         int width,
                                         int height,
                                         int src_row_bytes,
                                         int pixel_format,
                                         std::uint64_t frame_hash,
                                         void* out,
                                         int out_row_bytes);

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
  assert(render_dispatch.host.command_id == 3);
  assert(render_dispatch.command.command == zsoda::ae::AeCommand::kRender);
  assert(render_dispatch.command.host == &render_dispatch.host);
  assert(render_dispatch.command.error == &error);
  assert(render_dispatch.command.params_update == nullptr);
  assert(render_dispatch.command.render_request == nullptr);
  assert(render_dispatch.command.render_response == nullptr);

  zsoda::ae::AeDispatchContext unknown_dispatch;
  assert(zsoda::ae::BuildStubDispatch(99, &unknown_dispatch, &error));
  assert(unknown_dispatch.command.command == zsoda::ae::AeCommand::kUnknown);
}

void TestSafeFrameHashSeed() {
  zsoda::ae::AeFrameHashSeed seed;
  seed.current_time = 120;
  seed.time_step = 2;
  seed.time_scale = 24;
  seed.width = 1920;
  seed.height = 1080;
  seed.source_row_bytes = 1920U * 16U;
  seed.output_row_bytes = 1920U * 16U;

  int source_token = 11;
  int output_token = 22;
  seed.source_pixels = &source_token;
  seed.output_pixels = &output_token;

  const std::uint64_t hash_a = zsoda::ae::ComputeSafeFrameHash(seed);
  const std::uint64_t hash_b = zsoda::ae::ComputeSafeFrameHash(seed);
  assert(hash_a != 0);
  assert(hash_a == hash_b);

  seed.current_time += 1;
  const std::uint64_t hash_c = zsoda::ae::ComputeSafeFrameHash(seed);
  assert(hash_c != hash_a);

  zsoda::ae::AeFrameHashSeed empty_seed;
  const std::uint64_t empty_hash = zsoda::ae::ComputeSafeFrameHash(empty_seed);
  assert(empty_hash != 0);
}

void TestPixelFormatCandidatesFromRowBytes() {
  std::array<zsoda::core::PixelFormat, zsoda::ae::kAePixelFormatCandidateCapacity> candidates{};

  std::size_t count =
      zsoda::ae::BuildHostRenderPixelFormatCandidates(8, 8U * 16U, &candidates);
  assert(count == 3U);
  assert(candidates[0] == zsoda::core::PixelFormat::kRGBA8);
  assert(candidates[1] == zsoda::core::PixelFormat::kRGBA16);
  assert(candidates[2] == zsoda::core::PixelFormat::kRGBA32F);

  count = zsoda::ae::BuildHostRenderPixelFormatCandidates(8, 8U * 8U, &candidates);
  assert(count == 2U);
  assert(candidates[0] == zsoda::core::PixelFormat::kRGBA8);
  assert(candidates[1] == zsoda::core::PixelFormat::kRGBA16);

  count = zsoda::ae::BuildHostRenderPixelFormatCandidates(8, 8U * 4U, &candidates);
  assert(count == 1U);
  assert(candidates[0] == zsoda::core::PixelFormat::kRGBA8);

  count = zsoda::ae::BuildHostRenderPixelFormatCandidates(0, 0U, &candidates);
  assert(count == zsoda::ae::kAePixelFormatCandidateCapacity);
  assert(candidates[0] == zsoda::core::PixelFormat::kRGBA8);
  assert(candidates[1] == zsoda::core::PixelFormat::kRGBA16);
  assert(candidates[2] == zsoda::core::PixelFormat::kRGBA32F);

  assert(zsoda::ae::BuildHostRenderPixelFormatCandidates(8, 32U, nullptr) == 0U);
}

void TestPixelFormatInferenceFromStride() {
  assert(zsoda::ae::InferHostRenderPixelFormatFromStride(8, 8U * 4U) ==
         zsoda::core::PixelFormat::kRGBA8);
  assert(zsoda::ae::InferHostRenderPixelFormatFromStride(8, 8U * 8U) ==
         zsoda::core::PixelFormat::kRGBA16);
  assert(zsoda::ae::InferHostRenderPixelFormatFromStride(8, 8U * 16U) ==
         zsoda::core::PixelFormat::kRGBA32F);
  assert(!zsoda::ae::InferHostRenderPixelFormatFromStride(8, 65U).has_value());
  assert(!zsoda::ae::InferHostRenderPixelFormatFromStride(0, 64U).has_value());
}

void TestSelectHostRenderPixelFormat() {
  std::array<zsoda::core::PixelFormat, zsoda::ae::kAePixelFormatCandidateCapacity> candidates{};
  candidates[0] = zsoda::core::PixelFormat::kRGBA32F;
  candidates[1] = zsoda::core::PixelFormat::kRGBA16;
  candidates[2] = zsoda::core::PixelFormat::kRGBA8;

  assert(zsoda::ae::SelectHostRenderPixelFormat(candidates,
                                                3,
                                                zsoda::core::PixelFormat::kRGBA16,
                                                zsoda::core::PixelFormat::kRGBA16) ==
         zsoda::core::PixelFormat::kRGBA16);
  assert(zsoda::ae::SelectHostRenderPixelFormat(candidates,
                                                3,
                                                zsoda::core::PixelFormat::kRGBA8,
                                                std::nullopt) == zsoda::core::PixelFormat::kRGBA8);
  assert(!zsoda::ae::SelectHostRenderPixelFormat(candidates,
                                                 3,
                                                 zsoda::core::PixelFormat::kRGBA8,
                                                 zsoda::core::PixelFormat::kRGBA16)
              .has_value());
  assert(zsoda::ae::SelectHostRenderPixelFormat(
             candidates, 1, std::nullopt, std::nullopt) == zsoda::core::PixelFormat::kRGBA32F);
}

void TestParamSetupAndModelMenu() {
  auto engine = std::make_shared<zsoda::inference::ManagedInferenceEngine>("models");
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));
  auto pipeline = std::make_shared<zsoda::core::RenderPipeline>(engine);
  zsoda::ae::AeCommandRouter router(pipeline, engine);

  zsoda::ae::AeHostCommandContext host;
  host.command_id = 1;
  zsoda::ae::AeCommandContext setup_context;
  setup_context.command = zsoda::ae::AeCommand::kGlobalSetup;
  setup_context.host = &host;
  setup_context.error = &error;
  assert(router.Handle(setup_context));
  assert(!router.ModelMenu().empty());

  auto params = router.CurrentParams();
  params.model_id = router.ModelMenu().front();
  params.quality_boost_enabled = true;
  params.quality_boost_level = 4;
  zsoda::ae::AeCommandContext update_context;
  update_context.command = zsoda::ae::AeCommand::kUpdateParams;
  update_context.params_update = &params;
  update_context.error = &error;
  assert(router.Handle(update_context));
  assert(router.CurrentParams().model_id == router.ModelMenu().front());
  assert(router.CurrentParams().quality_boost_enabled);
  assert(router.CurrentParams().quality_boost_level == 4);

  params.model_id = "unknown-model-id";
  assert(!router.Handle(update_context));
  assert(!error.empty());
}

void TestRenderUsesCurrentAndOverrideParams() {
  auto engine = std::make_shared<zsoda::inference::ManagedInferenceEngine>("models");
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));
  auto pipeline = std::make_shared<zsoda::core::RenderPipeline>(engine);
  zsoda::ae::AeCommandRouter router(pipeline, engine);

  zsoda::ae::AeHostCommandContext host;
  host.command_id = 1;
  zsoda::ae::AeCommandContext setup_context;
  setup_context.command = zsoda::ae::AeCommand::kGlobalSetup;
  setup_context.host = &host;
  setup_context.error = &error;
  assert(router.Handle(setup_context));

  auto params = router.CurrentParams();
  params.quality_boost_enabled = true;
  params.quality_boost_level = 4;
  zsoda::ae::AeCommandContext update_context;
  update_context.command = zsoda::ae::AeCommand::kUpdateParams;
  update_context.params_update = &params;
  update_context.error = &error;
  assert(router.Handle(update_context));

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
  const auto expected_current_model = zsoda::ae::ToRenderParams(router.CurrentParams()).model_id;
  assert(response.message.find(expected_current_model) != std::string::npos);

  zsoda::ae::RenderRequest override_request = request;
  override_request.frame_hash = 9091;
  auto override_params = router.CurrentParams();
  override_params.model_id = "depth-anything-v3-small";
  override_request.params_override = override_params;

  zsoda::ae::RenderResponse override_response;
  render_context.render_request = &override_request;
  render_context.render_response = &override_response;
  assert(router.Handle(render_context));
  const auto expected_override_model = zsoda::ae::ToRenderParams(override_params).model_id;
  assert(override_response.message.find(expected_override_model) != std::string::npos);
  assert(!response.message.empty());
}

void TestRenderBridgeFrameHashCacheBehavior() {
  ScopedEnvironmentOverride force_temporal_alpha("ZSODA_TEMPORAL_ALPHA", "1");

  auto engine = std::make_shared<zsoda::inference::ManagedInferenceEngine>("models");
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));
  auto pipeline = std::make_shared<zsoda::core::RenderPipeline>(engine);
  zsoda::ae::AeCommandRouter router(pipeline, engine);

  zsoda::ae::AeHostCommandContext setup_host;
  setup_host.command_id = 1;
  zsoda::ae::AeCommandContext setup_context;
  setup_context.command = zsoda::ae::AeCommand::kGlobalSetup;
  setup_context.host = &setup_host;
  setup_context.error = &error;
  assert(router.Handle(setup_context));

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
  assert(first.status == zsoda::core::RenderStatus::kInference);

  zsoda::ae::RenderResponse second;
  render_context.render_response = &second;
  assert(router.Handle(render_context));
  assert(second.status == zsoda::core::RenderStatus::kCacheHit);

  request.frame_hash = 5002;
  zsoda::ae::RenderResponse third;
  render_context.render_response = &third;
  assert(router.Handle(render_context));
  assert(third.status == zsoda::core::RenderStatus::kInference);
}

void TestHostBufferRenderDispatchConversion() {
  constexpr int kWidth = 2;
  constexpr int kHeight = 1;
  constexpr int kRowBytes = kWidth * 4;

  std::vector<std::uint8_t> rgba(kRowBytes * kHeight, 0);
  rgba[0] = 255;
  rgba[1] = 0;
  rgba[2] = 0;
  rgba[3] = 255;
  rgba[4] = 0;
  rgba[5] = 255;
  rgba[6] = 0;
  rgba[7] = 255;

  zsoda::ae::AeHostRenderBridgePayload payload;
  payload.source.pixels = rgba.data();
  payload.source.width = kWidth;
  payload.source.height = kHeight;
  payload.source.row_bytes = kRowBytes;
  payload.source.format = zsoda::core::PixelFormat::kRGBA8;
  payload.destination.pixels = rgba.data();
  payload.destination.width = kWidth;
  payload.destination.height = kHeight;
  payload.destination.row_bytes = kRowBytes;
  payload.destination.format = zsoda::core::PixelFormat::kRGBA8;
  payload.frame_hash = 404;

  zsoda::ae::AeDispatchContext dispatch;
  std::string error;
  assert(zsoda::ae::BuildHostBufferRenderDispatch(payload, &dispatch, &error));
  assert(dispatch.command.command == zsoda::ae::AeCommand::kRender);
  assert(dispatch.command.render_request == &dispatch.render_request);
  assert(dispatch.command.render_response == &dispatch.render_response);
  assert(dispatch.render_request.frame_hash == 404);
  assert(dispatch.render_request.source.desc().width == kWidth);
  assert(dispatch.render_request.source.desc().height == kHeight);
  assert(dispatch.render_request.source.desc().channels == 4);
  assert(dispatch.render_request.source.desc().format == zsoda::core::PixelFormat::kRGBA32F);
  assert(dispatch.render_request.source.at(0, 0, 0) > 0.9F);
  assert(dispatch.render_request.source.at(0, 0, 1) < 0.1F);
  assert(dispatch.render_request.source.at(0, 0, 2) < 0.1F);
  assert(dispatch.render_request.source.at(1, 0, 0) < 0.1F);
  assert(dispatch.render_request.source.at(1, 0, 1) > 0.9F);
  assert(dispatch.render_request.source.at(1, 0, 2) < 0.1F);
}

void TestHostBufferRenderDispatchValidation() {
  constexpr int kWidth = 2;
  constexpr int kHeight = 1;
  std::vector<float> gray(kWidth * kHeight, 0.5F);

  zsoda::ae::AeHostRenderBridgePayload payload;
  payload.source.pixels = gray.data();
  payload.source.width = kWidth;
  payload.source.height = kHeight;
  payload.source.row_bytes = static_cast<std::size_t>(kWidth * sizeof(float));
  payload.source.format = zsoda::core::PixelFormat::kGray32F;
  payload.destination.pixels = gray.data();
  payload.destination.width = kWidth;
  payload.destination.height = kHeight;
  payload.destination.row_bytes = static_cast<std::size_t>(kWidth * sizeof(float));
  payload.destination.format = zsoda::core::PixelFormat::kRGBA32F;

  zsoda::ae::AeDispatchContext dispatch;
  std::string error;
  assert(!zsoda::ae::BuildHostBufferRenderDispatch(payload, &dispatch, &error));
  assert(error.find("host->rgb conversion failed") != std::string::npos);
}

#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK
void TestSdkRenderDispatchReadsParamsWhenNumParamsHintIsInputOnly() {
  constexpr int kWidth = 4;
  constexpr int kHeight = 2;
  constexpr int kRowBytes = kWidth * 4;
  constexpr int kParamCount = 20;

  std::array<std::uint8_t, kRowBytes * kHeight> src_pixels{};
  std::array<std::uint8_t, kRowBytes * kHeight> out_pixels{};
  src_pixels.fill(128U);
  out_pixels.fill(0U);

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

  params[0].u.ld = src_world;
  params[1].u.pd.value = 2;  // quality: 512 px
  params[2].u.bd.value = 1;  // preserve ratio
  params[3].u.bd.value = 1;  // quality boost enabled
  params[4].u.pd.value = 3;  // boost: 4x4
  params[7].u.pd.value = 1;  // locked model popup

  PF_InData in_data{};
  in_data.num_params = 1;  // Host may report input-only count on render paths.
  in_data.current_time = 100;
  in_data.time_step = 1;
  in_data.time_scale = 24;

  PF_OutData out_data{};
  out_data.num_params = kParamCount;

  zsoda::ae::AeSdkEntryPayload payload{};
  payload.command = PF_Cmd_RENDER;
  payload.in_data = &in_data;
  payload.out_data = &out_data;
  payload.params = param_ptrs.data();
  payload.output = &out_world;

  zsoda::ae::AeDispatchContext dispatch;
  std::string error;
  assert(zsoda::ae::BuildSdkDispatch(payload, &dispatch, &error));
  assert(dispatch.command.command == zsoda::ae::AeCommand::kRender);
  assert(dispatch.render_request.params_override.has_value());
  const auto& override = *dispatch.render_request.params_override;
  assert(override.model_id == "depth-anything-v3-large");
  assert(override.quality == 2);
  assert(override.preserve_ratio);
  assert(override.quality_boost_enabled);
  assert(override.quality_boost_level == 4);
}

void TestSdkSequenceResetupBootstrapsRouterParams() {
  constexpr int kParamCount = 20;

  std::array<PF_ParamDef, kParamCount> params{};
  std::array<PF_ParamDef*, kParamCount> param_ptrs{};
  for (int i = 0; i < kParamCount; ++i) {
    param_ptrs[static_cast<std::size_t>(i)] = &params[static_cast<std::size_t>(i)];
  }

  params[static_cast<int>(zsoda::ae::AeParamId::kQuality)].u.pd.value = 4;  // 1024 px
  params[static_cast<int>(zsoda::ae::AeParamId::kPreserveRatio)].u.bd.value = 0;
  params[static_cast<int>(zsoda::ae::AeParamId::kQualityBoostEnable)].u.bd.value = 1;
  params[static_cast<int>(zsoda::ae::AeParamId::kQualityBoostLevel)].u.pd.value = 4;  // 5x5
  params[static_cast<int>(zsoda::ae::AeParamId::kTimeConsistency)].u.bd.value = 1;
  params[static_cast<int>(zsoda::ae::AeParamId::kModel)].u.pd.value = 1;

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
  assert(dispatch.command.command == zsoda::ae::AeCommand::kUpdateParams);
  assert(dispatch.command.params_update == &dispatch.params_update);
  assert(dispatch.params_update.model_id == "depth-anything-v3-large");
  assert(dispatch.params_update.quality == 4);
  assert(!dispatch.params_update.preserve_ratio);
  assert(dispatch.params_update.quality_boost_enabled);
  assert(dispatch.params_update.quality_boost_level == 5);
  assert(dispatch.params_update.time_consistency);

  auto engine = std::make_shared<zsoda::inference::ManagedInferenceEngine>("models");
  assert(engine->Initialize("depth-anything-v3-small", &error));
  auto pipeline = std::make_shared<zsoda::core::RenderPipeline>(engine);
  zsoda::ae::AeCommandRouter router(pipeline, engine);

  zsoda::ae::AeCommandContext setup_context;
  setup_context.command = zsoda::ae::AeCommand::kGlobalSetup;
  setup_context.error = &error;
  assert(router.Handle(setup_context));

  dispatch.command.error = &error;
  assert(router.Handle(dispatch.command));
  const auto current = router.CurrentParams();
  assert(current.quality == 4);
  assert(!current.preserve_ratio);
  assert(current.quality_boost_enabled);
  assert(current.quality_boost_level == 5);
  assert(current.time_consistency);
}
#endif

void TestExecuteHostBufferRenderBridge() {
  ScopedEnvironmentOverride force_temporal_alpha("ZSODA_TEMPORAL_ALPHA", "1");

  auto engine = std::make_shared<zsoda::inference::ManagedInferenceEngine>("models");
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));
  auto pipeline = std::make_shared<zsoda::core::RenderPipeline>(engine);
  zsoda::ae::AeCommandRouter router(pipeline, engine);

  zsoda::ae::AeHostCommandContext setup_host;
  setup_host.command_id = 1;
  zsoda::ae::AeCommandContext setup_context;
  setup_context.command = zsoda::ae::AeCommand::kGlobalSetup;
  setup_context.host = &setup_host;
  setup_context.error = &error;
  assert(router.Handle(setup_context));

  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  constexpr int kRowBytes = kWidth * 4;
  std::vector<std::uint8_t> rgba_src(kRowBytes * kHeight, 0);
  std::vector<std::uint8_t> rgba_out_a(kRowBytes * kHeight, 0);
  std::vector<std::uint8_t> rgba_out_b(kRowBytes * kHeight, 0);

  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const int idx = y * kRowBytes + x * 4;
      rgba_src[idx + 0] = static_cast<std::uint8_t>(x * 40 + y * 5);
      rgba_src[idx + 1] = static_cast<std::uint8_t>(x * 20 + y * 15);
      rgba_src[idx + 2] = static_cast<std::uint8_t>(x * 10 + y * 25);
      rgba_src[idx + 3] = 255;
    }
  }

  zsoda::ae::AeHostRenderBridgePayload payload;
  payload.source.pixels = rgba_src.data();
  payload.source.width = kWidth;
  payload.source.height = kHeight;
  payload.source.row_bytes = kRowBytes;
  payload.source.format = zsoda::core::PixelFormat::kRGBA8;
  payload.destination.pixels = rgba_out_a.data();
  payload.destination.width = kWidth;
  payload.destination.height = kHeight;
  payload.destination.row_bytes = kRowBytes;
  payload.destination.format = zsoda::core::PixelFormat::kRGBA8;
  payload.frame_hash = 8301;

  zsoda::ae::AeHostRenderBridgeResult first_result;
  error.clear();
  assert(zsoda::ae::ExecuteHostBufferRenderBridge(&router, payload, &first_result, &error));
  assert(first_result.status == zsoda::core::RenderStatus::kInference);

  bool has_non_zero = false;
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const int idx = y * kRowBytes + x * 4;
      const auto r = rgba_out_a[idx + 0];
      const auto g = rgba_out_a[idx + 1];
      const auto b = rgba_out_a[idx + 2];
      const auto a = rgba_out_a[idx + 3];
      assert(r == g);
      assert(g == b);
      assert(a == 255);
      if (r > 0) {
        has_non_zero = true;
      }
    }
  }
  assert(has_non_zero);

  payload.destination.pixels = rgba_out_b.data();
  zsoda::ae::AeHostRenderBridgeResult second_result;
  error.clear();
  assert(zsoda::ae::ExecuteHostBufferRenderBridge(&router, payload, &second_result, &error));
  assert(second_result.status == zsoda::core::RenderStatus::kCacheHit);
  assert(second_result.message.find("cache hit") != std::string::npos);
}

void TestRouterPayloadValidation() {
  auto engine = std::make_shared<zsoda::inference::ManagedInferenceEngine>("models");
  std::string error;
  assert(engine->Initialize("depth-anything-v3-small", &error));
  auto pipeline = std::make_shared<zsoda::core::RenderPipeline>(engine);
  zsoda::ae::AeCommandRouter router(pipeline, engine);

  zsoda::ae::AeCommandContext update_context;
  update_context.command = zsoda::ae::AeCommand::kUpdateParams;
  update_context.error = &error;
  assert(!router.Handle(update_context));
  assert(error == "missing params update payload");

  zsoda::ae::AeCommandContext render_context;
  render_context.command = zsoda::ae::AeCommand::kRender;
  render_context.error = &error;
  error.clear();
  assert(!router.Handle(render_context));
  assert(error == "invalid render command arguments");

  zsoda::ae::AeHostCommandContext host;
  host.command_id = 3;
  render_context.host = &host;
  error.clear();
  // Bridge path accepts host-only render commands until AE SDK payload
  // extraction is wired.
  assert(router.Handle(render_context));
  assert(error.empty());

  zsoda::ae::AeCommandRouter null_pipeline_router(nullptr, engine);
  error.clear();
  assert(!null_pipeline_router.Handle(render_context));
  assert(error == "invalid render command arguments");
}

void TestPluginEntryValidation() {
  assert(ZSodaEffectMainStub(0) == 0);
  assert(ZSodaEffectMainStub(2) == 0);
  assert(ZSodaEffectMainStub(12345) == -1);
  assert(ZSodaSetModelIdStub(nullptr) == -1);

  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  constexpr int kPixels = kWidth * kHeight;

  float src[kPixels];
  float out[kPixels];
  for (int i = 0; i < kPixels; ++i) {
    src[i] = static_cast<float>(i) / static_cast<float>(kPixels);
    out[i] = 0.0F;
  }

  assert(ZSodaRenderGrayFrameStub(nullptr, kWidth, kHeight, 10, out, kPixels) == -1);
  assert(ZSodaRenderGrayFrameStub(src, kWidth, kHeight, 10, nullptr, kPixels) == -1);
  assert(ZSodaRenderGrayFrameStub(src, 0, kHeight, 10, out, kPixels) == -1);
  assert(ZSodaRenderGrayFrameStub(src, kWidth, -1, 10, out, kPixels) == -1);
  assert(ZSodaRenderGrayFrameStub(src, kWidth, kHeight, 10, out, kPixels - 1) == -1);

  std::vector<std::uint8_t> rgba_in(kWidth * kHeight * 4U, 0);
  std::vector<std::uint8_t> rgba_out(kWidth * kHeight * 4U, 0);
  assert(ZSodaRenderHostBufferStub(nullptr,
                                   kWidth,
                                   kHeight,
                                   kWidth * 4,
                                   static_cast<int>(zsoda::core::PixelFormat::kRGBA8),
                                   100,
                                   rgba_out.data(),
                                   kWidth * 4) == -1);
  assert(ZSodaRenderHostBufferStub(rgba_in.data(),
                                   kWidth,
                                   kHeight,
                                   kWidth * 4,
                                   999,
                                   100,
                                   rgba_out.data(),
                                   kWidth * 4) == -1);
}

void TestRenderGrayFrameStubHostBufferBridge() {
  assert(ZSodaEffectMainStub(1) == 0);
  assert(ZSodaSetModelIdStub("depth-anything-v3-large") == 0);

  constexpr int kWidth = 5;
  constexpr int kHeight = 4;
  constexpr int kPixels = kWidth * kHeight;

  std::vector<float> src(kPixels, 0.0F);
  std::vector<float> out(kPixels, -1.0F);
  for (int i = 0; i < kPixels; ++i) {
    src[i] = static_cast<float>(i % kWidth) / static_cast<float>(kWidth - 1);
  }

  assert(ZSodaRenderGrayFrameStub(src.data(), kWidth, kHeight, 7001, out.data(), kPixels) == 0);

  bool has_changed = false;
  for (float value : out) {
    if (value != -1.0F) {
      has_changed = true;
    }
    assert(value >= 0.0F);
    assert(value <= 1.0F);
  }
  assert(has_changed);

  std::vector<float> untouched(kPixels, -7.0F);
  assert(ZSodaRenderGrayFrameStub(src.data(), kWidth, kHeight, 7002, untouched.data(), kPixels - 1) ==
         -1);
  for (float value : untouched) {
    assert(value == -7.0F);
  }
}

void TestPluginEntrySetParamsStub() {
  assert(ZSodaEffectMainStub(1) == 0);
  assert(ZSodaSetParamsStub("depth-anything-v3-large",
                            2,
                            static_cast<int>(zsoda::ae::AeOutputMode::kDepthMap),
                            0,
                            0.2F,
                            0.8F,
                            0.1F,
                            1,
                            256,
                            16,
                            128) == 0);

  constexpr int kWidth = 8;
  constexpr int kHeight = 8;
  constexpr int kPixels = kWidth * kHeight;
  std::vector<float> src(kPixels, 0.0F);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      src[y * kWidth + x] = static_cast<float>(x + y) / static_cast<float>(kWidth + kHeight - 2);
    }
  }

  std::vector<float> depth_map(kPixels, 0.0F);
  std::vector<float> slicing(kPixels, 0.0F);
  assert(ZSodaRenderGrayFrameStub(src.data(), kWidth, kHeight, 8101, depth_map.data(), kPixels) == 0);

  assert(ZSodaSetParamsStub("depth-anything-v3-large",
                            2,
                            static_cast<int>(zsoda::ae::AeOutputMode::kSlicing),
                            0,
                            0.45F,
                            0.55F,
                            0.0F,
                            1,
                            256,
                            16,
                            128) == 0);
  assert(ZSodaRenderGrayFrameStub(src.data(), kWidth, kHeight, 8102, slicing.data(), kPixels) == 0);

  bool differs_from_depth = false;
  bool has_low_slice = false;
  for (int i = 0; i < kPixels; ++i) {
    if (depth_map[i] != slicing[i]) {
      differs_from_depth = true;
    }
    if (slicing[i] <= 0.01F) {
      has_low_slice = true;
    }
    assert(slicing[i] >= 0.0F);
    assert(slicing[i] <= 1.0F);
  }
  assert(differs_from_depth);
  assert(has_low_slice);

  assert(ZSodaSetParamsStub("unknown-model-id",
                            2,
                            static_cast<int>(zsoda::ae::AeOutputMode::kDepthMap),
                            0,
                            0.2F,
                            0.8F,
                            0.1F,
                            1,
                            256,
                            16,
                            128) == -1);
}

void TestPluginEntryBridgePath() {
  assert(ZSodaEffectMainStub(1) == 0);
  assert(ZSodaSetModelIdStub("depth-anything-v3-large") == 0);
  assert(ZSodaSetParamsStub("depth-anything-v3-large",
                            2,
                            static_cast<int>(zsoda::ae::AeOutputMode::kDepthMap),
                            0,
                            0.0F,
                            1.0F,
                            0.1F,
                            1,
                            512,
                            32,
                            0) == 0);

  constexpr int kWidth = 8;
  constexpr int kHeight = 8;
  constexpr int kPixels = kWidth * kHeight;

  float src[kPixels];
  float out[kPixels];
  for (int i = 0; i < kPixels; ++i) {
    src[i] = static_cast<float>(i) / static_cast<float>(kPixels);
    out[i] = 0.0F;
  }

  assert(ZSodaRenderGrayFrameStub(src, kWidth, kHeight, 1301, out, kPixels) == 0);

  bool has_non_zero = false;
  for (float value : out) {
    if (value > 0.0F) {
      has_non_zero = true;
      break;
    }
  }
  assert(has_non_zero);

  // Bridge-only render command path should be accepted until PF_Cmd payload
  // wiring is connected.
  assert(ZSodaEffectMainStub(3) == 0);
  assert(ZSodaSetModelIdStub("unknown-model-id") == -1);
}

void TestPluginEntryHostBufferBridgePath() {
  assert(ZSodaEffectMainStub(1) == 0);
  assert(ZSodaSetModelIdStub("depth-anything-v3-large") == 0);
  assert(ZSodaSetParamsStub("depth-anything-v3-large",
                            2,
                            static_cast<int>(zsoda::ae::AeOutputMode::kDepthMap),
                            0,
                            0.0F,
                            1.0F,
                            0.1F,
                            1,
                            512,
                            32,
                            0) == 0);

  constexpr int kWidth = 3;
  constexpr int kHeight = 2;
  constexpr int kRowBytes = kWidth * 4 + 4;
  std::vector<std::uint8_t> src(kRowBytes * kHeight, 0);
  std::vector<std::uint8_t> out(kRowBytes * kHeight, 17);

  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const int idx = y * kRowBytes + x * 4;
      src[idx + 0] = static_cast<std::uint8_t>(30 + x * 40 + y * 5);
      src[idx + 1] = static_cast<std::uint8_t>(20 + x * 20 + y * 10);
      src[idx + 2] = static_cast<std::uint8_t>(10 + x * 10 + y * 15);
      src[idx + 3] = 255;
    }
  }

  assert(ZSodaRenderHostBufferStub(src.data(),
                                   kWidth,
                                   kHeight,
                                   kRowBytes,
                                   static_cast<int>(zsoda::core::PixelFormat::kRGBA8),
                                   9201,
                                   out.data(),
                                   kRowBytes) == 0);

  bool has_non_zero = false;
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const int idx = y * kRowBytes + x * 4;
      const auto r = out[idx + 0];
      const auto g = out[idx + 1];
      const auto b = out[idx + 2];
      const auto a = out[idx + 3];
      assert(r == g);
      assert(g == b);
      assert(a == 255);
      if (r > 0) {
        has_non_zero = true;
      }
    }
  }
  assert(has_non_zero);
}

}  // namespace

void RunAeRouterTests() {
  TestStubCommandAndDispatchMapping();
  TestSafeFrameHashSeed();
  TestPixelFormatCandidatesFromRowBytes();
  TestPixelFormatInferenceFromStride();
  TestSelectHostRenderPixelFormat();
  TestParamSetupAndModelMenu();
  TestRenderUsesCurrentAndOverrideParams();
  TestRenderBridgeFrameHashCacheBehavior();
  TestHostBufferRenderDispatchConversion();
  TestHostBufferRenderDispatchValidation();
#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK
  TestSdkRenderDispatchReadsParamsWhenNumParamsHintIsInputOnly();
  TestSdkSequenceResetupBootstrapsRouterParams();
#endif
  TestExecuteHostBufferRenderBridge();
  TestRouterPayloadValidation();
  TestPluginEntryValidation();
  TestRenderGrayFrameStubHostBufferBridge();
  TestPluginEntrySetParamsStub();
  TestPluginEntryBridgePath();
  TestPluginEntryHostBufferBridgePath();
}
