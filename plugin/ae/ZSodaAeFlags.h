#pragma once

// Keep code-side out_flags/out_flags2 and PiPL metadata synchronized.
#define ZSODA_AE_GLOBAL_OUTFLAGS 0x04008120

#if defined(PF_OutFlag2_SUPPORTS_THREADED_RENDERING)
#define ZSODA_AE_GLOBAL_OUTFLAGS2 PF_OutFlag2_SUPPORTS_THREADED_RENDERING
#else
#define ZSODA_AE_GLOBAL_OUTFLAGS2 0x00000000
#endif

