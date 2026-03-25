#!/usr/bin/env python3
"""Run a lightweight packaging smoke for the shared stage helper and wrappers."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run Z-Soda packaging smoke checks against the shared staging contract."
    )
    parser.add_argument(
        "--windows-build-dir",
        default="build-origin-main-ae",
        help="Windows build directory that contains plugin/Release/ZSoda.aex.",
    )
    parser.add_argument(
        "--output-dir",
        default="artifacts/packaging-smoke",
        help="Smoke output root.",
    )
    return parser.parse_args()


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def run(command: list[str], *, cwd: Path, label: str) -> None:
    print(f"[RUN] {label}", flush=True)
    completed = subprocess.run(command, cwd=cwd, check=False)
    if completed.returncode != 0:
        raise RuntimeError(f"{label} failed with exit code {completed.returncode}")


def write_fake_assets(root: Path) -> dict[str, Path]:
    fake_windows_python = root / "fake-package-assets" / "python-win"
    fake_windows_models = root / "fake-package-assets" / "models" / "distill-any-depth-base"
    fake_sidecar_models_root = root / "fake-sidecar-assets" / "models"
    fake_sidecar_models = fake_sidecar_models_root / "distill-any-depth"
    fake_sidecar_ort = root / "fake-sidecar-assets" / "zsoda_ort"
    fake_mac_build = root / "fake-mac-stage" / "build-mac" / "plugin" / "Release" / "ZSoda.plugin"
    fake_mac_python = root / "fake-mac-stage" / "python-macos" / "bin"
    fake_mac_models = root / "fake-mac-stage" / "models" / "distill-any-depth-base"

    fake_windows_python.mkdir(parents=True, exist_ok=True)
    fake_windows_models.mkdir(parents=True, exist_ok=True)
    fake_sidecar_models.mkdir(parents=True, exist_ok=True)
    fake_sidecar_ort.mkdir(parents=True, exist_ok=True)
    fake_mac_build.mkdir(parents=True, exist_ok=True)
    fake_mac_python.mkdir(parents=True, exist_ok=True)
    fake_mac_models.mkdir(parents=True, exist_ok=True)

    (fake_windows_python / "python.exe").write_bytes(b"")
    (fake_windows_models / "config.json").write_text('{"kind":"stub-model"}\n', encoding="utf-8")
    (fake_windows_models / "model.safetensors").write_text("stub-weights\n", encoding="utf-8")
    (fake_sidecar_models_root / "models.manifest").write_text(
        "# id|display_name|relative_path|download_url|preferred_default\n"
        "distill-any-depth-base|DistillAnyDepth Base|distill-any-depth/distill_any_depth_base.onnx|https://example.com/distill_any_depth_base.onnx|true\n",
        encoding="utf-8",
    )
    (fake_sidecar_models / "distill_any_depth_base.onnx").write_text("stub-onnx\n", encoding="utf-8")
    (fake_sidecar_ort / "onnxruntime.dll").write_text("stub-ort\n", encoding="utf-8")
    (fake_sidecar_ort / "onnxruntime_providers_shared.dll").write_text(
        "stub-ort-provider\n",
        encoding="utf-8",
    )
    (fake_sidecar_ort / "DirectML.dll").write_text("stub-directml\n", encoding="utf-8")

    (fake_mac_python / "python3").write_text("stub\n", encoding="utf-8")
    (fake_mac_models / "config.json").write_text('{"kind":"stub-model"}\n', encoding="utf-8")
    (fake_mac_models / "model.safetensors").write_text("stub-weights\n", encoding="utf-8")

    return {
        "windows_python": fake_windows_python,
        "windows_models_root": fake_windows_models.parent,
        "sidecar_model_root": fake_sidecar_models_root,
        "sidecar_ort_root": fake_sidecar_ort,
        "sidecar_ort_dll": fake_sidecar_ort / "onnxruntime.dll",
        "mac_stage_root": fake_mac_build.parent.parent.parent.parent,
    }


def assert_zip_contains(zip_path: Path, required_entries: list[str]) -> None:
    with zipfile.ZipFile(zip_path, "r") as archive:
        names = set(archive.namelist())
    missing_entries = [entry for entry in required_entries if entry not in names]
    if missing_entries:
        raise FileNotFoundError(
            f"{zip_path} is missing expected archive entries: {', '.join(missing_entries)}"
        )


def assert_zip_excludes(zip_path: Path, forbidden_entries: list[str]) -> None:
    with zipfile.ZipFile(zip_path, "r") as archive:
        names = set(archive.namelist())
    present_entries = [entry for entry in forbidden_entries if entry in names]
    if present_entries:
        raise RuntimeError(
            f"{zip_path} unexpectedly contains flat-layout entries: {', '.join(present_entries)}"
        )


def main() -> int:
    args = parse_args()
    root = repo_root()
    smoke_root = (root / args.output_dir).resolve()
    if smoke_root.exists():
        shutil.rmtree(smoke_root)
    smoke_root.mkdir(parents=True, exist_ok=True)

    windows_build_dir = (root / args.windows_build_dir).resolve()
    if not (windows_build_dir / "plugin" / "Release" / "ZSoda.aex").is_file():
        raise FileNotFoundError(
            f"Windows build artifact not found under {windows_build_dir / 'plugin' / 'Release' / 'ZSoda.aex'}"
        )

    fake_assets = write_fake_assets(smoke_root)

    run(
        [
            sys.executable,
            "-m",
            "py_compile",
            "tools/package_layout.py",
            "tools/prepare_package_stage.py",
            "tools/check_release_readiness.py",
        ],
        cwd=root,
        label="py_compile shared packaging tools",
    )

    run(
        [
            sys.executable,
            "tools/prepare_package_stage.py",
            "--platform",
            "windows",
            "--build-dir",
            str(windows_build_dir),
            "--output-dir",
            str(smoke_root / "windows-stage"),
            "--include-manifest",
            "--python-runtime-dir",
            str(fake_assets["windows_python"]),
            "--model-repo-dir",
            str(fake_assets["windows_models_root"]),
            "--require-self-contained",
            "--quiet",
            "--plan-out",
            str(smoke_root / "windows-stage" / "plan.json"),
        ],
        cwd=root,
        label="shared stage helper windows smoke",
    )
    windows_stage_plan = json.loads(
        (smoke_root / "windows-stage" / "plan.json").read_text(encoding="utf-8")
    )
    if windows_stage_plan.get("package_root_dir_name", "") != "":
        raise RuntimeError("Embedded Windows stage plan should not set package_root_dir_name.")

    run(
        [
            sys.executable,
            "tools/prepare_package_stage.py",
            "--platform",
            "macos",
            "--build-dir",
            str(fake_assets["mac_stage_root"] / "build-mac"),
            "--output-dir",
            str(smoke_root / "mac-stage"),
            "--python-runtime-dir",
            str(fake_assets["mac_stage_root"] / "python-macos"),
            "--model-repo-dir",
            str(fake_assets["mac_stage_root"] / "models"),
            "--require-self-contained",
            "--quiet",
            "--plan-out",
            str(smoke_root / "mac-stage" / "plan.json"),
        ],
        cwd=root,
        label="shared stage helper macOS smoke",
    )

    run(
        [
            "powershell",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            ".\\tools\\package_plugin.ps1",
            "-Platform",
            "windows",
            "-BuildDir",
            str(windows_build_dir),
            "-OutputDir",
            str(smoke_root / "windows-package"),
            "-IncludeManifest",
            "-PythonRuntimeDir",
            str(fake_assets["windows_python"]),
            "-ModelRepoDir",
            str(fake_assets["windows_models_root"]),
        ],
        cwd=root,
        label="windows packager smoke",
    )

    run(
        [
            sys.executable,
            "tools/prepare_package_stage.py",
            "--platform",
            "windows",
            "--package-mode",
            "sidecar-ort",
            "--build-dir",
            str(windows_build_dir),
            "--output-dir",
            str(smoke_root / "windows-sidecar-stage"),
            "--include-manifest",
            "--model-root-dir",
            str(fake_assets["sidecar_model_root"]),
            "--ort-runtime-dir",
            str(fake_assets["sidecar_ort_root"]),
            "--require-self-contained",
            "--quiet",
            "--plan-out",
            str(smoke_root / "windows-sidecar-stage" / "plan.json"),
        ],
        cwd=root,
        label="shared stage helper windows sidecar smoke",
    )
    windows_sidecar_stage_plan = json.loads(
        (smoke_root / "windows-sidecar-stage" / "plan.json").read_text(encoding="utf-8")
    )
    if windows_sidecar_stage_plan.get("package_root_dir_name") != "Z-Soda":
        raise RuntimeError("Sidecar ORT stage plan should set package_root_dir_name to 'Z-Soda'.")

    run(
        [
            "powershell",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            ".\\tools\\package_plugin.ps1",
            "-Platform",
            "windows",
            "-PackageMode",
            "sidecar-ort",
            "-BuildDir",
            str(windows_build_dir),
            "-OutputDir",
            str(smoke_root / "windows-sidecar-package"),
            "-IncludeManifest",
            "-ModelRootDir",
            str(fake_assets["sidecar_model_root"]),
            "-OrtRuntimeDllPath",
            str(fake_assets["sidecar_ort_dll"]),
            "-RequireSelfContained",
            "-RequireOrtRuntimeDll",
        ],
        cwd=root,
        label="windows sidecar ORT packager smoke",
    )

    bash = shutil.which("bash")
    if bash is not None:
        run(
            [bash, "-n", "tools/package_plugin.sh"],
            cwd=root,
            label="bash syntax check package_plugin.sh",
        )
    else:
        print("[SKIP] bash syntax check package_plugin.sh (bash not found)", flush=True)

    packaged_zip = smoke_root / "windows-package" / "ZSoda-windows.zip"
    if not packaged_zip.is_file():
        raise FileNotFoundError(f"Expected smoke package was not created: {packaged_zip}")
    assert_zip_contains(packaged_zip, ["ZSoda.aex"])

    sidecar_packaged_zip = smoke_root / "windows-sidecar-package" / "ZSoda-windows.zip"
    if not sidecar_packaged_zip.is_file():
        raise FileNotFoundError(
            f"Expected sidecar smoke package was not created: {sidecar_packaged_zip}"
        )
    assert_zip_contains(
        sidecar_packaged_zip,
        [
            "Z-Soda/ZSoda.aex",
            "Z-Soda/models/models.manifest",
            "Z-Soda/models/distill-any-depth/distill_any_depth_base.onnx",
            "Z-Soda/zsoda_ort/onnxruntime.dll",
            "Z-Soda/zsoda_ort/onnxruntime_providers_shared.dll",
            "Z-Soda/zsoda_ort/DirectML.dll",
        ],
    )
    assert_zip_excludes(
        sidecar_packaged_zip,
        [
            "ZSoda.aex",
            "models/models.manifest",
            "zsoda_ort/onnxruntime.dll",
        ],
    )

    print("[OK] packaging smoke completed", flush=True)
    print(f"      windows package: {packaged_zip}", flush=True)
    print(f"      windows sidecar package: {sidecar_packaged_zip}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
