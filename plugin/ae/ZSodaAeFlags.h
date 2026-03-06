#pragma once

// Keep code-side out_flags/out_flags2 and PiPL metadata synchronized.
// NOTE:
// Exclude PF_OutFlag_DISPLAY_ERROR_MESSAGE (0x00000100) from global flags.
// AE may normalize PiPL flags without this bit during loader validation,
// which leads to recurring "code 4008120 vs PiPL 4008020" mismatches.
#define ZSODA_AE_GLOBAL_OUTFLAGS 0x04008020

#define ZSODA_AE_GLOBAL_OUTFLAGS2_BASE 0x00000000

#if defined(PF_OutFlag2_SUPPORTS_THREADED_RENDERING)
#undef ZSODA_AE_GLOBAL_OUTFLAGS2_BASE
#define ZSODA_AE_GLOBAL_OUTFLAGS2_BASE PF_OutFlag2_SUPPORTS_THREADED_RENDERING
#endif

#if defined(PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG)
#define ZSODA_AE_GLOBAL_OUTFLAGS2 \
  (ZSODA_AE_GLOBAL_OUTFLAGS2_BASE | PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG)
#else
#define ZSODA_AE_GLOBAL_OUTFLAGS2 ZSODA_AE_GLOBAL_OUTFLAGS2_BASE
#endif

