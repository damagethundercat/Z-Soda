# Models

Z-Soda currently ships one production model family:

- `distill-any-depth`
- `distill-any-depth-base`
- `distill-any-depth-large`

The preferred shipping entry is `distill-any-depth-base`.

## Current Shipping Contract

- Windows ORT sidecar packages stage the exported ONNX asset under:

```text
models/
  distill-any-depth/
    distill_any_depth_base.onnx
  models.manifest
```

- The ORT runtime resolves that local ONNX asset on the happy path.
- Python/Hugging Face runtime resolution is now legacy debug/fallback behavior,
  not the primary shipping contract.

## Manifest

- [models.manifest](models.manifest)

Format:

```text
id|display_name|relative_path|download_url|preferred_default|auxiliary_assets(relative_path::download_url;...)
```

The manifest stays versioned so model identity, export targets, and packaging
remain deterministic even when the runtime path changes.
