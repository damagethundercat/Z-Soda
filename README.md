# Z-Soda

After Effects depth effect plugin focused on a single production path:

- model: `distill-any-depth-base`
- host: Adobe After Effects
- runtime: local Python remote service + binary localhost transport
- primary outputs: depth map / depth-driven compositing

## Current Product Shape

- AE UI exposes the shipping controls:
  - `Quality`
  - `Preserve Ratio`
  - `Output`
  - `Color Map`
  - `Slice Mode`
  - `Position (%)`
  - `Range (%)`
  - `Soft Border (%)`
- The plugin always runs the single production DAD path.
- `Color Map` affects `Depth Map` only and currently offers `Gray`, `Turbo`,
  `Viridis`, `Inferno`, and `Magma`.
- Depth slicing is part of the shipping UI and uses the internal slice-matte path.
- Quality boost, preview/final mode switches, and time-consistency toggles are no longer part of the shipping UI.
- The default display mapping for DistillAnyDepth is raw/linear grayscale.

## Repository Layout

- [PLAN.md](/Users/Yongkyu/code/Z-Soda/PLAN.md): execution checklist
- [PROGRESS.md](/Users/Yongkyu/code/Z-Soda/PROGRESS.md): live work log in Korean
- [plugin/ae](/Users/Yongkyu/code/Z-Soda/plugin/ae): AE entry, params, host bridge
- [plugin/core](/Users/Yongkyu/code/Z-Soda/plugin/core): cache, render pipeline, postprocess
- [plugin/inference](/Users/Yongkyu/code/Z-Soda/plugin/inference): engine/runtime/backend glue
- [tools](/Users/Yongkyu/code/Z-Soda/tools): build/package/runtime helper scripts
- [tests](/Users/Yongkyu/code/Z-Soda/tests): unit/integration harnesses

## Runtime Notes

- Production model is fixed to `distill-any-depth-base`.
- The plugin prefers the remote backend for DistillAnyDepth and auto-starts
  [distill_any_depth_remote_service.py](/Users/Yongkyu/code/Z-Soda/tools/distill_any_depth_remote_service.py)
  when needed.
- The hot path uses binary HTTP on `127.0.0.1`, not `PPM + JSON float array`.
- If the remote path is unavailable, the plugin must fall back safely and never crash AE.

## Build

Windows plugin build:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build_aex.ps1
```

Native CMake path:

```powershell
cmake -S . -B build-win -DZSODA_WITH_AE_SDK=ON
cmake --build build-win --config Release --target zsoda_aex
```

Debug/unit test path:

```powershell
cmake -S . -B build-cleanup
cmake --build build-cleanup --config Debug --target zsoda_tests
build-cleanup\tests\Debug\zsoda_tests.exe
```

## Packaging

- Build/package helper: [tools/build_aex.ps1](/Users/Yongkyu/code/Z-Soda/tools/build_aex.ps1)
- Packaging helper: [tools/package_plugin.ps1](/Users/Yongkyu/code/Z-Soda/tools/package_plugin.ps1)
- MediaCore deployment is handled from the Windows build helper.

## Models

- Manifest: [models/models.manifest](/Users/Yongkyu/code/Z-Soda/models/models.manifest)
- Current production entries:
  - `distill-any-depth`
  - `distill-any-depth-base`
  - `distill-any-depth-large`

DistillAnyDepth runs through the remote service, so normal AE usage does not require
shipping local ONNX weights in the repo.
