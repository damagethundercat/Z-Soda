#include "AEConfig.h"
#include "AE_EffectVers.h"
#include "AE_Effect.h"
#include "ZSodaAeFlags.h"
#include "ZSodaVersion.h"

#ifndef AE_OS_WIN
  #include <AE_General.r>
#endif

resource 'PiPL' (16001) {
  {
    Kind {
      AEEffect
    },
    Name {
      "ZSoda Loader Probe"
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
      // Keep probe on the same host-compat spec as the main plugin.
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
      ZSODA_AE_GLOBAL_OUTFLAGS_LITERAL
    },
    AE_Effect_Global_OutFlags_2 {
      ZSODA_AE_GLOBAL_OUTFLAGS2_LITERAL
    },
    AE_Effect_Match_Name {
      "ZSoda Loader Probe"
    },
    AE_Reserved_Info {
      0
    }
  }
};
