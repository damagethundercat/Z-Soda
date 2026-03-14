# Release Assets

This document defines the canonical `release-assets/` layout used by
self-contained `Z-Soda` packaging.

## Goal

The release target is:

1. user downloads a zip
2. user sees `ZSoda.plugin` or `ZSoda.aex`
3. user copies that plug-in file into the AE plug-in folder
4. user launches After Effects
5. depth inference works immediately

That means release packaging must include:

- a portable Python runtime
- the DistillAnyDepth helper script
- local Hugging Face model snapshots
- optional preseeded Hugging Face cache

## Canonical Layout

```text
release-assets/
  asset-manifest.json
  python-macos/
    ...
  python-win/
    ...
  models/
    distill-any-depth-base/
      config.json
      ...
  hf-cache/
    ...
```

The packaging helpers consume those directories as:

- macOS:
  - `release-assets/python-macos`
  - `release-assets/models`
  - `release-assets/hf-cache`
- Windows:
  - `release-assets/python-win`
  - `release-assets/models`
  - `release-assets/hf-cache`

## Staged Output Layout

macOS bundle:

```text
ZSoda.plugin/
  Contents/
    Resources/
      zsoda_py/
        distill_any_depth_remote_service.py
        python/
          ...
      models/
        models.manifest
        hf/
          distill-any-depth-base/
            config.json
            ...
        hf-cache/
          ...
```

Windows embedded payload:

```text
ZSoda.aex
  [embedded payload footer]

extracted at runtime to:
%LOCALAPPDATA%/ZSoda/PayloadCache/<sha256>/
  zsoda_py/
    distill_any_depth_remote_service.py
    python/
      ...
  models/
    models.manifest
    hf/
      distill-any-depth-base/
        config.json
        ...
    hf-cache/
      ...
```

## Preparing The Layout

Use [prepare_release_assets.py](../../tools/prepare_release_assets.py):

```bash
python3 tools/prepare_release_assets.py \
  --output-dir release-assets \
  --macos-python-runtime-dir /path/to/python-macos \
  --windows-python-runtime-dir /path/to/python-win \
  --model-repo-dir /path/to/model-repos \
  --hf-cache-dir /path/to/hf-cache \
  --clean
```

This writes `release-assets/asset-manifest.json` and makes the packaging helpers
auto-detect those inputs.

## Packaging With Auto-Detect

After `release-assets/` exists, the packagers pick it up automatically.

macOS:

```bash
bash tools/build_plugin_macos.sh \
  --ae-sdk-root "/path/to/AdobeAfterEffectsSDK" \
  --build-dir build-mac \
  --output-dir dist-mac \
  --require-self-contained
```

Windows:

```powershell
.\tools\package_plugin.ps1 `
  -Platform windows `
  -BuildDir build-win `
  -OutputDir dist `
  -IncludeManifest `
  -RequireSelfContained
```

## Validation Rules

`--require-self-contained` currently checks for:

- bundled helper script under `zsoda_py/`
- bundled Python entrypoint
  - macOS: `bin/python3` or `python/bin/python3`
  - Windows: `python.exe` or `python/python.exe`
- at least one local model repo under `models/hf/`

If those checks fail, packaging stops instead of emitting a misleading
non-shipping package.
