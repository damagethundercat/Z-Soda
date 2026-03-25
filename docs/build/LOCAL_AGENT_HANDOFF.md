# Windows Release Handoff

This document describes the current Windows shipping target for Z-Soda.

## Shipping Contract

The intended user flow is now:

1. download `ZSoda-windows.zip`
2. unzip it
3. copy the single `Z-Soda/` folder into MediaCore
4. launch After Effects
5. use the effect immediately

The Windows package should not require a separate installer or first-run model
download for the current release path.

## Current Product Baseline

- Public effect name: `Z-Soda`
- Match name: `Z-Soda Depth Slice`
- Production model: `distill-any-depth-base`
- Windows target runtime: native ONNX Runtime sidecar
- Sidecar layout:
  - `Z-Soda/ZSoda.aex`
  - `Z-Soda/models/`
  - `Z-Soda/zsoda_ort/`
- Python remote service: explicit debug/fallback only
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

### Windows packaging/runtime

- Windows sidecar packaging now emits a top-level `Z-Soda/` folder.
- Runtime resolution prefers plugin-adjacent `models/` and `zsoda_ort/`.
- Packaging smoke now asserts:
  - `Z-Soda/ZSoda.aex` exists in the zip
  - `Z-Soda/models/...` exists
  - `Z-Soda/zsoda_ort/...` exists
  - old flat-root sidecar entries do not exist
- `distill-any-depth-base` ONNX export is now validated across multiple input
  shapes so fixed-output exports fail during packaging work instead of surfacing
  later in AE smoke.

### Current real package

The current manual-test package is:

- `C:\Users\ikidk\Documents\Code\01 Z-Soda\artifacts\14_ort-sidecar-dynamic-quality-manual-test\ZSoda-windows.zip`

That zip expands to:

- `Z-Soda/ZSoda.aex`
- `Z-Soda/models/distill-any-depth/distill_any_depth_base.onnx`
- `Z-Soda/zsoda_ort/onnxruntime.dll`
- `Z-Soda/zsoda_ort/onnxruntime_providers_shared.dll`
- `Z-Soda/zsoda_ort/DirectML.dll`

## Important Scripts

- [tools/build_aex.ps1](../../tools/build_aex.ps1)
- [tools/package_plugin.ps1](../../tools/package_plugin.ps1)
- [tools/package_plugin.sh](../../tools/package_plugin.sh)
- [tools/package_layout.py](../../tools/package_layout.py)
- [tools/prepare_package_stage.py](../../tools/prepare_package_stage.py)
- [tools/prepare_ort_sidecar_release.py](../../tools/prepare_ort_sidecar_release.py)
- [tools/export_depth_model_onnx.py](../../tools/export_depth_model_onnx.py)
- [tools/run_packaging_smoke.py](../../tools/run_packaging_smoke.py)
- [docs/build/README.md](README.md)
- [docs/build/AE_SMOKE_TEST.md](AE_SMOKE_TEST.md)

## Required Windows Deliverables

### 1. Build the ORT-enabled `.aex`

Example:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build_aex.ps1 `
  -AeSdkIncludeDir "$env:AE_HEADERS" `
  -OrtIncludeDir "<onnxruntime include dir>" `
  -OrtLibrary "<onnxruntime import lib or dll path>" `
  -OrtRuntimeDllPath "<onnxruntime.dll path>" `
  -EnableOrtApi `
  -OrtDirectLinkMode OFF `
  -BuildDir "build-origin-main-ae-ort-dml" `
  -Config Release
```

### 2. Prepare the native sidecar assets

Example:

```powershell
python .\tools\prepare_ort_sidecar_release.py `
  --onnx-model-path artifacts\ort-export-probe\distill_any_depth_base.from-script.onnx `
  --ort-runtime-dir artifacts\ort-sidecar-directml-assets\zsoda_ort `
  --output-dir artifacts\ort-sidecar-directml-assets-dynamic `
  --overwrite
```

### 3. Package the Windows zip

```powershell
.\tools\package_plugin.ps1 `
  -Platform windows `
  -PackageMode sidecar-ort `
  -BuildDir build-origin-main-ae-ort-dml `
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

## Runtime Expectations

- `Quality` changes must alter actual process resolution.
- `distill-any-depth-base` ONNX exports must remain dynamically shaped.
- Windows ORT sidecar is the preferred shipping path.
- Python remote service should not be treated as the default user-facing path.
- No dummy-engine success masking in release behavior.

## Windows Smoke Test Focus

### Install path

For release validation, test the real user path:

1. unzip `ZSoda-windows.zip`
2. copy the `Z-Soda/` folder into MediaCore
3. launch After Effects
4. apply `Z-Soda`
5. verify first use succeeds without setup/download requirements

### Expected behavior

- Plug-in loads as `Z-Soda`
- `Quality` visibly changes render resolution/path
- `Color Map` updates depth visualization immediately
- Slice controls update the matte/output immediately
- DirectML/ORT runtime loads from `Z-Soda/zsoda_ort`
- No fallback to dummy output on the happy path

## Logs And Failure Checks

- AE runtime log:
  - `%TEMP%\ZSoda_AE_Runtime.log`
- Loader diagnostics:
  - [tools/collect_ae_loader_diagnostics.ps1](../../tools/collect_ae_loader_diagnostics.ps1)

If Windows smoke fails, check:

1. whether AE loaded `MediaCore\Z-Soda\ZSoda.aex`
2. whether `Z-Soda\models\models.manifest` is present
3. whether `Z-Soda\zsoda_ort\onnxruntime.dll` is present
4. whether runtime logs show ORT provider selection instead of remote fallback
5. whether `tools/run_packaging_smoke.py` still passes

## Commit Hygiene

- Do not commit generated runtime/model artifacts.
- Do commit code, tests, docs, and tooling changes that define the shipping path.
- Keep `PLAN.md` and `PROGRESS.md` aligned with the current ORT sidecar direction.
