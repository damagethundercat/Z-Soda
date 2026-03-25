# macOS ORT Handoff

This document is the handoff note for the macOS agent that will continue the
current ORT-first shipping direction after pulling the latest branch state.

## Current Direction

Windows has been moved to:

- ORT-first runtime policy
- native sidecar shipping assets
- dynamic `distill-any-depth-base` ONNX export
- no Python remote-service requirement on the happy path

Windows release-candidate status as of the latest handoff:

- package: `C:\Users\ikidk\Documents\Code\01 Z-Soda\artifacts\15_ort-sidecar-release-candidate\ZSoda-windows.zip`
- install shape: `MediaCore\Z-Soda\ZSoda.aex + models + zsoda_ort`
- user smoke status: pass
- practical outcome: Windows should now be treated as the validated reference lane
  for the shared ORT-first core

The macOS work should continue from that same product direction. Do not fork
the core design back to Python-first or self-contained embedded payloads unless
there is a hard blocker that is documented with evidence.

## What Is Already Shared

These parts are now intended to stay cross-platform:

- `distill-any-depth-base` as the default production model
- `Quality` mapping to real process resolution changes
- ORT-first engine selection in:
  - [plugin/inference/EngineFactory.cpp](../../plugin/inference/EngineFactory.cpp)
  - [plugin/inference/ManagedInferenceEngine.cpp](../../plugin/inference/ManagedInferenceEngine.cpp)
- dynamic ONNX export in:
  - [tools/export_depth_model_onnx.py](../../tools/export_depth_model_onnx.py)
- runtime path discovery in:
  - [plugin/inference/RuntimePathResolver.cpp](../../plugin/inference/RuntimePathResolver.cpp)

The macOS agent should assume those core parts are the baseline and only
change them when the macOS bring-up reveals a real cross-platform bug.

## What Is Platform-Specific On macOS

The main macOS-specific work should be limited to:

1. build + link the AE `.plugin` bundle
2. provide a macOS ORT runtime sidecar inside the bundle
3. select a working macOS GPU provider
4. package the final `.plugin` bundle and smoke-test it in AE

That means the expected macOS release shape is:

```text
ZSoda.plugin/
  Contents/
    MacOS/
      ZSoda
    Resources/
      models/
        distill-any-depth/
          distill_any_depth_base.onnx
      zsoda_ort/
        libonnxruntime.dylib
        ...
```

Unlike Windows, macOS does not need a separate top-level `Z-Soda/` folder
because the `.plugin` bundle is already the package boundary.

## Existing macOS Hooks In The Code

The repo already contains macOS-oriented pieces that the agent should reuse:

- bundle/resource resolution:
  - [plugin/inference/RuntimePathResolver.cpp](../../plugin/inference/RuntimePathResolver.cpp)
- macOS packaging wrapper:
  - [tools/package_plugin.sh](../../tools/package_plugin.sh)
- macOS build wrapper:
  - [tools/build_plugin_macos.sh](../../tools/build_plugin_macos.sh)
- CoreML execution-provider append path:
  - [plugin/inference/OnnxRuntimeBackend.cpp](../../plugin/inference/OnnxRuntimeBackend.cpp)

The first assumption to test is:

- ORT CPU works from inside the macOS bundle
- ORT CoreML can be enabled on top of that

If CoreML does not work, the fallback should be:

1. keep ORT CPU path working
2. document the exact CoreML blocker
3. only then evaluate whether the Python fallback is needed for macOS smoke

## Recommended macOS Work Order

1. Pull latest branch state.
2. Build the macOS plugin without changing the model/runtime contract.
3. Stage the exported ONNX model under the bundle `Contents/Resources/models`.
4. Stage the macOS ORT runtime under `Contents/Resources/zsoda_ort`.
5. Verify ORT CPU session creation first.
6. Verify whether ORT provider probing sees CoreML.
7. Enable CoreML if available and stable.
8. Package `ZSoda.plugin` and run AE smoke.

If the macOS agent needs a short starting instruction, use:

```text
git fetch origin
git checkout codex/self-contained-stabilization
git pull

Read docs/build/MAC_AGENT_HANDOFF.md first.
Keep the ORT-first design and the current model/runtime contract.
Do not reintroduce Python-first shipping or giant embedded payloads unless a
documented blocker makes ORT bring-up impossible.
```

## Do Not Re-solve These Problems

The following were already resolved on the Windows side and should not be
reopened casually during the macOS handoff:

- giant CUDA self-contained payloads
- first-run extraction of a huge embedded `.aex` payload
- dummy-engine fallback pretending to be success
- fixed-resolution ONNX export for `distill-any-depth-base`

If any of these appear on macOS, treat them as regressions to investigate, not
as open product questions.

## What The macOS Agent Should Validate

### Build/package

- `.plugin` loads in AE
- bundle contains `models/` and `zsoda_ort/` in `Contents/Resources`
- no missing dylib/runtime path issues

### Runtime

- ORT session opens against bundled `distill_any_depth_base.onnx`
- `Quality` changes alter actual process resolution
- provider selection is visible in logs
- no Python remote-service dependency on the happy path

### AE smoke

- effect loads as `Z-Soda`
- controls are unchanged
- `Depth Map` and `Depth Slice` both work
- `Quality` visibly changes output
- render queue output matches interactive preview

## Useful Files

- [docs/build/README.md](README.md)
- [docs/build/AE_SMOKE_TEST.md](AE_SMOKE_TEST.md)
- [docs/build/LOCAL_AGENT_HANDOFF.md](LOCAL_AGENT_HANDOFF.md)
- [tools/prepare_ort_sidecar_release.py](../../tools/prepare_ort_sidecar_release.py)
- [tools/run_packaging_smoke.py](../../tools/run_packaging_smoke.py)

## Suggested First Report Back

After the first macOS pass, report only these points:

1. Does the `.plugin` load?
2. Does ORT CPU work from bundled assets?
3. Does CoreML provider enumerate and run?
4. Does `Quality` visibly change output in AE?
5. What exact packaging/runtime blocker remains, if any?
