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

- [PLAN.md](PLAN.md): execution checklist
- [PROGRESS.md](PROGRESS.md): live work log in Korean
- [plugin/ae](plugin/ae): AE entry, params, host bridge
- [plugin/core](plugin/core): cache, render pipeline, postprocess
- [plugin/inference](plugin/inference): engine/runtime/backend glue
- [tools](tools): build/package/runtime helper scripts
- [tests](tests): unit/integration harnesses

## Runtime Notes

- Production model is fixed to `distill-any-depth-base`.
- The plugin prefers the remote backend for DistillAnyDepth and auto-starts
  [distill_any_depth_remote_service.py](tools/distill_any_depth_remote_service.py)
  when needed.
- The hot path uses binary HTTP on `127.0.0.1`, not `PPM + JSON float array`.
- If the remote path is unavailable, the plugin must fall back safely and never crash AE.

## Build

Windows plugin build:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build_aex.ps1
```

macOS plugin build/package:

```bash
bash tools/build_plugin_macos.sh --ae-sdk-root "/path/to/AdobeAfterEffectsSDK"
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

- Build/package helper: [tools/build_aex.ps1](tools/build_aex.ps1)
- Mac build/package helper: [tools/build_plugin_macos.sh](tools/build_plugin_macos.sh)
- Packaging helper: [tools/package_plugin.ps1](tools/package_plugin.ps1)
- Shell packaging helper: [tools/package_plugin.sh](tools/package_plugin.sh)
- Release asset prep helper: [tools/prepare_release_assets.py](tools/prepare_release_assets.py)
- Windows release packaging now targets a single-file `ZSoda.aex`.
  - `models/`, `zsoda_py/`, and optional `zsoda_ort/` are appended to the
    `.aex` as an embedded payload during packaging.
  - On first load, the plug-in extracts that payload into a per-user cache
    instead of expecting sidecar folders next to the `.aex`.
- macOS packages stage `models/`, `zsoda_py/`, and optional `zsoda_ort/`
  under `ZSoda.plugin/Contents/Resources/`, so the release zip still exposes a
  single `.plugin` bundle to the user.
- Self-contained release packaging now accepts:
  - `--python-runtime-dir <dir>`: stages a portable runtime under `zsoda_py/python`
  - `--model-repo-dir <dir>`: stages local HF repos from `<dir>/<model_id>/...`
    into `models/hf/<model_id>/...`
  - `--hf-cache-dir <dir>`: optionally stages a preseeded Hugging Face cache
    under `models/hf-cache/`
  - `--require-self-contained`: fails packaging unless both the bundled Python
    runtime and at least one local model repo are present
- If `release-assets/` exists, packaging helpers auto-detect:
  - `release-assets/python-macos`
  - `release-assets/python-win`
  - `release-assets/models`
  - `release-assets/hf-cache`
- Canonical layout details: [docs/build/RELEASE_ASSETS.md](docs/build/RELEASE_ASSETS.md)
- Packaging helpers now also emit `ZSoda-windows.zip` or `ZSoda-macos.zip`
  plus matching `.sha256` files for handoff/distribution.
- The repo still does not contain a bundled Python runtime or local production
  model weights. For true plug-and-play shipping, those assets must be supplied
  to the packaging step.

## Models

- Manifest: [models/models.manifest](models/models.manifest)
- Current production entries:
  - `distill-any-depth`
  - `distill-any-depth-base`
  - `distill-any-depth-large`

DistillAnyDepth runs through the remote service, so normal AE usage does not require
shipping local ONNX weights in the repo.
