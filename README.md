# Z-Soda

After Effects depth effect plugin focused on one shipping path:

- host: Adobe After Effects
- model: `distill-any-depth-base`
- runtime: native ONNX Runtime sidecar
- outputs: `Depth Map` and `Depth Slice`

## Current Product Shape

- Windows shipping install shape:
  - `Z-Soda/ZSoda.aex`
  - `Z-Soda/models/`
  - `Z-Soda/zsoda_ort/`
- AE UI exposes only the shipping controls:
  - `Quality`
  - `Preserve Ratio`
  - `Output`
  - `Color Map`
  - `Slice Mode`
  - `Position (%)`
  - `Range (%)`
  - `Soft Border (%)`
- `Quality` maps to real process-resolution changes.
- Python remote service remains available only for explicit debug/fallback work.

## Repository Layout

- [PLAN.md](PLAN.md): execution checklist
- [PROGRESS.md](PROGRESS.md): live work log in Korean
- [plugin/ae](plugin/ae): AE entry, params, host bridge
- [plugin/core](plugin/core): cache, render pipeline, postprocess
- [plugin/inference](plugin/inference): engine/runtime/backend glue
- [tools](tools): build/package/runtime helper scripts
- [tests](tests): unit/integration harnesses
- [docs/build/README.md](docs/build/README.md): current build/package guide

## Runtime Notes

- Production model is fixed to `distill-any-depth-base`.
- The preferred user-facing path is bundled ORT sidecar inference.
- The plugin should resolve `models/` and `zsoda_ort/` next to `ZSoda.aex`
  before considering any legacy embedded or remote fallback path.
- Hard failures must return safe output through the render pipeline and never crash AE.

## Build

Windows build and package guidance lives in:

- [docs/build/README.md](docs/build/README.md)
- [docs/build/LOCAL_AGENT_HANDOFF.md](docs/build/LOCAL_AGENT_HANDOFF.md)

macOS handoff guidance lives in:

- [docs/build/MAC_AGENT_HANDOFF.md](docs/build/MAC_AGENT_HANDOFF.md)

## Packaging

- Windows shipping package is a zip containing one top-level `Z-Soda/` folder.
- That folder contains:
  - `ZSoda.aex`
  - `models/distill-any-depth/distill_any_depth_base.onnx`
  - `zsoda_ort/onnxruntime.dll`
  - provider/runtime DLLs required by the chosen ORT EP
- Legacy self-contained and thin-bootstrap notes are kept only as historical
  references under [docs/build](docs/build).

## Models

- Manifest: [models/models.manifest](models/models.manifest)
- Shipping model family:
  - `distill-any-depth`
  - `distill-any-depth-base`
  - `distill-any-depth-large`

The current ORT shipping path uses exported ONNX weights staged under
`models/distill-any-depth/`.
