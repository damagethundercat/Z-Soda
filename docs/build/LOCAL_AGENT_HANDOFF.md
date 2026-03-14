# Windows Release Handoff

This document is the current handoff note for the Windows agent that will
produce the shipping `.aex` package after the macOS bring-up work.

The intended end-user flow is fixed:

1. user downloads a zip
2. user sees `ZSoda.plugin` or `ZSoda.aex`
3. user copies that single plug-in file into the AE plug-in folder
4. user launches After Effects
5. depth inference works immediately

That means Windows release work must preserve the single-file `.aex` UX.

## Current Product Baseline

- Public effect name: `Z-Soda`
- Match name: `Z-Soda Depth Slice`
- Production model: `distill-any-depth-base`
- Runtime path: local Python remote service
- Transport: binary localhost HTTP
- Public controls:
  - `Quality`
  - `Preserve Ratio`
  - `Output`
  - `Color Map`
  - `Slice Mode`
  - `Position (%)`
  - `Range (%)`
  - `Soft Border (%)`

## What Is Already Done

### Cross-platform packaging/runtime

- Windows packaging is now single-file from the user's point of view.
  - `models/`, `zsoda_py/`, and optional `zsoda_ort/` are embedded into
    `ZSoda.aex` instead of being shipped as sidecar folders.
  - On first load the plug-in extracts those payloads into
    `%LOCALAPPDATA%\ZSoda\PayloadCache\<sha256>\...`.
- Embedded payload extraction/runtime discovery is implemented in:
  - [plugin/inference/EmbeddedPayload.cpp](../../plugin/inference/EmbeddedPayload.cpp)
  - [plugin/inference/EmbeddedPayload.h](../../plugin/inference/EmbeddedPayload.h)
  - [plugin/inference/EngineFactory.cpp](../../plugin/inference/EngineFactory.cpp)
  - [plugin/inference/RuntimePathResolver.cpp](../../plugin/inference/RuntimePathResolver.cpp)
  - [plugin/inference/RemoteInferenceBackend.cpp](../../plugin/inference/RemoteInferenceBackend.cpp)
- Packaging helpers support self-contained release inputs:
  - [tools/package_plugin.ps1](../../tools/package_plugin.ps1)
  - [tools/package_plugin.sh](../../tools/package_plugin.sh)
  - [tools/build_embedded_payload.py](../../tools/build_embedded_payload.py)
  - [tools/prepare_release_assets.py](../../tools/prepare_release_assets.py)
  - [tools/check_release_readiness.py](../../tools/check_release_readiness.py)

### macOS status

- The macOS `.plugin` path is already working end-to-end.
- A real self-contained mac bundle was built with:
  - bundled Python runtime
  - bundled local `distill-any-depth-base` HF snapshot
  - helper preload on `mps`
  - successful `/zsoda/depth` smoke response
- This is important because the release contract is now validated on one OS.

## What Is Still Missing For Windows

The repository packaging state after the mac work is effectively `5/8`:

- present:
  - mac build
  - mac package
  - `release-assets` manifest path/tooling
  - local model repo layout
  - mac portable runtime layout
- missing:
  - `release-assets/python-win`
  - `build-win/plugin/Release/ZSoda.aex`
  - `dist/ZSoda-windows.zip`

The Windows agent should treat those three items as the immediate blockers.

## Important Scripts

- [tools/build_aex.ps1](../../tools/build_aex.ps1)
- [tools/package_plugin.ps1](../../tools/package_plugin.ps1)
- [tools/distill_any_depth_remote_service.py](../../tools/distill_any_depth_remote_service.py)
- [tools/prepare_release_assets.py](../../tools/prepare_release_assets.py)
- [tools/check_release_readiness.py](../../tools/check_release_readiness.py)
- [docs/build/README.md](README.md)
- [docs/build/RELEASE_ASSETS.md](RELEASE_ASSETS.md)
- [docs/build/AE_SMOKE_TEST.md](AE_SMOKE_TEST.md)

## Required Windows Deliverables

### 1. Prepare a portable Windows runtime

Stage a relocatable Python runtime under:

- `release-assets/python-win/python.exe`
  or
- `release-assets/python-win/python/python.exe`

