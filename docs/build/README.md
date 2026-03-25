# AE Build and Packaging Guide

This document records the current production build and package path for the
shipping `Z-Soda` After Effects effect.

## Current Shipping Contract

- Production model: `distill-any-depth-base`
- Default runtime path: native ONNX Runtime sidecar
- Windows package shape:
  - `Z-Soda/ZSoda.aex`
  - `Z-Soda/models/`
  - `Z-Soda/zsoda_ort/`
- Windows install flow:
  1. unzip `ZSoda-windows.zip`
  2. copy the single `Z-Soda/` folder into MediaCore
  3. launch After Effects
  4. use the effect immediately
- Public AE UI: 8 shipping controls
  - `Quality`
  - `Preserve Ratio`
  - `Output`
  - `Color Map`
  - `Slice Mode`
  - `Position (%)`
  - `Range (%)`
  - `Soft Border (%)`

## Primary References

- [LOCAL_AGENT_HANDOFF.md](LOCAL_AGENT_HANDOFF.md)
- [MAC_AGENT_HANDOFF.md](MAC_AGENT_HANDOFF.md)
- [AE_SMOKE_TEST.md](AE_SMOKE_TEST.md)
- [ORT_RUNTIME_DEPLOY.md](ORT_RUNTIME_DEPLOY.md)
- [ORT_RUNTIME_ISOLATION_PLAN.md](ORT_RUNTIME_ISOLATION_PLAN.md)
- [build_aex.ps1](../../tools/build_aex.ps1)
- [build_plugin_macos.sh](../../tools/build_plugin_macos.sh)
- [package_plugin.ps1](../../tools/package_plugin.ps1)
- [package_plugin.sh](../../tools/package_plugin.sh)
- [prepare_ort_sidecar_release.py](../../tools/prepare_ort_sidecar_release.py)
- [run_packaging_smoke.py](../../tools/run_packaging_smoke.py)

## Historical References

These are kept only as legacy context. They are not the current release path.

- [MAC_SILICON_HANDOFF.md](MAC_SILICON_HANDOFF.md)
- [RELEASE_ASSETS.md](RELEASE_ASSETS.md)
- [THIN_SETUP_DESIGN.md](THIN_SETUP_DESIGN.md)

## Windows Prerequisites

- Adobe After Effects SDK headers
- CMake 3.21+
- Visual Studio 2022
- ONNX Runtime headers/import library
- ORT runtime DLL payload that will ship under `Z-Soda/zsoda_ort/`
- Exported ONNX model payload that will ship under `Z-Soda/models/`

Example environment variables:

```powershell
$env:AE_SDK_ROOT = "C:\SDKs\AdobeAfterEffectsSDK"
$env:AE_HEADERS = "$env:AE_SDK_ROOT\Examples\Headers"
$env:ORT_INCLUDE = "C:\onnxruntime\include"
$env:ORT_LIB = "C:\onnxruntime\lib\onnxruntime.lib"
$env:ORT_DLL = "C:\onnxruntime\bin\onnxruntime.dll"
```

## Recommended Windows Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build_aex.ps1 `
  -AeSdkIncludeDir "$env:AE_HEADERS" `
  -OrtIncludeDir "$env:ORT_INCLUDE" `
  -OrtLibrary "$env:ORT_LIB" `
  -OrtRuntimeDllPath "$env:ORT_DLL" `
  -EnableOrtApi `
  -RequireOrtRuntimeDll `
  -BuildDir "build-win" `
  -Config Release
```

Expected outputs:

- `build-win\plugin\Release\ZSoda.aex`
- `build-win\plugin\Release\ZSoda.pdb`
- `build-win\plugin\Release\ZSoda.map`
- `build-win\plugin\Release\zsoda_ort\...`

## Windows Packaging

Prepare the sidecar assets first:

