#!/usr/bin/env python3
"""Thin request/response bridge to a persistent local DA3 service."""

from __future__ import annotations

import json
import os
import pathlib
import sys
import urllib.error
import urllib.request


def resolve_endpoint() -> str:
    endpoint = (
        os.getenv("ZSODA_REMOTE_DA3_SERVICE_URL", "").strip()
        or os.getenv("ZSODA_REMOTE_INFERENCE_ENDPOINT", "").strip()
    )
    if not endpoint:
        raise RuntimeError(
            "ZSODA_REMOTE_DA3_SERVICE_URL is not set. "
            "Point it to the official DA3 service endpoint, for example "
            "http://127.0.0.1:8345/zsoda/depth"
        )
    return endpoint


def resolve_timeout_seconds() -> float:
    raw = (
        os.getenv("ZSODA_REMOTE_DA3_SERVICE_TIMEOUT_MS", "").strip()
        or os.getenv("ZSODA_REMOTE_INFERENCE_TIMEOUT_MS", "").strip()
    )
    if not raw:
        return 300.0
    try:
        milliseconds = max(1000, int(raw, 10))
    except ValueError as exc:
        raise RuntimeError(
            f"invalid timeout value for ZSODA_REMOTE_DA3_SERVICE_TIMEOUT_MS: {raw!r}"
        ) from exc
    return milliseconds / 1000.0


def main() -> int:
    if len(sys.argv) != 3:
        print(
            "usage: official_da3_remote_client.py <request_json_path> <response_json_path>",
            file=sys.stderr,
        )
        return 1

    request_path = pathlib.Path(sys.argv[1]).expanduser().resolve()
    response_path = pathlib.Path(sys.argv[2]).expanduser().resolve()
    response_path.parent.mkdir(parents=True, exist_ok=True)

    try:
        endpoint = resolve_endpoint()
        timeout_seconds = resolve_timeout_seconds()
        payload = request_path.read_bytes()

        request = urllib.request.Request(
            endpoint,
            data=payload,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=timeout_seconds) as handle:
            response_bytes = handle.read()

        parsed = json.loads(response_bytes.decode("utf-8"))
        response_path.write_text(
            json.dumps(parsed, separators=(",", ":"), ensure_ascii=True),
            encoding="utf-8",
        )
        return 0
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        print(f"HTTP {exc.code}: {detail}", file=sys.stderr)
        return 2
    except Exception as exc:  # noqa: BLE001
        print(str(exc), file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
