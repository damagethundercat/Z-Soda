#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(ZSODA_WITH_AE_SDK) && ZSODA_WITH_AE_SDK

#include "AEConfig.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectVers.h"
#include "AE_Macros.h"
#include "ae/ZSodaAeFlags.h"
#include "ae/ZSodaVersion.h"

#ifndef DllExport
#if defined(_WIN32)
#define DllExport __declspec(dllexport)
#else
#define DllExport
#endif
#endif

namespace {

PF_Err DoAbout(PF_OutData* out_data) {
  if (out_data == nullptr) {
    return PF_Err_INTERNAL_STRUCT_DAMAGED;
  }
  std::snprintf(out_data->return_msg,
                sizeof(out_data->return_msg),
                "ZSoda Loader Probe v%d.%d",
                1,
                0);
  return PF_Err_NONE;
}

PF_Err DoGlobalSetup(PF_OutData* out_data) {
  if (out_data == nullptr) {
    return PF_Err_INTERNAL_STRUCT_DAMAGED;
  }

  out_data->my_version = static_cast<A_u_long>(ZSODA_EFFECT_VERSION_HEX);
  out_data->out_flags = static_cast<A_long>(ZSODA_AE_GLOBAL_OUTFLAGS);
  out_data->out_flags2 = static_cast<A_long>(ZSODA_AE_GLOBAL_OUTFLAGS2);
  return PF_Err_NONE;
}

PF_Err DoParamsSetup(PF_OutData* out_data) {
  if (out_data == nullptr) {
    return PF_Err_INTERNAL_STRUCT_DAMAGED;
  }
  // Input layer only.
  out_data->num_params = 1;
  return PF_Err_NONE;
}

PF_Err DoRender(PF_ParamDef* params[], PF_LayerDef* output) {
  if (params == nullptr || params[0] == nullptr || output == nullptr || output->data == nullptr) {
    return PF_Err_NONE;
  }

  const PF_LayerDef* input = &params[0]->u.ld;
  if (input->data == nullptr) {
    return PF_Err_NONE;
  }

  const A_long rows = output->height;
  const A_long bytes_to_copy = (output->rowbytes < input->rowbytes) ? output->rowbytes : input->rowbytes;
  if (rows <= 0 || bytes_to_copy <= 0) {
    return PF_Err_NONE;
  }

  auto* dst = reinterpret_cast<std::uint8_t*>(output->data);
  const auto* src = reinterpret_cast<const std::uint8_t*>(input->data);
  for (A_long y = 0; y < rows; ++y) {
    std::memcpy(dst, src, static_cast<std::size_t>(bytes_to_copy));
    dst += output->rowbytes;
    src += input->rowbytes;
  }
  return PF_Err_NONE;
}

}  // namespace

extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd,
                                       PF_InData* in_data,
                                       PF_OutData* out_data,
                                       PF_ParamDef* params[],
                                       PF_LayerDef* output,
                                       void* extra) {
  (void)in_data;
  (void)extra;

  switch (cmd) {
    case PF_Cmd_ABOUT:
      return DoAbout(out_data);
    case PF_Cmd_GLOBAL_SETUP:
      return DoGlobalSetup(out_data);
    case PF_Cmd_PARAMS_SETUP:
      return DoParamsSetup(out_data);
    case PF_Cmd_RENDER:
      return DoRender(params, output);
    default:
      return PF_Err_NONE;
  }
}

#endif