That runtime must include the packages needed by
[tools/distill_any_depth_remote_service.py](../../tools/distill_any_depth_remote_service.py):

- `torch`
- `transformers`
- `Pillow`
- `huggingface_hub`

The runtime must be shippable inside the embedded `.aex` payload. Do not rely
on a system Python installation.

### 2. Reuse the same model layout

The shipping model stays fixed to `distill-any-depth-base`.

Required layout:

```text
release-assets/
  models/
    distill-any-depth-base/
      config.json
      model.safetensors
      preprocessor_config.json
      ...
```

The helper already prefers bundled local HF repos under `models/hf/<model_id>/`
after packaging. Do not switch back to remote-only first-run download as the
primary release path.

### 3. Produce the real Release `.aex`

Example build flow:

```powershell
$env:AE_SDK_ROOT = "C:\SDKs\AdobeAfterEffectsSDK"
$env:AE_HEADERS = "$env:AE_SDK_ROOT\Examples\Headers"

powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build_aex.ps1 `
  -AeSdkIncludeDir "$env:AE_HEADERS" `
  -BuildDir "build-win" `
  -Config Release
```

Expected outputs:

- `build-win\plugin\Release\ZSoda.aex`
- `build-win\plugin\Release\ZSoda.pdb`
- `build-win\plugin\Release\ZSoda.map`

### 4. Produce the self-contained Windows zip

```powershell
.\tools\package_plugin.ps1 `
  -Platform windows `
  -BuildDir build-win `
  -OutputDir dist `
  -IncludeManifest `
  -RequireSelfContained
```

Expected outputs:

- `dist\ZSoda.aex`
- `dist\ZSoda-windows.zip`
- `dist\ZSoda-windows.zip.sha256`

The zip should expose a single `ZSoda.aex` file to the user.

## Runtime Expectations

- The plug-in should auto-start the bundled DistillAnyDepth helper when needed.
- The helper should prefer bundled Python/runtime before any system Python.
- The helper should prefer bundled local HF repo snapshots before remote model
  names.
- AE should not expose legacy controls such as `Normalize`, `Absolute Depth`,
  `Minimum`, `Maximum`, `Time Consistency`, or visible model selectors.
- `zsoda_ort\` may be absent in the default DAD-only production build.
- Detailed runtime tracing remains opt-in through `ZSODA_AE_TRACE=1`.

## Windows Smoke Test Focus

### Install path

For release validation, test the actual user path:

1. unzip `ZSoda-windows.zip`
2. copy `ZSoda.aex` only
3. place it into MediaCore or the AE plug-ins folder
4. launch After Effects
5. confirm immediate usable inference

### Expected behavior

- Plug-in loads as `Z-Soda`
- UI exposes:
  - `Quality`
  - `Preserve Ratio`
  - `Output`
  - `Color Map`
  - `Slice Mode`
  - `Position (%)`
  - `Range (%)`
  - `Soft Border (%)`
- `Color Map` updates the depth visualization immediately
  (`Gray`, `Turbo`, `Viridis`, `Inferno`, `Magma`)
- Slice settings update the matte/output immediately
- Slider arrow nudges do not crash AE
- The first inference request does not fall back to the dummy engine
- The bundled helper starts successfully from the extracted payload root

## Logs And Failure Checks

- AE runtime log:
  - `%TEMP%\ZSoda_AE_Runtime.log`
- Remote service log:
  - `%TEMP%\ZSoda_RemoteService.log`

If Windows falls back to the dummy engine, check:

1. whether the embedded payload extracted under `%LOCALAPPDATA%\ZSoda\PayloadCache\...`
2. whether the extracted Python runtime contains the required packages
3. whether `distill-any-depth-base` exists under extracted `models\hf\`
4. whether helper `/status` reports `loaded=true`

## Commit Hygiene

- Do not commit local generated payloads such as:
  - `release-assets/`
  - `dist-mac/`
  - `.cache/`
- Commit code, docs, and tooling only.

## Current Priority Order

1. Prepare `release-assets/python-win`
2. Build `build-win/plugin/Release/ZSoda.aex`
3. Package `dist/ZSoda-windows.zip`
4. Smoke test in After Effects on Windows
5. Report back the exact result and any runtime log excerpts
