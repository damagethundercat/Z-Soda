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

- [LOCAL_AGENT_HANDOFF.md](LOCAL_AGENT_HANDOFF.md)
- [MAC_SILICON_HANDOFF.md](MAC_SILICON_HANDOFF.md)
- [RELEASE_ASSETS.md](RELEASE_ASSETS.md)
- [THIN_SETUP_DESIGN.md](THIN_SETUP_DESIGN.md)
- [AE_SMOKE_TEST.md](AE_SMOKE_TEST.md)
- [ORT_RUNTIME_DEPLOY.md](ORT_RUNTIME_DEPLOY.md)
- [ORT_RUNTIME_ISOLATION_PLAN.md](ORT_RUNTIME_ISOLATION_PLAN.md)
- [build_aex.ps1](../../tools/build_aex.ps1)
- [build_plugin_macos.sh](../../tools/build_plugin_macos.sh)
- [prepare_release_assets.py](../../tools/prepare_release_assets.py)
- [package_plugin.ps1](../../tools/package_plugin.ps1)
- [package_plugin.sh](../../tools/package_plugin.sh)

## Windows Prerequisites

- Adobe After Effects SDK headers
- CMake 3.21+
- Visual Studio 2022
- Packaging-time access to the runtime/model payloads that should be embedded
  into the shipped `.aex`

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
- optional runtime roots under `build-win\plugin\Release\zsoda_py`,
  `build-win\plugin\Release\zsoda_ort`, and `models\`

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
- `ZSoda-windows.zip` and `ZSoda-windows.zip.sha256` for redistribution
- When present at package time, `models\`, `zsoda_py\`, and `zsoda_ort\` are
  embedded into `ZSoda.aex` rather than staged as sidecar folders.
- On first load the plug-in extracts the embedded payload into
  `%LOCALAPPDATA%\ZSoda\PayloadCache\<sha256>\...`.
- For a true zero-install release, package with:

```powershell
.\tools\package_plugin.ps1 `
  -Platform windows `
  -BuildDir build-win `
  -OutputDir dist `
  -IncludeManifest `
  -PythonRuntimeDir "release-assets\python-win" `
  -ModelRepoDir "release-assets\models" `
  -RequireSelfContained
```

- Expected payload layout:
  - `release-assets\models\distill-any-depth-base\...`
  - `release-assets\python-win\python.exe`
- Those inputs are staged as:
  - `models\hf\distill-any-depth-base\...`
  - `zsoda_py\python\...`
- Canonical asset prep:

```bash
python3 tools/prepare_release_assets.py \
  --output-dir release-assets \
  --macos-python-runtime-dir /path/to/python-macos \
  --windows-python-runtime-dir /path/to/python-win \
  --model-repo-dir /path/to/model-repos \
  --hf-cache-dir /path/to/hf-cache \
  --clean
```

## Recommended macOS Build

```bash
bash tools/build_plugin_macos.sh \
  --ae-sdk-root "/path/to/AdobeAfterEffectsSDK" \
  --build-dir build-mac \
  --output-dir dist-mac \
  --python-runtime-dir release-assets/python-macos \
  --model-repo-dir release-assets/models \
  --require-self-contained
```

Expected packaged bundle:

- `dist-mac/ZSoda.plugin`
- `dist-mac/ZSoda-macos.zip`
- `dist-mac/ZSoda-macos.zip.sha256`
- `dist-mac/ZSoda.plugin/Contents/MacOS/ZSoda`
- `dist-mac/ZSoda.plugin/Contents/Resources/models/...` when requested
- `dist-mac/ZSoda.plugin/Contents/Resources/zsoda_py/distill_any_depth_remote_service.py`
- `dist-mac/ZSoda.plugin/Contents/Resources/zsoda_ort/...` when present in the build
- The release zip exposes a single `.plugin` bundle to the user.
- For a true zero-install release, supply:
  - `release-assets/models/<model_id>/...` local HF repo snapshots
  - `release-assets/python-macos/...` portable Python runtime
- If `release-assets/` exists, the packager auto-detects it and `build_plugin_macos.sh`
  can stay on the shorter `--require-self-contained` form.
- Those inputs are staged as:
  - `ZSoda.plugin/Contents/Resources/models/hf/<model_id>/...`
  - `ZSoda.plugin/Contents/Resources/zsoda_py/python/...`

## Runtime Notes

- The plugin no longer depends on old DA3 service scripts for normal operation.
- The preferred production runtime path is the local Python service payload under
  `zsoda_py\`.
- `zsoda_ort\` is optional and only present when the build explicitly includes
  ONNX Runtime.
- Windows release packaging now embeds runtime payload directories into the
  `.aex` itself so distribution can stay single-file from the user's point of view.
- The helper now prefers bundled local HF repos under `models/hf/<model_id>/`
  before falling back to remote Hugging Face repo names.
- `%TEMP%\ZSoda_AE_Runtime.log` is failure-focused by default.
- Optional verbose host/router tracing can be enabled with `ZSODA_AE_TRACE=1`.
- The current packaged path is still self-contained, but the planned next
  release direction is thin distribution plus first-run setup.
  See [THIN_SETUP_DESIGN.md](THIN_SETUP_DESIGN.md).

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
