# Mac Apple Silicon Handoff

This document is the current handoff note for bringing `Z-Soda` up on
Apple Silicon macOS as a native After Effects plug-in.

It is a readiness and execution guide, not a claim that the macOS build is
already shipping.

## Scope

- Host target: Adobe After Effects on Apple Silicon macOS
- Primary architecture for bring-up: `arm64`
- Preferred install target:
  `/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/`
- Current product baseline:
  - fixed model: `distill-any-depth-base`
  - runtime: local Python remote service
  - public AE UI:
    - `Quality`
    - `Preserve Ratio`
    - `Output`
    - `Color Map`
    - `Slice Mode`
    - `Position (%)`
    - `Range (%)`
    - `Soft Border (%)`

## Official References

- Adobe: [Building plug-ins for Apple Silicon support](https://ae-plugins.docsforadobe.dev/intro/whats-new/#building-plugins-for-apple-silicon-support)
- Adobe: [Installing your plug-in](https://ae-plugins.docsforadobe.dev/intro/overview/#installing-your-plug-in)
- Adobe: [Effect entry points / exporting symbols on macOS](https://ae-plugins.docsforadobe.dev/effect-details/ex-entry-points/#exporting-symbols)
- Apple: [Building a universal macOS binary](https://developer.apple.com/documentation/apple-silicon/building-a-universal-macos-binary)
- Apple: [Notarizing macOS software before distribution](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution)
- ONNX Runtime: [CoreML Execution Provider](https://onnxruntime.ai/docs/execution-providers/CoreML-ExecutionProvider.html)
- PyTorch: [MPS backend](https://docs.pytorch.org/docs/stable/notes/mps.html)

Adobe's current guidance is the key baseline:
- native ARM plug-ins require Xcode 12.2 or later
- universal binaries are preferred when Intel Mac support still matters
- the plug-in must expose `EffectMain` correctly on macOS and install under
  the standard AE plug-in path

## Repo Snapshot

### Already Present

- [`plugin/CMakeLists.txt`](../../plugin/CMakeLists.txt) already defines an
  Apple bundle target named `zsoda_plugin_bundle`.
- [`plugin/ae/Info.plist.in`](../../plugin/ae/Info.plist.in) already exists.
- [`plugin/ae/ZSodaPiPL.r`](../../plugin/ae/ZSodaPiPL.r) already declares
  `CodeMacIntel64` and `CodeMacARM64`.
- [`tools/package_plugin.sh`](../../tools/package_plugin.sh) and
  [`tools/package_plugin.ps1`](../../tools/package_plugin.ps1) already know
  that the mac artifact name should be `ZSoda.plugin`.
- [`plugin/inference/RuntimePathResolver.cpp`](../../plugin/inference/RuntimePathResolver.cpp)
  already recognizes `libonnxruntime.dylib` as the mac runtime filename.

### Current Gaps / Blockers

- Apple CMake wiring is incomplete. The mac branch builds a `.plugin` bundle,
  but there is no Apple-side PiPL generation or resource embedding path
  comparable to the Windows `PiPLtool.exe` flow in
  [`plugin/CMakeLists.txt`](../../plugin/CMakeLists.txt).
- There is no mac build/deploy helper equivalent to
  [`tools/build_aex.ps1`](../../tools/build_aex.ps1). Bring-up currently
  depends on manual CMake/Xcode work.
- Packaging is incomplete for runtime payloads. The existing mac package path
  copies the bundle only; it does not stage `zsoda_py`, `models`, or a mac
  runtime `.dylib` payload.
- Non-Windows module directory discovery is missing. `TryResolveCurrentModuleDirectory()`
  returns `nullopt` on mac today, so plug-in-relative discovery of `models/`,
  `zsoda_py/`, and `zsoda_ort/` is not ready.
- The current remote service is CUDA/CPU-biased.
  [`tools/distill_any_depth_remote_service.py`](../../tools/distill_any_depth_remote_service.py)
  currently auto-selects `cuda` or `cpu`; it does not prefer or validate `mps`.
- Diagnostics are still Windows-focused. AE runtime trace and several
  inference diagnostics write only under `_WIN32`, so first mac bring-up will
  have weak observability unless this is expanded.
- CI does not exercise macOS at all.
  [`.github/workflows/ci.yml`](../../.github/workflows/ci.yml) currently runs
  only the Linux `g++` lane.

## Recommended Delivery Strategy

### Architecture Decision

- Phase 1 should target native `arm64` only.
- Phase 2 can add a universal `arm64;x86_64` binary if Intel Mac support is
  still required.
- Do not start with Rosetta as a shipping plan. Use it only as a diagnostic
  fallback if absolutely necessary.

Reason:
- the user target is Apple Silicon first
- Adobe explicitly supports native ARM plug-ins
- a universal build adds packaging, dependency, and signing complexity before
  the arm64 path is proven stable

### Inference Decision

Near term:
- keep the current remote-service production path
- make it Apple-native by using arm64 Python and explicit `mps` support

Medium term:
- evaluate replacing or complementing the Python helper with native
  ONNX Runtime + CoreML EP inside the plug-in or a small signed helper

Reason:
- the current repo is already organized around the DAD remote-service path
- shipping a full Python tree inside a mac plug-in will increase signing and
  notarization cost
- Apple Silicon performance will likely be materially better with a native
  CoreML-backed path if local inference remains a product requirement

## Environment Needed For Bring-Up

- Apple Silicon Mac
- Current Xcode toolchain, with Adobe's minimum of Xcode 12.2+ satisfied
- CMake 3.21+
- Adobe After Effects SDK headers
- Installed After Effects on the same machine for local smoke testing
- A decision on the runtime path:
  - bundled arm64 Python environment for the existing remote service
  - or a native ORT/CoreML runtime artifact

Suggested configure/build starting point:

```bash
cmake -S . -B build-mac -G Xcode \
  -DZSODA_WITH_AE_SDK=ON \
  -DAE_SDK_ROOT="/path/to/AdobeAfterEffectsSDK" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="<AE-supported-minimum>"

cmake --build build-mac --config Release --target zsoda_plugin_bundle
```

This is only a starting point. Expect first bring-up work in
[`plugin/CMakeLists.txt`](../../plugin/CMakeLists.txt) before the produced
bundle is actually loadable by AE.

## First Implementation Tasks

1. Add a real mac build lane.
   - Introduce a documented `build-mac` configure path.
   - Add a `macos-latest` or fixed macOS runner CI job for at least configure
     and compile coverage.
   - Lock `CMAKE_OSX_ARCHITECTURES=arm64` first.

2. Finish mac PiPL and symbol-export wiring.
   - Ensure the `.plugin` bundle embeds a valid PiPL resource for
     `CodeMacARM64`.
   - Verify `EffectMain` is exported and visible on mac.
   - Validate with `nm -gU` and bundle inspection before opening AE.

3. Implement plug-in-relative path resolution on mac.
   - Add a non-Windows implementation for
     `TryResolveCurrentModuleDirectory()`.
   - On mac this should resolve the bundle location reliably, typically via
     `dladdr`.
   - Decide whether runtime assets live under `Contents/Resources` or a sibling
     folder and update every resolver consistently.

4. Define a canonical mac bundle payload layout.
   - Example target layout:
     - `ZSoda.plugin/Contents/MacOS/ZSoda`
     - `ZSoda.plugin/Contents/Resources/models/...`
     - `ZSoda.plugin/Contents/Resources/zsoda_py/...`
     - `ZSoda.plugin/Contents/Resources/zsoda_ort/...`
   - Then update:
     - [`plugin/inference/EngineFactory.cpp`](../../plugin/inference/EngineFactory.cpp)
     - [`plugin/inference/RuntimePathResolver.cpp`](../../plugin/inference/RuntimePathResolver.cpp)
     - [`tools/package_plugin.sh`](../../tools/package_plugin.sh)

5. Make the remote helper Apple-Silicon aware.
   - Extend `resolve_device()` so `auto` can choose `mps` on supported systems.
   - Verify the packaged Python stack is fully arm64-native.
   - Do not ship an x86_64-only Python or wheel set and rely on Rosetta.

6. Add mac deployment and verification helpers.
   - Build helper equivalent to `tools/build_aex.ps1`
   - Deploy helper to copy the bundle into MediaCore
   - Verification helper for `codesign`, `plutil`, `otool`, `nm`, and
     `lipo`

## Performance Guidance

### Bring-Up Priorities

- Keep the current cache/session reuse rules intact.
- Keep model load and file I/O out of the per-frame AE render path.
- Make sure the remote helper is persistent; never launch a Python process per
  frame.
- Preserve the current binary localhost transport and avoid debug-heavy logging
  in hot render paths.

### Apple Silicon Specific

- Prefer a native `arm64` stack end to end:
  - After Effects host
  - plug-in bundle
  - helper process, if used
  - Python interpreter and wheels, if used
- Add `mps` support to the current helper before making performance claims.
- If ORT is evaluated on mac, test CoreML EP first and keep input shapes as
  stable as possible. CoreML partitioning and compile behavior are more
  predictable when the model input surface is not arbitrarily dynamic.
- If the remote Python path remains the shipping plan, benchmark at least:
  - first frame latency
  - warm steady-state latency
  - memory usage under long previews
  - thermal throttling behavior on fanless or low-power Apple Silicon devices

### Practical Optimizations To Expect

- Prewarm the remote service outside the first interactive render if possible.
- Bucket input sizes by the existing `Quality` presets instead of allowing too
  many unique resolutions.
- Avoid unnecessary CPU round-trips after inference.
- Keep false-color mapping and slice postprocess on the cheapest side of the
  pipeline if the depth tensor is already on CPU.

## Packaging, Signing, And Notarization

- The deliverable on mac is a signed `ZSoda.plugin` bundle, not a `.aex`.
- If nested helpers, Python binaries, or `.dylib` files are shipped inside the
  bundle, sign them explicitly before signing the outer bundle.
- Plan for notarization before calling the mac build "release ready".
- Verify both local debug deployment and distributable package signing.

Useful checks:

```bash
plutil -p "ZSoda.plugin/Contents/Info.plist"
nm -gU "ZSoda.plugin/Contents/MacOS/ZSoda" | grep EffectMain
otool -L "ZSoda.plugin/Contents/MacOS/ZSoda"
codesign --verify --deep --strict "ZSoda.plugin"
```

If a universal build is added later:

```bash
lipo -info "ZSoda.plugin/Contents/MacOS/ZSoda"
```

## Deployment And Smoke Test

Primary install path:

```bash
/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/
```

Package/deploy starting point:

```bash
bash tools/package_plugin.sh --platform macos --build-dir build-mac --output-dir dist --include-manifest
cp -R dist/ZSoda.plugin "/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/"
```

Important:
- the current package helper does not yet stage mac runtime payloads
- treat the command above as a bundle-copy baseline, not a complete shipping
  deploy

Smoke checklist after bring-up:

1. AE loads `Z-Soda` from the effect list.
2. The instance shows the current 8 shipping controls.
3. `Depth Map` and `Depth Slice` both render without host crashes.
4. `Color Map` changes update the visualization immediately.
5. Slice slider arrow nudges remain crash-free.
6. The runtime stays on the intended arm64-native path and does not silently
   fall back to a mismatched helper environment.

## Open Decisions For The Next Owner

1. Is the first mac release `arm64` only, or must it be universal immediately?
2. Will mac keep the Python remote helper, or should local inference move to a
   native ORT/CoreML path?
3. If Python stays, do we bundle a full runtime or replace it with a smaller
   signed helper executable?
4. Where should runtime assets live inside the `.plugin` bundle:
   `Contents/Resources`, a sibling directory, or another Adobe-compatible
   layout?
5. What is the minimum supported macOS version for the first Apple Silicon
   release, based on the current AE host support matrix?
