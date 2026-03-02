# PLAN.md - Depth Scanner-style After Effects Plugin (Execution Plan)

This file contains the implementation plan/checklists moved out of `AGENTS.md`.

## 1) Suggested Repo Layout
- /plugin/
  - /ae/           AE SDK glue (PF_Cmd handlers, params)
  - /core/         color/bit-depth conversions, cache, tiler, post-process
  - /inference/    ORT session mgmt, IO binding, pre/post
  - /backends/     cuda, directml, metal/coreml (optional)
- /models/         model manifests only; weights distributed separately
- /tools/          validation & regression utilities
- /docs/           parameters, performance notes

## 2) Minimum Feature Requirements
Inputs:
- Source layer/frame
- Optional: mask/guide input (future expansion)

Outputs (modes):
- Depth Map: normalized depth or inverse-depth (toggle)
- Slicing: matte where depth in [minDepth, maxDepth], with softness/feather

Core parameters (minimum):
- Model: DA3 / fallback model(s)
- Quality: draft/medium/high (resolution scale + post strength)
- Invert / Near-Far mapping
- Temporal Stabilize: off/low/high (optional)
- Tiling: tile size, overlap, fp16 toggle, VRAM budget hint
- Cache: enable, size limit, purge control (if feasible)

Post-processing (keep lightweight):
- edge-aware smoothing (optional), clamp/levels, simple denoise

## 3) Testing & Validation Plan
- Unit tests for core: normalization, slicing, tiling composition, cache keys
- Regression tests on a fixed frame set:
  - depth stats (mean/var/hist bins), matte area stability
- Performance tests:
  - 1080p/4K ms/frame, VRAM usage, cache hit rate
- Stability tests:
  - long-run (1000+ frames), ensure no leaks, no handle accumulation

## 4) Build & Packaging Plan
- Windows: MSVC + AE SDK, optional CUDA/DirectML builds
- macOS: Xcode + AE SDK, optional Metal/CoreML builds
- Deliverables: .aex/.plugin + separate model package + changelog/versioning

## 5) Suggested Work Sequence
1) P1: Scaffold plugin layout and AE SDK command handlers.
2) P2: Implement model/session lifecycle and cache-first render pipeline.
3) P3: Add depth map + slicing modes with 8/16/32 bpc boundary conversions.
4) P4: Implement fallback paths (OOM/backend fail -> tiling/downscale -> safe output).
5) P5: Add test/benchmark/stability suites and packaging scripts.

## 6) Progress Tracking Contract
- `PROGRESS.md` is the source of truth for live status updates.
- Track sequence items by IDs (`P1`..`P5`) in `PROGRESS.md`.
- Update `PROGRESS.md` after each completed work unit with:
  - status change (`대기`/`진행중`/`완료`/`차단`)
  - overall progress percentage
  - newly completed work and remaining tasks
