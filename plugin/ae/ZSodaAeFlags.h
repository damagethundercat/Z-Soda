#pragma once

// Keep code-side out_flags/out_flags2 and PiPL metadata synchronized.
//
// Root-cause note:
// Previous revisions advertised PF_OutFlag_CUSTOM_UI and PF_OutFlag_I_DO_DIALOG
// via the raw literal 0x00008020, even though the plug-in does not implement
// PF_Cmd_EVENT/PF_Cmd_DO_DIALOG or any custom ECW widget handling. That pushed
// After Effects through UI-specific code paths during apply/control-panel
// bring-up and correlated with U.dll 0xc0000409 crashes. The shipping flags
// below now only advertise capabilities the effect actually implements.

// PiPL compilation on macOS goes through Rez, not the full effect C++ header
// surface. Keep literal fallbacks here so the advertised flags remain stable
// even when PF_OutFlag_* enums are not visible.
#if defined(PF_OutFlag_PIX_INDEPENDENT)
#define ZSODA_AE_GLOBAL_OUTFLAGS_BASE PF_OutFlag_PIX_INDEPENDENT
#else
#define ZSODA_AE_GLOBAL_OUTFLAGS_BASE 0x00000400
#endif

#if defined(PF_OutFlag_DEEP_COLOR_AWARE)
#define ZSODA_AE_GLOBAL_OUTFLAGS \
  (ZSODA_AE_GLOBAL_OUTFLAGS_BASE | PF_OutFlag_DEEP_COLOR_AWARE)
#else
#define ZSODA_AE_GLOBAL_OUTFLAGS (ZSODA_AE_GLOBAL_OUTFLAGS_BASE | 0x02000000)
#endif
#define ZSODA_AE_GLOBAL_OUTFLAGS_LITERAL 0x02000400

#if defined(PF_OutFlag2_SUPPORTS_THREADED_RENDERING)
#define ZSODA_AE_GLOBAL_OUTFLAGS2_BASE PF_OutFlag2_SUPPORTS_THREADED_RENDERING
#else
#define ZSODA_AE_GLOBAL_OUTFLAGS2_BASE 0x08000000
#endif

// Do not advertise FLOAT_COLOR_AWARE until the effect implements the SmartFX
// selectors (PF_Cmd_SMART_PRE_RENDER / PF_Cmd_SMART_RENDER) that AE expects
// for 32-bpc float rendering.
#define ZSODA_AE_GLOBAL_OUTFLAGS2 ZSODA_AE_GLOBAL_OUTFLAGS2_BASE
#define ZSODA_AE_GLOBAL_OUTFLAGS2_LITERAL 0x08000000
