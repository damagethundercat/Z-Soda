#!/usr/bin/env python3
"""Remote inference worker MVP for stdin/stdout roundtrip tests.

This worker is intentionally dependency-free and deterministic. It reads one JSON
request from stdin and writes one JSON response to stdout.
"""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import sys
import time
from typing import Any, Dict, List

PROTOCOL_VERSION = 1
WORKER_NAME = "zsoda-remote-inference-mvp"
WORKER_VERSION = "0.1.0"
DEFAULT_PATTERN = "gradient-v1"
SUPPORTED_PATTERNS = {DEFAULT_PATTERN, "checker-v1"}


class RequestError(ValueError):
    """Raised when request payload validation fails."""


def _read_int_env(name: str, default: int, minimum: int | None = None, maximum: int | None = None) -> int:
    value = os.getenv(name, "").strip()
    if not value:
        return default

    try:
        parsed = int(value, 10)
    except ValueError as exc:  # pragma: no cover - defensive branch
        raise ValueError(f"{name} must be an integer: {value!r}") from exc

    if minimum is not None and parsed < minimum:
        raise ValueError(f"{name} must be >= {minimum}: {parsed}")
    if maximum is not None and parsed > maximum:
        raise ValueError(f"{name} must be <= {maximum}: {parsed}")
    return parsed


def _read_bool_env(name: str, default: bool) -> bool:
    value = os.getenv(name, "").strip().lower()
    if not value:
        return default
    if value in {"1", "true", "yes", "on"}:
        return True
    if value in {"0", "false", "no", "off"}:
        return False
    raise ValueError(f"{name} must be a boolean-like value (0/1/true/false): {value!r}")


def _write_response(payload: Dict[str, Any]) -> None:
    sys.stdout.write(json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True))
    sys.stdout.write("\n")
    sys.stdout.flush()


def _write_response_to_file(path: pathlib.Path, payload: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True),
        encoding="utf-8",
    )


def _write_error(
    *,
    request_id: str | None,
    code: str,
    message: str,
    details: Dict[str, Any] | None = None,
) -> Dict[str, Any]:
    response: Dict[str, Any] = {
        "ok": False,
        "protocol_version": PROTOCOL_VERSION,
        "request_id": request_id,
        "worker": {"name": WORKER_NAME, "version": WORKER_VERSION},
        "error": {"code": code, "message": message},
    }
    if details:
        response["error"]["details"] = details
    return response


def _read_request(stdin_text: str) -> Dict[str, Any]:
    if not stdin_text.strip():
        raise RequestError("stdin is empty")
    try:
        payload = json.loads(stdin_text)
    except json.JSONDecodeError as exc:
        raise RequestError(f"invalid JSON: {exc.msg}") from exc
    if not isinstance(payload, dict):
        raise RequestError("request must be a JSON object")
    return payload


def _require_int(payload: Dict[str, Any], key: str, minimum: int = 0) -> int:
    value = payload.get(key)
    if isinstance(value, bool) or not isinstance(value, int):
        raise RequestError(f"{key!r} must be an integer")
    if value < minimum:
        raise RequestError(f"{key!r} must be >= {minimum}")
    return value


def _resolve_dimensions(payload: Dict[str, Any]) -> tuple[int, int]:
    source = payload.get("source")
    if isinstance(source, dict):
        width = source.get("width")
        height = source.get("height")
        if isinstance(width, int) and isinstance(height, int) and width > 0 and height > 0:
            return width, height
    width = _require_int(payload, "width", minimum=1)
    height = _require_int(payload, "height", minimum=1)
    return width, height


def _resolve_pattern(payload: Dict[str, Any], default_pattern: str) -> str:
    raw = payload.get("pattern", default_pattern)
    if not isinstance(raw, str):
        raise RequestError("'pattern' must be a string")
    pattern = raw.strip().lower()
    if pattern not in SUPPORTED_PATTERNS:
        raise RequestError(f"'pattern' must be one of {sorted(SUPPORTED_PATTERNS)}")
    return pattern


def _build_depth_values(width: int, height: int, frame_index: int, seed: int, pattern: str) -> List[float]:
    max_x = max(width - 1, 1)
    max_y = max(height - 1, 1)
    values: List[float] = []

    for y in range(height):
        for x in range(width):
            norm_x = x / max_x
            norm_y = y / max_y
            gradient = 0.5 * (norm_x + norm_y)

            # Lightweight deterministic pseudo-random wobble to avoid a fully flat ramp.
            wobble_bits = (x * 92837111) ^ (y * 689287499) ^ ((frame_index + seed) * 283923481)
            wobble = float(wobble_bits & 1023) / 1023.0

            depth = 0.82 * gradient + 0.18 * wobble
            if pattern == "checker-v1":
                checker = 0.12 if ((x + y + frame_index) % 2 == 0) else -0.12
                depth += checker

            depth = min(1.0, max(0.0, depth))
            values.append(round(depth, 6))

    return values


