# Remote Inference Worker MVP (stdin/stdout JSON)

This document defines the minimum roundtrip contract for the out-of-process
depth worker used in early integration tests.

Implemented worker:
- `tools/remote_inference_worker.py`
- Runtime requirements: Python 3.x (stdlib only, no extra dependencies)

## 1) Transport Contract

- Process model: one-shot
- Supported invocation modes:
  - `stdin/stdout` mode: read a single JSON request from `stdin`, write a single JSON response to `stdout`
  - `file-arg` mode: `python3 tools/remote_inference_worker.py <request_json_path> <response_json_path>`
    - request JSON is read from `<request_json_path>`
    - response JSON is written to `<response_json_path>`
- Exit codes:
  - `0`: success (`"ok": true`)
  - `1`: invalid request or internal error (`"ok": false`)
  - `2`: invalid environment configuration (`"ok": false`, `error.code=invalid_env`)
  - `3`: forced failure for testing (`"ok": false`, `error.code=forced_error`)

## 2) Request JSON

Required fields:
- `width` (int, `>= 1`)
- `height` (int, `>= 1`)

Optional fields:
- `request_id` (string): echoed in response for correlation
- `frame_index` (int, `>= 0`, default `0`)
- `pattern` (string): `gradient-v1` or `checker-v1`

Example request:

```json
{
  "request_id": "frame-0001",
  "width": 4,
  "height": 3,
  "frame_index": 12,
  "pattern": "gradient-v1"
}
```

## 3) Success Response JSON

Top-level:
- `ok`: `true`
- `protocol_version`: `1`
- `request_id`: echoed string or `null`
- `worker`: name/version metadata
- `depth`: generated depth map payload
- `meta`: deterministic generation metadata and stats

Depth payload:
- `encoding`: `row-major-f32-json`
- `width`, `height`
- `values`: flattened `width * height` float array in row-major order, range `[0, 1]`

Stats:
- `min`, `max`, `mean`
- `count`
- `sha256`: checksum over depth values stringified to 6 decimals

Example success response:

```json
{
  "ok": true,
  "protocol_version": 1,
  "request_id": "frame-0001",
  "worker": {
    "name": "zsoda-remote-inference-mvp",
    "version": "0.1.0"
  },
  "depth": {
    "encoding": "row-major-f32-json",
    "width": 2,
    "height": 2,
    "values": [0.168387, 0.558152, 0.543372, 0.910264]
  },
  "meta": {
    "frame_index": 12,
    "pattern": "gradient-v1",
    "seed": 1337,
    "stats": {
      "min": 0.168387,
      "max": 0.910264,
      "mean": 0.545044,
      "count": 4,
      "sha256": "b32922f8db2a2f2aaa8023b63e8de23fe49f5a0dc6576c7e74c6a151dfb25e2a"
    }
  }
}
```

## 4) Error Response JSON

Top-level:
- `ok`: `false`
- `protocol_version`: `1`
- `request_id`: echoed when available
- `worker`
- `error`: `{ "code": string, "message": string, "details"?: object }`

Common error codes:
- `invalid_request`
- `invalid_env`
- `forced_error`
- `internal_error`

## 5) Deterministic Output Rule

For each pixel `(x, y)`, the worker computes a deterministic depth value using:
- normalized coordinate gradient
- deterministic integer-hash wobble derived from `x`, `y`, `frame_index`, `seed`
- optional checker modulation (`checker-v1`)

Outputs are clamped to `[0, 1]` and rounded to 6 decimals to keep snapshots and
test expectations stable.

## 6) Environment Variables

- `ZSODA_REMOTE_WORKER_SEED`
  - Default: `1337`
  - Integer seed used by the deterministic generator.

- `ZSODA_REMOTE_WORKER_PATTERN`
  - Default: `gradient-v1`
  - Default pattern when request omits `pattern`.
  - Allowed: `gradient-v1`, `checker-v1`

- `ZSODA_REMOTE_WORKER_DELAY_MS`
  - Default: `0`
  - Adds artificial latency before response generation.
  - Range: `0..120000`

- `ZSODA_REMOTE_WORKER_MAX_PIXELS`
  - Default: `16777216`
  - Rejects requests where `width * height` exceeds this budget.

- `ZSODA_REMOTE_WORKER_FORCE_ERROR`
  - Default: empty
  - If set to non-empty text, worker returns `forced_error` and exits with code `3`.

- `ZSODA_REMOTE_WORKER_LOG_STDERR`
  - Default: `0`
  - `1/true/yes/on` enables per-request summary logs to `stderr`.

## 7) CLI Examples

Success:

```bash
echo '{"request_id":"demo-1","width":4,"height":3,"frame_index":5}' \
  | python3 tools/remote_inference_worker.py
```

Forced error:

```bash
echo '{"request_id":"demo-err","width":4,"height":3}' \
  | ZSODA_REMOTE_WORKER_FORCE_ERROR="test fail path" \
    python3 tools/remote_inference_worker.py
```

Plugin-compatible file-arg mode:

```bash
python3 tools/remote_inference_worker.py /tmp/req.json /tmp/res.json
```
