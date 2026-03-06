# Official DA3 Python Backend Integration Notes

Date: 2026-03-07

## Why this path

Official Depth Anything 3 distribution is Python-first:

- Official repo: `ByteDance-Seed/Depth-Anything-3`
- Official base weights card: `depth-anything/DA3-BASE`
- Official large 1.1 card: `depth-anything/DA3-LARGE-1.1`

For `from_pretrained(...)`, the service defaults follow the lowercase repo ids shown in official Hugging Face examples:

- `depth-anything/da3-small`
- `depth-anything/da3-base`
- `depth-anything/da3-large`

That means the cleanest quality path for Z-Soda is:

1. Keep the current in-plugin ONNX path as the default fast path.
2. Add an opt-in official Python path for quality experiments.
3. Revisit ONNX/TensorRT after the official Python baseline is understood.

## Transport change

The old remote MVP serialized the full source frame as JSON float arrays. That is not viable for real AE frames.

The new remote request shape writes the source frame to a temp `PPM` file and sends a lightweight JSON payload:

```json
{
  "schema": "zsoda.remote_depth.v2",
  "model_id": "depth-anything-v3-base",
  "quality": 4,
  "resize_mode": "upper_bound_letterbox",
  "source": {
    "width": 1920,
    "height": 1080,
    "channels": 3,
    "format": "ppm",
    "path": "C:/.../zsoda-remote-source-....ppm"
  }
}
```

## Service/client split

Preferred path:

- Z-Soda `RemoteInferenceBackend` -> local HTTP endpoint directly
- `tools/official_da3_remote_service.py` keeps the official model loaded in memory

Fallback path:

- `tools/official_da3_remote_service.py`
  - Persistent local HTTP service
  - Loads official DA3 once and keeps it hot in memory
  - Endpoint: `POST /zsoda/depth`
  - Status: `GET /status`
- `tools/official_da3_remote_client.py`
  - Tiny bridge for legacy command-mode remote execution
  - Reads request JSON
  - Forwards it to the persistent service
  - Writes the response JSON back to disk

The new direct-endpoint path avoids both per-frame model loads and the extra client process.

## Current expected setup

The manual-terminal prototype is no longer the target shape.

Current direction:

1. Stage `official_da3_remote_service.py` and the `depth_anything_3` package next to the plugin under `zsoda_py/`.
2. Let `RemoteInferenceBackend` auto-start the local service when the remote backend is explicitly selected.
3. Reuse the local HTTP service for subsequent AE renders without any user-run terminal.

The current auto-start path still expects the remote backend to be selected explicitly, for example with:

```powershell
$env:ZSODA_INFERENCE_BACKEND = "remote"
$env:ZSODA_REMOTE_SERVICE_AUTOSTART = "1"
$env:ZSODA_LOCKED_MODEL_ID = "depth-anything-v3-base"
```

Manual `python ...official_da3_remote_service.py` startup is now only a debugging fallback.

Legacy fallback if direct endpoint mode is unavailable:

```powershell
$env:ZSODA_REMOTE_DA3_SERVICE_URL = "http://127.0.0.1:8345/zsoda/depth"
$env:ZSODA_REMOTE_INFERENCE_COMMAND = '"C:\Path\To\pythonw.exe" "C:\Users\Yongkyu\code\Z-Soda\tools\official_da3_remote_client.py" {request} {response}'
```

## Model mapping

- `depth-anything-v3-small` -> `depth-anything/da3-small`
- `depth-anything-v3-small-multiview` -> `depth-anything/da3-small`
- `depth-anything-v3-base` -> `depth-anything/da3-base`
- `depth-anything-v3-large` -> `depth-anything/da3-large`
- `depth-anything-v3-large-multiview` -> `depth-anything/da3-large`

Each repo id can be overridden with env vars:

- `ZSODA_DA3_MODEL_SMALL`
- `ZSODA_DA3_MODEL_BASE`
- `ZSODA_DA3_MODEL_LARGE`
- `ZSODA_DA3_DEFAULT_MODEL_REPO`

## Limitations

- The HTTP service response still returns depth values as JSON floats.
- The remote backend is not yet the default AE path.
- This is not the final performance architecture.

The next step, if this path proves worthwhile, is a binary response path or shared-memory IPC to remove JSON float overhead.
