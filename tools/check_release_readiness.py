#!/usr/bin/env python3
"""Check whether Z-Soda is ready for self-contained release packaging."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Report release-readiness status for macOS and Windows packaging."
    )
    parser.add_argument("--build-win", default="build-win", help="Windows build directory.")
    parser.add_argument("--build-mac", default="build-mac", help="macOS build directory.")
    parser.add_argument(
        "--release-assets",
        default="release-assets",
        help="Canonical release-assets directory.",
    )
    parser.add_argument("--dist-win", default="dist", help="Windows dist directory.")
    parser.add_argument("--dist-mac", default="dist-mac", help="macOS dist directory.")
    return parser.parse_args()


def check_path(path: Path, kind: str) -> bool:
    if kind == "file":
        return path.is_file()
    if kind == "dir":
        return path.is_dir()
    raise ValueError(kind)


def status_line(label: str, ok: bool, detail: str) -> str:
    marker = "OK" if ok else "MISSING"
    return f"[{marker}] {label}: {detail}"


def main() -> int:
    args = parse_args()

    build_win = Path(args.build_win)
    build_mac = Path(args.build_mac)
    release_assets = Path(args.release_assets)
    dist_win = Path(args.dist_win)
    dist_mac = Path(args.dist_mac)

    checks: list[tuple[str, bool, str]] = []

    manifest_path = release_assets / "asset-manifest.json"
    checks.append(("release-assets manifest", manifest_path.is_file(), str(manifest_path)))
    checks.append(
        ("macOS Python runtime", (release_assets / "python-macos").is_dir(), str(release_assets / "python-macos"))
    )
    checks.append(
        ("Windows Python runtime", (release_assets / "python-win").is_dir(), str(release_assets / "python-win"))
    )
    checks.append(("local model repos", (release_assets / "models").is_dir(), str(release_assets / "models")))

    build_win_aex = build_win / "plugin" / "Release" / "ZSoda.aex"
    build_mac_bundle = build_mac / "plugin" / "Release" / "ZSoda.plugin"
    checks.append(("built Windows plugin", build_win_aex.is_file(), str(build_win_aex)))
    checks.append(("built macOS plugin", build_mac_bundle.is_dir(), str(build_mac_bundle)))

    dist_win_zip = dist_win / "ZSoda-windows.zip"
    dist_mac_zip = dist_mac / "ZSoda-macos.zip"
    checks.append(("packaged Windows zip", dist_win_zip.is_file(), str(dist_win_zip)))
    checks.append(("packaged macOS zip", dist_mac_zip.is_file(), str(dist_mac_zip)))

    ok_count = sum(1 for _, ok, _ in checks if ok)
    total_count = len(checks)

    print("Z-Soda release readiness")
    print(f"score: {ok_count}/{total_count}")
    for label, ok, detail in checks:
        print(status_line(label, ok, detail))

    blockers: list[str] = []
    if not manifest_path.is_file():
        blockers.append("prepare release-assets with tools/prepare_release_assets.py")
    if not (release_assets / "python-macos").is_dir():
        blockers.append("stage portable macOS Python runtime into release-assets/python-macos")
    if not (release_assets / "python-win").is_dir():
        blockers.append("stage portable Windows Python runtime into release-assets/python-win")
    if not (release_assets / "models").is_dir():
        blockers.append("stage local HF model repos into release-assets/models")
    if not build_win_aex.is_file():
        blockers.append("produce a Windows Release build (build-win/plugin/Release/ZSoda.aex)")
    if not build_mac_bundle.is_dir():
        blockers.append("produce a macOS Release build (build-mac/plugin/Release/ZSoda.plugin)")
    if not dist_win_zip.is_file():
        blockers.append("run Windows self-contained packaging")
    if not dist_mac_zip.is_file():
        blockers.append("run macOS self-contained packaging")

    if blockers:
        print("\nremaining blockers:")
        for blocker in blockers:
            print(f"- {blocker}")
    else:
        print("\nremaining blockers:")
        print("- none in repository packaging state")
        print("- final external steps still remain: AE smoke on both OSes, mac notarization, GitHub Releases upload")

    if manifest_path.is_file():
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except Exception as exc:  # noqa: BLE001
            print(f"\nmanifest parse: failed ({exc})")
        else:
            print("\nasset manifest summary:")
            for key in ("python-macos", "python-win", "models", "hf-cache"):
                if key in manifest:
                    print(f"- {key}: present")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