def _compute_depth_stats(values: List[float]) -> Dict[str, Any]:
    if not values:
        return {"min": 0.0, "max": 0.0, "mean": 0.0, "count": 0}
    minimum = min(values)
    maximum = max(values)
    mean = round(sum(values) / len(values), 6)
    checksum_input = ",".join(f"{value:.6f}" for value in values).encode("ascii")
    checksum = hashlib.sha256(checksum_input).hexdigest()
    return {
        "min": minimum,
        "max": maximum,
        "mean": mean,
        "count": len(values),
        "sha256": checksum,
    }


def main() -> int:
    request_id: str | None = None

    try:
        seed = _read_int_env("ZSODA_REMOTE_WORKER_SEED", 1337)
        delay_ms = _read_int_env("ZSODA_REMOTE_WORKER_DELAY_MS", 0, minimum=0, maximum=120000)
        max_pixels = _read_int_env("ZSODA_REMOTE_WORKER_MAX_PIXELS", 16_777_216, minimum=1)
        log_stderr = _read_bool_env("ZSODA_REMOTE_WORKER_LOG_STDERR", False)
        default_pattern = os.getenv("ZSODA_REMOTE_WORKER_PATTERN", DEFAULT_PATTERN).strip().lower() or DEFAULT_PATTERN
        if default_pattern not in SUPPORTED_PATTERNS:
            raise ValueError(
                "ZSODA_REMOTE_WORKER_PATTERN must be one of "
                + ", ".join(sorted(SUPPORTED_PATTERNS))
                + f"; got {default_pattern!r}"
            )
    except ValueError as exc:
        _write_response(
            _write_error(request_id=None, code="invalid_env", message=str(exc), details={"stage": "env_parse"})
        )
        return 2

    force_error = os.getenv("ZSODA_REMOTE_WORKER_FORCE_ERROR", "").strip()

    request_file: pathlib.Path | None = None
    response_file: pathlib.Path | None = None
    if len(sys.argv) not in {1, 3}:
        _write_response(
            _write_error(
                request_id=None,
                code="invalid_request",
                message="usage: remote_inference_worker.py [<request_json_path> <response_json_path>]",
            )
        )
        return 1
    if len(sys.argv) == 3:
        request_file = pathlib.Path(sys.argv[1]).expanduser()
        response_file = pathlib.Path(sys.argv[2]).expanduser()

    try:
        if request_file is not None:
            request_text = request_file.read_text(encoding="utf-8")
        else:
            request_text = sys.stdin.read()
        request = _read_request(request_text)
        raw_request_id = request.get("request_id")
        if raw_request_id is not None and not isinstance(raw_request_id, str):
            raise RequestError("'request_id' must be a string when provided")
        request_id = raw_request_id

        width, height = _resolve_dimensions(request)
        frame_index = _require_int(request, "frame_index", minimum=0) if "frame_index" in request else 0
        pattern = _resolve_pattern(request, default_pattern)

        pixel_count = width * height
        if pixel_count > max_pixels:
            raise RequestError(
                f"request exceeds max pixel budget: {pixel_count} > {max_pixels}. "
                "Set ZSODA_REMOTE_WORKER_MAX_PIXELS to override."
            )

        if force_error:
            response = _write_error(
                request_id=request_id,
                code="forced_error",
                message="forced by ZSODA_REMOTE_WORKER_FORCE_ERROR",
                details={"reason": force_error},
            )
            if response_file is not None:
                _write_response_to_file(response_file, response)
            else:
                _write_response(response)
            return 3

        if delay_ms > 0:
            time.sleep(delay_ms / 1000.0)

        values = _build_depth_values(width, height, frame_index, seed, pattern)
        stats = _compute_depth_stats(values)

        response: Dict[str, Any] = {
            "ok": True,
            "protocol_version": PROTOCOL_VERSION,
            "request_id": request_id,
            "worker": {"name": WORKER_NAME, "version": WORKER_VERSION},
            "depth": {
                "encoding": "row-major-f32-json",
                "width": width,
                "height": height,
                "values": values,
            },
            "meta": {
                "frame_index": frame_index,
                "pattern": pattern,
                "seed": seed,
                "stats": stats,
            },
        }

        if log_stderr:
            print(
                f"[{WORKER_NAME}] request_id={request_id!r} size={width}x{height} "
                f"frame_index={frame_index} pattern={pattern} count={stats['count']}",
                file=sys.stderr,
            )

        if response_file is not None:
            _write_response_to_file(response_file, response)
        else:
            _write_response(response)
        return 0

    except RequestError as exc:
        response = _write_error(request_id=request_id, code="invalid_request", message=str(exc))
        if response_file is not None:
            _write_response_to_file(response_file, response)
        else:
            _write_response(response)
        return 1
    except Exception as exc:  # pragma: no cover - last-resort safety net
        response = _write_error(request_id=request_id, code="internal_error", message=str(exc))
        if response_file is not None:
            _write_response_to_file(response_file, response)
        else:
            _write_response(response)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
