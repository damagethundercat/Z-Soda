# AGENTS.md - Depth Scanner-style After Effects Plugin (Essential Instructions)

This document instructs coding agents working in this repo to build and maintain an After Effects effect plugin that (1) ingests AE frames, (2) runs local depth inference (e.g., Depth Anything 3-class models) inside the plugin, and (3) exposes depth-map and depth-slicing outputs for compositing.

## 1) Scope / Non-scope
In scope:
- AE effect plugin (AE Plug-in SDK) with per-frame depth generation
- Depth Map output (normalized 0..1), and Slicing output (depth-band matte)
- Caching, tiling, and safe fallbacks
- 8/16/32 bpc support
- Optional lightweight temporal stabilization modes

Out of scope:
- Training/fine-tuning models
- Cloud inference requirements for core functionality
- Shipping model weights inside git (use LFS or external packaging)

## 2) Architecture (must follow)
After Effects Host -> Plugin (C++/AE SDK) -> Inference Runtime (prefer ONNX Runtime) -> Backend (CUDA/DirectML/Metal/CoreML)

Priority order for any frame:
1) Cache hit -> return immediately
2) Cache miss -> run inference (possibly proxy first) -> write cache -> return
3) OOM / backend failure -> tile/downscale fallback -> return
4) Hard failure -> pass-through or safe empty output (never crash AE)

Never load models or do file I/O in the hot per-frame render path.

## 3) Model & Runtime Rules
- Load/initialize the model once (or on model/setting change), not per frame.
- Reuse inference sessions; avoid graph recompilation.
- Minimize memory copies; prefer GPU IO binding where possible.
- If GPU backends differ by OS, document capability and provide clear fallbacks.

## 4) AE Host Constraints (critical)
- AE calls effects frequently and differently during scrub/preview/render.
- Never block the render thread with long operations; design for responsiveness.
- Thread safety is mandatory: cache/session access must be synchronized carefully.
- Support 8/16/32 bpc. Use float internally and convert only at boundaries.
- On any error: return safe output; never throw exceptions across SDK boundaries.

## 5) Coding Standards (enforced)
- Avoid per-frame allocations; use buffer pools.
- Rate-limit logs; no verbose logging in the render loop.
- All failures must propagate as error codes and trigger fallbacks.
- Any new dependency must be version-pinned and license-documented.

## 6) Git / PR Rules
- Commit format: `module: summary` (e.g., `inference: reuse ORT session`)
- Any perf-affecting change must include before/after benchmarks in the PR.
- Do not commit model weights or large binaries directly into git.

## 7) Plan and Progress Documents
Execution plan items are maintained in `PLAN.md`.
Live execution status must be maintained in `PROGRESS.md` (Korean), and updated after each completed work unit.
Use `AGENTS.md` for mandatory constraints, `PLAN.md` for sequencing/checklists, and `PROGRESS.md` for current status/remaining work.
