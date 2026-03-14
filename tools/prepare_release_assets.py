#!/usr/bin/env python3
"""Prepare canonical release-assets/ layout for self-contained packaging."""

from __future__ import annotations

import argparse
import json
import os
import shutil
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Stage portable runtimes and local HF model snapshots into release-assets/."
    )
    parser.add_argument(
        "--output-dir",
        default="release-assets",
        help="Destination directory for canonical release asset layout.",
    )
    parser.add_argument(
        "--macos-python-runtime-dir",
        default="",
        help="Portable macOS Python runtime to stage under release-assets/python-macos.",
    )
    parser.add_argument(
        "--windows-python-runtime-dir",
        default="",
        help="Portable Windows Python runtime to stage under release-assets/python-win.",
    )
    parser.add_argument(
        "--model-repo-dir",
        default="",
        help="Directory whose child folders are local HF repos named by model id.",
    )
    parser.add_argument(
        "--hf-cache-dir",
        default="",
        help="Optional Hugging Face cache directory to stage under release-assets/hf-cache.",
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="Delete the output directory before staging the provided assets.",
    )
    return parser.parse_args()


def copy_tree_contents(source_dir: Path, destination_dir: Path) -> None:
    destination_dir.mkdir(parents=True, exist_ok=True)
    for child in source_dir.iterdir():
        target = destination_dir / child.name
        if target.exists():
            if target.is_dir() and not target.is_symlink():
                shutil.rmtree(target)
            else:
                target.unlink()
        if child.is_dir() and not child.is_symlink():
            shutil.copytree(child, target)
        else:
            shutil.copy2(child, target)


def detect_python_entry(root: Path, platform_name: str) -> str:
    if platform_name == "macos":
      candidates = (
          root / "bin" / "python3",
          root / "python" / "bin" / "python3",
          root / "runtime" / "bin" / "python3",
      )
    else:
      candidates = (
          root / "python.exe",
          root / "python" / "python.exe",
          root / "runtime" / "python.exe",
      )

    for candidate in candidates:
        if candidate.is_file():
            return str(candidate.relative_to(root))
    raise SystemExit(
        f"{platform_name} portable Python runtime is missing an expected interpreter entrypoint "
        f"under {root}"
    )


def summarize_directory(path: Path) -> dict[str, object]:
    file_count = 0
    total_bytes = 0
    for entry in path.rglob("*"):
        if entry.is_file():
            file_count += 1
            total_bytes += entry.stat().st_size
    return {
        "path": str(path),
        "file_count": file_count,
        "total_bytes": total_bytes,
    }


def stage_python_runtime(
    source_text: str,
    output_root: Path,
    folder_name: str,
    platform_name: str,
    manifest: dict[str, object],
) -> None:
    if not source_text:
        return
    source_dir = Path(source_text).expanduser().resolve()
    if not source_dir.is_dir():
        raise SystemExit(f"{platform_name} Python runtime directory was not found: {source_dir}")

    destination = output_root / folder_name
    if destination.exists():
        shutil.rmtree(destination)
    copy_tree_contents(source_dir, destination)
    manifest[folder_name] = {
        **summarize_directory(destination),
        "python_entry": detect_python_entry(destination, platform_name),
        "source": str(source_dir),
    }


def stage_model_repos(source_text: str, output_root: Path, manifest: dict[str, object]) -> None:
    if not source_text:
        return
    source_root = Path(source_text).expanduser().resolve()
    if not source_root.is_dir():
        raise SystemExit(f"Model repo directory was not found: {source_root}")

    destination_root = output_root / "models"
    destination_root.mkdir(parents=True, exist_ok=True)
    repo_dest_root = destination_root

    model_repos: list[dict[str, object]] = []
    repo_dirs = sorted(entry for entry in source_root.iterdir() if entry.is_dir())
    if not repo_dirs:
        raise SystemExit(f"Model repo directory does not contain any model subdirectories: {source_root}")

    for repo_dir in repo_dirs:
        destination = repo_dest_root / repo_dir.name
        if destination.exists():
            shutil.rmtree(destination)
        copy_tree_contents(repo_dir, destination)
        model_repos.append(
            {
                "model_id": repo_dir.name,
                **summarize_directory(destination),
                "source": str(repo_dir),
            }
        )

    manifest["models"] = {
        "path": str(repo_dest_root),
        "repos": model_repos,
    }


def stage_hf_cache(source_text: str, output_root: Path, manifest: dict[str, object]) -> None:
    if not source_text:
        return
    source_dir = Path(source_text).expanduser().resolve()
    if not source_dir.is_dir():
        raise SystemExit(f"HF cache directory was not found: {source_dir}")

    destination = output_root / "hf-cache"
    if destination.exists():
        shutil.rmtree(destination)
    copy_tree_contents(source_dir, destination)
    manifest["hf-cache"] = {
        **summarize_directory(destination),
        "source": str(source_dir),
    }


def main() -> int:
    args = parse_args()
    output_root = Path(args.output_dir).expanduser().resolve()

    if args.clean and output_root.exists():
        shutil.rmtree(output_root)
    output_root.mkdir(parents=True, exist_ok=True)

    manifest: dict[str, object] = {
        "output_dir": str(output_root),
        "cwd": os.getcwd(),
    }

    stage_python_runtime(
        args.macos_python_runtime_dir,
        output_root,
        "python-macos",
        "macos",
        manifest,
    )
    stage_python_runtime(
        args.windows_python_runtime_dir,
        output_root,
        "python-win",
        "windows",
        manifest,
    )
    stage_model_repos(args.model_repo_dir, output_root, manifest)
    stage_hf_cache(args.hf_cache_dir, output_root, manifest)

    manifest_path = output_root / "asset-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    print(f"Prepared release assets under {output_root}")
    print(f"Manifest: {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
