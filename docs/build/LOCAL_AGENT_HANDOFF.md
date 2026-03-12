# Local Windows Agent Handoff Guide

This document is the current Windows handoff note for local `.aex` work.
Historical DA3 research and legacy loader investigations are intentionally
omitted from this file.

## Repo Root

- `C:\Users\Yongkyu\code\Z-Soda`

## Current Production Baseline

- Public effect name: `Z-Soda`
- Match name: `Z-Soda Depth Slice`
- Production model: `distill-any-depth-base`
- Runtime path: local Python remote service
- Transport: binary localhost HTTP
- Public controls:
  - `Quality`
  - `Preserve Ratio`
  - `Output`
  - `Normalize`
  - `Absolute Depth`
  - `Minimum`
  - `Maximum`
  - `Soft Border`

## Important Scripts

- [tools/build_aex.ps1](/Users/Yongkyu/code/Z-Soda/tools/build_aex.ps1)
- [tools/package_plugin.ps1](/Users/Yongkyu/code/Z-Soda/tools/package_plugin.ps1)
- [tools/distill_any_depth_remote_service.py](/Users/Yongkyu/code/Z-Soda/tools/distill_any_depth_remote_service.py)
- [docs/build/AE_SMOKE_TEST.md](/Users/Yongkyu/code/Z-Soda/docs/build/AE_SMOKE_TEST.md)

## Build Setup

```powershell
$env:AE_SDK_ROOT = "C:\SDKs\AdobeAfterEffectsSDK"
$env:AE_HEADERS = "$env:AE_SDK_ROOT\Examples\Headers"
```

Sanity checks:

```powershell
Test-Path "$env:AE_HEADERS\AE_Effect.h"
```

## Build and Deploy

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build_aex.ps1 `
  -AeSdkIncludeDir "$env:AE_HEADERS" `
  -BuildDir "build-win" `
  -Config Release `
  -CopyToMediaCore
```

Outputs to verify:

- `build-win\plugin\Release\ZSoda.aex`
- `build-win\plugin\Release\ZSoda.pdb`
- `build-win\plugin\Release\ZSoda.map`
- `build-win\plugin\Release\zsoda_py\distill_any_depth_remote_service.py`
- `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\ZSoda.aex`

## Optional ORT Path

Keep this disabled for the normal DAD-only production build. If you need an
explicit ONNX Runtime backend build, provide the SDK paths and turn the API path
on:

```powershell
$env:ORT_INCLUDE = "C:\onnxruntime\include"
$env:ORT_LIB = "C:\onnxruntime\lib\onnxruntime.lib"

powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build_aex.ps1 `
  -AeSdkIncludeDir "$env:AE_HEADERS" `
  -OrtIncludeDir "$env:ORT_INCLUDE" `
  -OrtLibrary "$env:ORT_LIB" `
  -EnableOrtApi `
  -BuildDir "build-win" `
  -Config Release `
  -CopyToMediaCore
```

## Runtime Expectations

- The plugin should auto-start the DistillAnyDepth remote service when needed.
- The preferred GPU service path must resolve to a CUDA-capable Python when available.
- AE should not show the old Advanced UI.
- `zsoda_ort\` may be absent in the default production build.

## Smoke Test Focus

### New project
- Plugin loads as `ZSoda`
- Main UI exposes `Quality`, `Preserve Ratio`, `Output`, `Normalize`, `Absolute Depth`, `Minimum`, `Maximum`, and `Soft Border`
- Quality changes alter actual render resolution
- Slice settings alter the matte/output immediately

## Logs

- AE runtime:
  - `%TEMP%\ZSoda_AE_Runtime.log`
- Remote service:
  - `%TEMP%\ZSoda_RemoteService.log`

## Current Known Priorities

1. Keep the production path DAD-only.
2. Keep the visible AE UX limited to the 3 production controls.
3. Keep the remote service binary transport path healthy.
4. Keep shutdown/save/load behavior stable with the reduced sequence state.
