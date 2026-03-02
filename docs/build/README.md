# AE Final-Mile Packaging Guide (.aex/.plugin)

This document records the packaging path from CMake build output to After Effects installable artifacts.

## Scope

- Target artifacts:
  - Windows: `.aex`
  - macOS: `.plugin` bundle
- Current branch status:
  - `cmake` always builds `zsoda_plugin` (static library).
  - with `ZSODA_WITH_AE_SDK=ON` on native host:
    - Windows target `zsoda_aex` is generated (`ZSoda.aex`)
    - macOS target `zsoda_plugin_bundle` is generated (`ZSoda.dylib` module target; bundle metadata wiring remains pending)
  - Commands below include both:
    - what you can run now (scaffold verification)
    - final-mile packaging steps to apply when AE SDK-linked targets are added

## Prerequisites

- Adobe After Effects SDK extracted on local machine
  - Windows example: `C:\SDKs\AdobeAfterEffectsSDK`
  - macOS example: `$HOME/SDKs/AdobeAfterEffectsSDK`
  - Expected inside SDK root: headers + examples/skeleton directories from Adobe SDK package
- CMake `>= 3.21`
- Compiler toolchain:
  - Windows: Visual Studio 2022 (MSVC v143, x64)
  - macOS: Xcode (AppleClang)
- Optional model package root for runtime testing: `ZSODA_MODEL_ROOT`

## CMake Options (Packaging-Oriented)

- `-DZSODA_BUILD_TESTS=OFF`
  - keep packaging build minimal
- `-DCMAKE_BUILD_TYPE=Release` (single-config generators) or `--config Release` (multi-config)
- `-DAE_SDK_ROOT=<absolute-path>`
  - canonical SDK root path flag
  - if `AE_SDK_INCLUDE_DIR` is not set, build tries:
    - `${AE_SDK_ROOT}/Examples/Headers`
    - `${AE_SDK_ROOT}/Headers`
- `-DAE_SDK_INCLUDE_DIR=<absolute-path>`
  - directory that directly contains `AE_Effect.h`
- `-DZSODA_WITH_AE_SDK=ON`
  - enables conditional `EffectMain` entrypoint scaffold
- `-DZSODA_WITH_ONNX_RUNTIME=ON`
  - enables ONNX backend module in plugin build
- `-DZSODA_WITH_ONNX_RUNTIME_API=ON`
  - enables real ONNX Runtime C++ API execution path (default is scaffold/off)
  - requires:
    - `-DONNXRUNTIME_INCLUDE_DIR=<absolute-path-containing-onnxruntime_cxx_api.h>`
    - `-DONNXRUNTIME_LIBRARY=<absolute-path-to-onnxruntime-binary>`
  - if any required path is missing, configure step stops with `FATAL_ERROR`

## Windows Commands (PowerShell)

```powershell
$env:AE_SDK_ROOT = "C:\SDKs\AdobeAfterEffectsSDK"

cmake -S . -B build-win -G "Visual Studio 17 2022" -A x64 `
  -DZSODA_BUILD_TESTS=OFF `
  -DZSODA_WITH_AE_SDK=ON `
  -DZSODA_WITH_ONNX_RUNTIME=ON `
  -DZSODA_WITH_ONNX_RUNTIME_API=ON `
  -DONNXRUNTIME_INCLUDE_DIR="C:\onnxruntime\include" `
  -DONNXRUNTIME_LIBRARY="C:\onnxruntime\lib\onnxruntime.lib" `
  -DCMAKE_BUILD_TYPE=Release `
  -DAE_SDK_ROOT="$env:AE_SDK_ROOT"

cmake --build build-win --config Release --target zsoda_plugin
cmake --build build-win --config Release --target zsoda_aex
```

Current expected artifacts:
- `build-win/plugin/Release/zsoda_plugin.lib`
- `build-win/plugin/Release/ZSoda.aex` (`zsoda_aex` target)

Final-mile packaging path (after AE SDK target wiring lands):

```powershell
# Example target name for final packaging stage
cmake --build build-win --config Release --target zsoda_aex

# Example deploy path used by Adobe shared plug-ins
Copy-Item "build-win/plugin/Release/ZSoda.aex" `
  "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\ZSoda.aex"
```

## macOS Commands (zsh/bash)

```bash
export AE_SDK_ROOT="$HOME/SDKs/AdobeAfterEffectsSDK"

cmake -S . -B build-mac -G Xcode \
  -DZSODA_BUILD_TESTS=OFF \
  -DZSODA_WITH_AE_SDK=ON \
  -DZSODA_WITH_ONNX_RUNTIME=ON \
  -DZSODA_WITH_ONNX_RUNTIME_API=ON \
  -DONNXRUNTIME_INCLUDE_DIR="$HOME/onnxruntime/include" \
  -DONNXRUNTIME_LIBRARY="$HOME/onnxruntime/lib/libonnxruntime.dylib" \
  -DCMAKE_BUILD_TYPE=Release \
  -DAE_SDK_ROOT="$AE_SDK_ROOT"

cmake --build build-mac --config Release --target zsoda_plugin
cmake --build build-mac --config Release --target zsoda_plugin_bundle
```

Current expected artifacts:
- `build-mac/plugin/Release/libzsoda_plugin.a` (or generator-specific static library output)
- `build-mac/plugin/Release/ZSoda.dylib` (`zsoda_plugin_bundle` target; `.plugin` bundle packaging is still pending)

Final-mile packaging path (after AE SDK target wiring lands):

```bash
# Example target name for final packaging stage
cmake --build build-mac --config Release --target zsoda_plugin_bundle

# Example deploy path used by Adobe shared plug-ins
cp -R "build-mac/plugin/Release/ZSoda.plugin" \
  "/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/"
```

## Expected Artifact Checklist

When packaging integration is complete, verify:

- Windows:
  - `ZSoda.aex`
  - any required runtime sidecars (if backend-specific runtime DLLs are needed)
- macOS:
  - `ZSoda.plugin/Contents/MacOS/ZSoda`
  - `Info.plist`, code-signing state (if distribution policy requires signing)
- Models:
  - model weights are delivered separately (not committed into git)

## Current Environment Limitations (as of 2026-03-02)

- Working environment is Linux/WSL2, not native Windows/macOS.
- `cmake` is not installed in this environment.
- `xcodebuild`/MSBuild are unavailable here.
- Adobe AE SDK is not present in this workspace.
- Therefore `.aex/.plugin` end-to-end packaging commands are documented but not executable in the current environment until prerequisites are installed on target OS hosts.
