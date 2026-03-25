#!/usr/bin/env python3
"""Prepare the shared Z-Soda packaging stage layout and emit its plan as JSON."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from package_layout import prepare_package_stage


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Prepare the shared package staging layout for Windows/macOS packagers."
    )
    parser.add_argument("--platform", required=True, choices=("windows", "macos"))
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument(
        "--package-mode",
        default="embedded-windows",
        choices=("embedded-windows", "sidecar-ort"),
    )
    parser.add_argument("--include-manifest", action="store_true")
    parser.add_argument("--python-runtime-dir", default="")
    parser.add_argument("--model-repo-dir", default="")
    parser.add_argument("--model-root-dir", default="")
    parser.add_argument("--hf-cache-dir", default="")
    parser.add_argument("--ort-runtime-dir", default="")
    parser.add_argument("--require-self-contained", action="store_true")
    parser.add_argument("--plan-out", default="")
    parser.add_argument("--quiet", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    plan = prepare_package_stage(
        platform=args.platform,
        build_dir=args.build_dir,
        output_dir=args.output_dir,
        package_mode=args.package_mode,
        include_manifest=args.include_manifest,
        python_runtime_dir=args.python_runtime_dir,
        model_repo_dir=args.model_repo_dir,
        model_root_dir=args.model_root_dir,
        hf_cache_dir=args.hf_cache_dir,
        require_self_contained=args.require_self_contained,
        ort_runtime_dir=args.ort_runtime_dir,
    )
    payload = json.dumps(plan.to_json_dict(), indent=2, sort_keys=True)
    if args.plan_out:
        plan_path = Path(args.plan_out)
        plan_path.parent.mkdir(parents=True, exist_ok=True)
        plan_path.write_text(payload + "\n", encoding="utf-8")
    if not args.quiet:
        print(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
