#include "AEConfig.h"
#include "AE_EffectVers.h"
#include "AE_Effect.h"
#include "ZSodaAeFlags.h"
#include "ZSodaVersion.h"

#ifndef AE_OS_WIN
  #include <AE_General.r>
#endif

resource 'PiPL' (16000) {
  {
    Kind {
      AEEffect
    },
    Name {
      "ZSoda"
    },
    Category {
      "Z-Soda"
    },
#ifdef AE_OS_WIN
    CodeWin64X86 {"EffectMain"},
#elif defined(AE_OS_MAC)
    CodeMacIntel64 {"EffectMain"},
    CodeMacARM64 {"EffectMain"},
#endif
    AE_PiPL_Version {
      2,
      0
    },
    AE_Effect_Spec_Version {
      // Pin to AE 25.0-compatible spec (13.28) to avoid loader rejection
      // when newer SDK headers provide a higher PF_PLUG_IN_SUBVERS.
      13,
      28
    },
    AE_Effect_Version {
      ZSODA_EFFECT_VERSION_HEX
    },
    AE_Effect_Info_Flags {
      0
    },
    AE_Effect_Global_OutFlags {
      ZSODA_AE_GLOBAL_OUTFLAGS
    },
    AE_Effect_Global_OutFlags_2 {
      ZSODA_AE_GLOBAL_OUTFLAGS2
    },
    AE_Effect_Match_Name {
      "ZSoda Depth"
    },
    AE_Reserved_Info {
      0
    },
    AE_Effect_Support_URL {
      "https://example.com/zsoda"
    }
  }
};
