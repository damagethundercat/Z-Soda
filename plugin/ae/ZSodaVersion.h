#pragma once

// Keep code-side `my_version` and PiPL `AE_Effect_Version` in sync.
// Bump the public identity when we intentionally break old AE effect-control
// streams so the host does not attempt legacy parameter conversion.
#define ZSODA_EFFECT_VERSION_HEX 0x00090000
#define ZSODA_EFFECT_DISPLAY_NAME "Z-Soda"
#define ZSODA_EFFECT_MATCH_NAME "Z-Soda Depth Slice"

