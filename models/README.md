# Models

Z-Soda currently ships a single production model family:

- `distill-any-depth`
- `distill-any-depth-base`
- `distill-any-depth-large`

The preferred production entry is `distill-any-depth-base`.

## Important

These model IDs are resolved through the local remote service:

- [tools/distill_any_depth_remote_service.py](/Users/Yongkyu/code/Z-Soda/tools/distill_any_depth_remote_service.py)

That means normal AE operation does not depend on checked-in ONNX weights under this
folder. The manifest still exists so model identity and packaging stay deterministic.

## Manifest

- [models.manifest](/Users/Yongkyu/code/Z-Soda/models/models.manifest)

Format:

```text
id|display_name|relative_path|download_url|preferred_default|auxiliary_assets(relative_path::download_url;...)
```

For DistillAnyDepth entries, `download_url` is intentionally `remote://...` because
runtime resolution is delegated to the remote service rather than local model files.
