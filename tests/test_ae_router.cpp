#include <cassert>
#include <cstdint>
#include <memory>
#include <string>

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
  params.model_id = "depth-anything-v3-large";
  zsoda::ae::AeCommandContext update_context;
  update_context.command = zsoda::ae::AeCommand::kUpdateParams;
  update_context.params_update = &params;
  update_context.error = &error;
  assert(router.Handle(update_context));
  assert(router.CurrentParams().model_id == "depth-anything-v3-large");

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
  params.model_id = "depth-anything-v3-large";
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
  assert(response.message.find("depth-anything-v3-large") != std::string::npos);

  zsoda::ae::RenderRequest override_request = request;
  override_request.frame_hash = 9091;
  auto override_params = router.CurrentParams();
  override_params.model_id = "depth-anything-v3-small";
  override_request.params_override = override_params;

  zsoda::ae::RenderResponse override_response;
  render_context.render_request = &override_request;
  render_context.render_response = &override_response;
  assert(router.Handle(render_context));
  assert(override_response.message.find("depth-anything-v3-small") != std::string::npos);
  assert(!response.message.empty());
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
}

void TestPluginEntryBridgePath() {
  assert(ZSodaEffectMainStub(1) == 0);
  assert(ZSodaSetModelIdStub("depth-anything-v3-large") == 0);

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

}  // namespace

void RunAeRouterTests() {
  TestStubCommandAndDispatchMapping();
  TestParamSetupAndModelMenu();
  TestRenderUsesCurrentAndOverrideParams();
  TestRouterPayloadValidation();
  TestPluginEntryValidation();
  TestPluginEntryBridgePath();
}
