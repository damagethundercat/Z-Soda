# PLAN.md - ZSoda Execution Plan

This file keeps the current production-facing implementation plan. Historical
research and experiment notes belong in `docs/research/` and should not drive
the shipping path directly.

## Product Baseline

- Host: Adobe After Effects
- Production model: `distill-any-depth-base`
- Windows target runtime: native ONNX Runtime sidecar (`models/` + `zsoda_ort/`)
- Python remote service: explicit dev/fallback path only
- Public UI: production controls only
  - `Quality`
  - `Preserve Ratio`
  - `Output`
  - `Color Map`
  - `Slice Mode`
  - `Position (%)`
  - `Range (%)`
  - `Soft Border (%)`

## Repo Layout

- `/plugin/ae`
  - AE entry, parameter registration, host bridge
- `/plugin/core`
  - frame conversion, cache, render pipeline, postprocess
- `/plugin/inference`
  - managed engine, remote backend, runtime path and lifecycle
- `/models`
  - manifest and lightweight model metadata only
- `/tools`
  - build, package, diagnostics, remote service
- `/docs`
  - build, runbook, smoke-test, and product docs

## Production Requirements

### Render behavior
- Cache hit returns immediately.
- Cache miss runs inference once and stores the result.
- Backend or memory failure falls back safely.
- Hard failure returns safe output or pass-through without crashing AE.

### Runtime behavior
- No model load in the per-frame hot path.
- Reuse inference sessions.
- Keep the default path fixed to `distill-any-depth-base`.
- Never require legacy DA3 tooling or cloud services for normal operation.
- The normal shipping path should resolve bundled ORT sidecar assets without
  first-run extraction or download.
- Keep remote-service fallback available for explicit/debug use, not as the
  primary production path.
- Release builds must not present dummy depth output as a successful inference result.

### AE UX behavior
- Single-pass DAD-base remains the only shipping render path.
- Depth slicing is part of the shipping path and should stay simple and direct.
- Removed Advanced controls must stay removed from the visible UI.

## Current Workstreams

### `RF-01` Repo cleanup
- Remove experiment-only compare GUI and runner tooling.
- Remove generated package artifacts from versioned source paths.
- Keep `StillQualityHarness` as optional internal diagnostics only.
- Keep docs and scripts aligned with the current DAD-only production path.
- Drive packaging stage layout from one shared tools-side spec instead of
  duplicating root selection and staging rules across PowerShell and shell packagers.

### `RF-02` AE layer cleanup
- Keep only the visible production controls.
- Move AE state handling toward per-sequence and per-instance storage.
- Avoid relying on one global shared parameter snapshot for real AE renders.

### `RF-03` Runtime and core cleanup
- Keep remote transport binary-first.
- Treat legacy JSON and file transport as debug-only fallback.
- Move Python runtime discovery and autostart selection out of
  `RemoteInferenceBackend.cpp` into smaller dedicated helpers.
- Move detached remote-service launch, `--port-file` handshake, and readiness
  polling out of `RemoteInferenceBackend.cpp` into a dedicated helper so the
  backend keeps only endpoint/state policy.
- Add focused tests for extracted runtime/autostart helpers instead of relying
  only on monolithic inference regressions.
- Remove always-off dummy-depth fallback from the release path and let
  `RenderPipeline` safe output handle hard failures directly.
- Keep unit tests split by domain and force Release test targets to preserve
  assertions so cleanup regressions fail explicitly instead of continuing into
  undefined behavior.
- Keep Windows/macOS packagers on the same staged-root contract by preparing
  `.payload-stage` through one shared Python helper before bundle/copy packaging.

### `RF-04` Slicing UX
- Keep the shipping baseline centered on DAD-base.
- Expose depth-map display as `Output` plus `Color Map`.
- Expose slicing as `Output`, `Slice Mode`, `Position (%)`, `Range (%)`, and `Soft Border (%)`.
- Do not reintroduce UI-facing quality boost or temporal toggles in the shipping path.

### `RF-05` ORT sidecar shipping path
- Make Windows prefer native ORT sidecar assets when `models/*.onnx` and
  `zsoda_ort/onnxruntime.dll` are present.
- Treat remote inference as explicit-only or dev/fallback-only. Do not infer a
  remote-primary shipping mode from `distill-any-depth*` model ids.
- Package Windows native GPU releases as a single `Z-Soda/` folder containing
  `ZSoda.aex`, `models/`, and `zsoda_ort/` instead of embedding Python/HF
  payloads into the `.aex`.
- Keep the packaging contract smoke-tested with a repo-local sidecar ORT zip
  before manual AE validation.
- Quantify the real NVIDIA runtime budget before freezing the shipping layout,
  including ORT GPU wheel size and the extra CUDA/cuDNN DLL set that CUDA EP
  requires.
- Keep a reproducible local export path from the existing Hugging Face snapshot
  to `distill_any_depth_base.onnx`, and validate the exported graph with an
  ORT CPU smoke before treating it as a shippable native model asset.
- Fail the export step when the ORT graph keeps a constant output shape across
  multiple square/non-square validation inputs. Quality controls must map to
  real input/output resolution changes for `distill-any-depth-base`.

## Validation

### Automated
- Build Debug and Release on Windows.
- Keep the split unit suites passing:
  `zsoda_core_tests`, `zsoda_ae_params_tests`, `zsoda_ae_router_tests`,
  `zsoda_inference_tests`, and `zsoda_render_tests`.
- Keep `zsoda_python_autostart_tests` passing for extracted Python runtime
  discovery logic and early launch-failure paths.
- Keep the shared packaging stage helper valid on both platforms and run at
  least one Windows packaging smoke for the prepared stage roots.
- Keep `sidecar-ort` packaging smoke passing for the Windows `.aex + models +
  zsoda_ort` release contract.
- Keep `tools/run_packaging_smoke.py` green as the repo-local packaging gate
  before manual AE smoke.
- Validate ORT model export on multiple input shapes so `Quality` remains real.

### Manual
- New AE project:
  - plugin loads as `ZSoda`
  - slicing controls are shown in the main UI
- Use the current Windows native ORT sidecar package at
  `artifacts/15_ort-sidecar-release-candidate/ZSoda-windows.zip` for the next
  manual AE smoke.
- Installation contract for the current Windows ORT package:
  - unzip the package
  - copy the single `Z-Soda/` folder into MediaCore
  - keep `Z-Soda/ZSoda.aex`, `Z-Soda/models/`, and `Z-Soda/zsoda_ort/`
    adjacent under that folder
- Playback:
  - quality changes alter render resolution
  - `Color Map` changes the depth-map visualization immediately
  - slice parameters alter the matte/output immediately
- Current Windows status:
  - release-candidate smoke has passed
- Current macOS status:
  - ORT-first AE smoke has passed after the session-IO black-screen fix
  - bundled ORT CPU/CoreML path is active from `ZSoda.plugin/Contents/Resources`
- Next cross-platform milestone:
  - keep the new macOS path covered by regression tests, diagnostics, and release-gate tooling

## Progress Tracking Contract

- `PROGRESS.md` is the live Korean status log.
- Update `PROGRESS.md` after each completed work unit.
- Keep entries append-only.