```powershell
python .\tools\prepare_ort_sidecar_release.py `
  --onnx-model-path artifacts\ort-export-probe\distill_any_depth_base.from-script.onnx `
  --ort-runtime-dir artifacts\ort-sidecar-directml-assets\zsoda_ort `
  --output-dir artifacts\ort-sidecar-directml-assets-dynamic `
  --overwrite
```

Then package the Windows release zip:

```powershell
.\tools\package_plugin.ps1 `
  -Platform windows `
  -PackageMode sidecar-ort `
  -BuildDir build-win `
  -ModelRootDir artifacts\ort-sidecar-directml-assets-dynamic\models `
  -OrtRuntimeDllPath artifacts\ort-sidecar-directml-assets-dynamic\zsoda_ort\onnxruntime.dll `
  -OutputDir dist `
  -RequireSelfContained `
  -RequireOrtRuntimeDll
```

Expected outputs:

- `dist\Z-Soda\ZSoda.aex`
- `dist\Z-Soda\models\...`
- `dist\Z-Soda\zsoda_ort\...`
- `dist\ZSoda-windows.zip`
- `dist\ZSoda-windows.zip.sha256`

Shell variant:

```bash
bash tools/package_plugin.sh --platform windows --package-mode sidecar-ort --build-dir build-win --output-dir dist
```

## Recommended macOS Build

The current macOS direction is also ORT-first, but it must be completed on a
real macOS machine. Use [MAC_AGENT_HANDOFF.md](MAC_AGENT_HANDOFF.md) as the
handoff contract.

Starting point:

```bash
bash tools/build_plugin_macos.sh \
  --ae-sdk-root "/path/to/AdobeAfterEffectsSDK" \
  --build-dir build-mac \
  --output-dir dist-mac
```

Expected bundle shape:

- `ZSoda.plugin/Contents/MacOS/ZSoda`
- `ZSoda.plugin/Contents/Resources/models/...`
- `ZSoda.plugin/Contents/Resources/zsoda_ort/...`

## Runtime Notes

- The preferred user-facing path is native ORT from bundled sidecar assets.
- `Quality` must map to real input/output resolution changes.
- Python remote service remains available only for explicit debug/fallback work.
- Legacy embedded self-contained payloads are no longer the primary shipping path.
- `Z-Soda/ZSoda.aex`, `Z-Soda/models/`, and `Z-Soda/zsoda_ort/` must stay adjacent.
- `%TEMP%\ZSoda_AE_Runtime.log` is the primary failure-focused runtime log.
- Optional verbose host/router tracing can be enabled with `ZSODA_AE_TRACE=1`.

## Smoke Checklist

1. Build succeeds without loader signature warnings.
2. The `Z-Soda/` folder is copied into MediaCore.
3. After Effects shows `Z-Soda` in the effect list.
4. New instances expose `Quality`, `Preserve Ratio`, `Output`, `Color Map`,
   `Slice Mode`, `Position (%)`, `Range (%)`, and `Soft Border (%)`.
5. `Quality` changes alter the render resolution/path as expected.
6. `Output`, `Color Map`, `Slice Mode`, `Position (%)`, `Range (%)`, and `Soft Border (%)`
   update the visible result immediately, including arrow-button adjustments.
7. Rendering runs through the bundled ORT path without dummy-depth masking or
   an unexpected remote-primary fallback.

## Failure Checks

- Loader problem:
  - inspect `build-win\plugin\Release\ZSoda.loader_check.txt`
- Runtime problem:
  - inspect `%TEMP%\ZSoda_AE_Runtime.log`
- Sidecar runtime problem:
  - verify `MediaCore\Z-Soda\zsoda_ort\onnxruntime.dll`
  - verify `MediaCore\Z-Soda\models\distill-any-depth\distill_any_depth_base.onnx`
- Legacy remote-service problem:
  - inspect `%TEMP%\ZSoda_RemoteService.log` only when explicitly testing the remote fallback path
