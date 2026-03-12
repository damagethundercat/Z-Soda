# AE Build and Packaging Guide

This document records the current production build and package path for the
shipping `Z-Soda` After Effects effect.

## Current Product Assumptions

- Production model: `distill-any-depth-base`
- Default runtime path: local Python remote service
- Transport: binary localhost HTTP
- Public AE UI: 8 shipping controls
  - `Quality`
  - `Preserve Ratio`
  - `Output`
  - `Color Map`
  - `Slice Mode`
  - `Position (%)`
  - `Range (%)`
  - `Soft Border (%)`
- Primary host target: Windows + After Effects
- `Color Map` presets: `Gray`, `Turbo`, `Viridis`, `Inferno`, `Magma`

## Related Files

- [LOCAL_AGENT_HANDOFF.md](/Users/Yongkyu/code/Z-Soda/docs/build/LOCAL_AGENT_HANDOFF.md)
- [MAC_SILICON_HANDOFF.md](MAC_SILICON_HANDOFF.md)
- [AE_SMOKE_TEST.md](/Users/Yongkyu/code/Z-Soda/docs/build/AE_SMOKE_TEST.md)
- [ORT_RUNTIME_DEPLOY.md](/Users/Yongkyu/code/Z-Soda/docs/build/ORT_RUNTIME_DEPLOY.md)
- [ORT_RUNTIME_ISOLATION_PLAN.md](/Users/Yongkyu/code/Z-Soda/docs/build/ORT_RUNTIME_ISOLATION_PLAN.md)
- [build_aex.ps1](/Users/Yongkyu/code/Z-Soda/tools/build_aex.ps1)
- [package_plugin.ps1](/Users/Yongkyu/code/Z-Soda/tools/package_plugin.ps1)
- [package_plugin.sh](/Users/Yongkyu/code/Z-Soda/tools/package_plugin.sh)

## Windows Prerequisites

- Adobe After Effects SDK headers
- CMake 3.21+
- Visual Studio 2022
- A Python environment that can run
  [distill_any_depth_remote_service.py](/Users/Yongkyu/code/Z-Soda/tools/distill_any_depth_remote_service.py)

Example environment variables:

```powershell
$env:AE_SDK_ROOT = "C:\SDKs\AdobeAfterEffectsSDK"
$env:AE_HEADERS = "$env:AE_SDK_ROOT\Examples\Headers"
```

## Recommended Windows Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build_aex.ps1 `
  -AeSdkIncludeDir "$env:AE_HEADERS" `
  -BuildDir "build-win" `
  -Config Release `
  -CopyToMediaCore
```

Expected outputs:

- `build-win\plugin\Release\ZSoda.aex`
- `build-win\plugin\Release\ZSoda.pdb`
- `build-win\plugin\Release\ZSoda.map`
- `build-win\plugin\Release\zsoda_py\distill_any_depth_remote_service.py`

## Optional ORT Build

If you explicitly want to keep the local ONNX Runtime backend available in the
Windows build, pass the SDK paths and enable the API path:

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

## Packaging

To collect a redistributable package from an existing build:

```powershell
.\tools\package_plugin.ps1 -Platform windows -BuildDir build-win -OutputDir dist -IncludeManifest
```

Shell variant:

```bash
bash tools/package_plugin.sh --platform windows --build-dir build-win --output-dir dist --include-manifest
```

The Windows package should contain:

- `ZSoda.aex`
- `models.manifest` when requested
- `zsoda_py\distill_any_depth_remote_service.py`
- `zsoda_ort\*` only when the build was produced with an explicit ORT runtime

## Runtime Notes

- The plugin no longer depends on old DA3 service scripts for normal operation.
- The preferred production runtime path is the local Python service payload under
  `zsoda_py\`.
- `zsoda_ort\` is optional and only present when the build explicitly includes
  ONNX Runtime.
- `%TEMP%\ZSoda_AE_Runtime.log` is failure-focused by default.
- Optional verbose host/router tracing can be enabled with `ZSODA_AE_TRACE=1`.

## Smoke Checklist

1. Build succeeds without loader signature warnings.
2. `ZSoda.aex` is copied into MediaCore.
3. After Effects shows `Z-Soda` in the effect list.
4. New instances expose `Quality`, `Preserve Ratio`, `Output`, `Color Map`,
   `Slice Mode`, `Position (%)`, `Range (%)`, and `Soft Border (%)`.
5. `Quality` changes alter the render resolution/path as expected.
6. `Output`, `Color Map`, `Slice Mode`, `Position (%)`, `Range (%)`, and `Soft Border (%)`
   update the visible result immediately, including arrow-button adjustments.
7. Rendering runs through the DAD remote path without falling back to the dummy engine.

## Failure Checks

- Loader problem:
  - inspect `build-win\plugin\Release\ZSoda.loader_check.txt`
- Runtime problem:
  - if needed, launch AE with `ZSODA_AE_TRACE=1`
  - inspect `%TEMP%\ZSoda_AE_Runtime.log`
- Remote service problem:
  - inspect `%TEMP%\ZSoda_RemoteService.log`
