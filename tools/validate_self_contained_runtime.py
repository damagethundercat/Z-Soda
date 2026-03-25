#!/usr/bin/env python3
"""Validate that a staged self-contained runtime can load the bundled model."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

from package_layout import MACOS_BUNDLED_PYTHON_CANDIDATES, WINDOWS_BUNDLED_PYTHON_CANDIDATES


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate a staged Z-Soda self-contained runtime bundle."
    )
    parser.add_argument("--stage-root", required=True)
    parser.add_argument("--platform", required=True, choices=("windows", "macos"))
    parser.add_argument("--model-id", default="distill-any-depth-base")
    parser.add_argument("--validate-device", default="cpu")
    return parser.parse_args()


def bundled_python_candidates(platform: str) -> tuple[str, ...]:
    if platform == "windows":
        return WINDOWS_BUNDLED_PYTHON_CANDIDATES
    return MACOS_BUNDLED_PYTHON_CANDIDATES


def resolve_bundled_python(stage_root: Path, platform: str) -> Path:
    python_root = stage_root / "zsoda_py"
    for candidate in bundled_python_candidates(platform):
        python_path = python_root / candidate
        if python_path.is_file():
            return python_path
    raise FileNotFoundError(f"bundled Python executable not found under: {python_root}")


def main() -> int:
    args = parse_args()
    stage_root = Path(args.stage_root).resolve()
    service_script = stage_root / "zsoda_py" / "distill_any_depth_remote_service.py"
    if not service_script.is_file():
        raise FileNotFoundError(f"bundled service script not found: {service_script}")

    python_path = resolve_bundled_python(stage_root, args.platform)
    env = os.environ.copy()
    env["ZSODA_REMOTE_SERVICE_ASSET_ROOT"] = str(stage_root)

    command = [
        str(python_path),
        str(service_script),
        "--validate-bundle",
        "--repo-root",
        str(stage_root),
        "--preload-model-id",
        args.model_id,
        "--validate-device",
        args.validate_device,
    ]
    result = subprocess.run(command, env=env, capture_output=True, text=True, check=False)
    if result.stdout:
        sys.stdout.write(result.stdout)
    if result.stderr:
        sys.stderr.write(result.stderr)
    if result.returncode != 0:
        raise SystemExit(result.returncode)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
