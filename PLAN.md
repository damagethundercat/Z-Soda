# PLAN.md - ZSoda Execution Plan

This file keeps the current production-facing implementation plan. Historical
research and experiment notes belong in `docs/research/` and should not drive
the shipping path directly.

## Product Baseline

- Host: Adobe After Effects
- Production model: `distill-any-depth-base`
- Runtime: local Python remote service with binary localhost transport
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
- Reuse inference sessions and remote service process.
- Keep the default path fixed to `distill-any-depth-base`.
- Never require legacy DA3 tooling or cloud services for normal operation.

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

### `RF-02` AE layer cleanup
- Keep only the visible production controls.
- Move AE state handling toward per-sequence and per-instance storage.
- Avoid relying on one global shared parameter snapshot for real AE renders.

### `RF-03` Runtime and core cleanup
- Keep remote transport binary-first.
- Treat legacy JSON and file transport as debug-only fallback.

### `RF-04` Slicing UX
- Keep the shipping baseline centered on DAD-base.
- Expose depth-map display as `Output` plus `Color Map`.
- Expose slicing as `Output`, `Slice Mode`, `Position (%)`, `Range (%)`, and `Soft Border (%)`.
- Do not reintroduce UI-facing quality boost or temporal toggles in the shipping path.

## Validation

### Automated
- Build Debug and Release on Windows.
- Keep `zsoda_tests` passing for current production paths.
- Validate binary remote transport and remote service startup.

### Manual
- New AE project:
  - plugin loads as `ZSoda`
  - slicing controls are shown in the main UI
- Playback:
  - quality changes alter render resolution
  - `Color Map` changes the depth-map visualization immediately
  - slice parameters alter the matte/output immediately

## Progress Tracking Contract

- `PROGRESS.md` is the live Korean status log.
- Update `PROGRESS.md` after each completed work unit.
- Keep entries append-only.
