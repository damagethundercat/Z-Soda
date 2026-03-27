#!/usr/bin/env python3
"""Check whether Z-Soda is ready for self-contained release packaging."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import zipfile
from pathlib import Path

from package_layout import (
    MACOS_SIDECAR_REQUIRED_RUNTIME_FILES,
    SELF_CONTAINED_ROOTS,
    WINDOWS_SIDECAR_PACKAGE_ROOT,
    WINDOWS_BUNDLED_PYTHON_CANDIDATES,
    WINDOWS_SIDECAR_REQUIRED_RUNTIME_FILES,
)

PAYLOAD_HEADER_MAGIC = b"ZSODA_PAYLOAD_V1"
PAYLOAD_FOOTER_MAGIC = b"ZSODA_FOOTER_V1"
PAYLOAD_HEADER_STRUCT = struct.Struct("<16sII")
PAYLOAD_ENTRY_STRUCT = struct.Struct("<IQ")
PAYLOAD_FOOTER_STRUCT = struct.Struct("<16sQ32s")


def is_windows_bundled_python_entry(entry_path: str) -> bool:
    normalized = entry_path.replace("\\", "/").strip("/")
    return any(
        normalized == f"zsoda_py/{candidate.replace(chr(92), '/')}"
        for candidate in WINDOWS_BUNDLED_PYTHON_CANDIDATES
    )


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
    parser.add_argument(
        "--inspect-windows-artifact",
        default="",
        help="Inspect a packaged Windows self-contained .aex and validate its embedded payload contract.",
    )
    parser.add_argument(
        "--expected-model-id",
        default="distill-any-depth-base",
        help="Expected bundled model repo id inside the embedded Windows payload.",
    )
    parser.add_argument(
        "--require-self-contained",
        action="store_true",
        help="Require the embedded Windows payload to contain the bundled runtime/model roots.",
    )
    parser.add_argument(
        "--compare-windows-artifact-to-macos-fixture",
        default="",
        help=(
            "Optional known-good macOS self-contained fixture (.zip or .plugin dir) used to "
            "compare the bundled models/layout contract against a packaged Windows .aex."
        ),
    )
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


def describe_runtime_probe(name: str, manifest_entry: dict[str, object]) -> list[str]:
    runtime_probe = manifest_entry.get("runtime_probe")
    if not isinstance(runtime_probe, dict):
        return []

    lines = [f"- {name} runtime probe:"]
    torch_version = runtime_probe.get("torch_version", "")
    cuda_version = runtime_probe.get("cuda_version", None)
    cuda_available = runtime_probe.get("cuda_available", None)
    cuda_device_count = runtime_probe.get("cuda_device_count", None)
    cuda_device_name = runtime_probe.get("cuda_device_name", "")
    probe_error = runtime_probe.get("probe_error", "")
    if probe_error:
        lines.append(f"  - probe error: {probe_error}")
        return lines

    lines.append(f"  - torch: {torch_version or '<missing>'}")
    lines.append(f"  - cuda version: {cuda_version if cuda_version is not None else '<none>'}")
    lines.append(f"  - cuda available: {cuda_available}")
    if cuda_device_count is not None:
        lines.append(f"  - cuda device count: {cuda_device_count}")
    if cuda_device_name:
        lines.append(f"  - cuda device name: {cuda_device_name}")
    return lines


def _read_exact(stream, size: int, hasher: hashlib._Hash | None = None) -> bytes:
    chunk = stream.read(size)
    if len(chunk) != size:
        raise ValueError(f"unexpected end of file while reading {size} bytes")
    if hasher is not None:
        hasher.update(chunk)
    return chunk


def _read_zip_entry_sha256(archive: zipfile.ZipFile, name: str) -> str:
    hasher = hashlib.sha256()
    with archive.open(name, "r") as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            hasher.update(chunk)
    return hasher.hexdigest()


def collect_macos_fixture_entries(
    fixture_path: Path,
) -> tuple[dict[str, str], list[str], dict[str, object]]:
    issues: list[str] = []
    report: dict[str, object] = {}
    entries: dict[str, str] = {}
    roots: set[str] = set()

    if not fixture_path.exists():
        return {}, [f"macOS fixture not found: {fixture_path}"], report

    def maybe_add_entry(relative_path: str, digest: str) -> None:
        normalized = relative_path.replace("\\", "/").strip("/")
        if not normalized:
            return
        root = normalized.split("/", 1)[0]
        if root not in SELF_CONTAINED_ROOTS:
            return
        entries[normalized] = digest
        roots.add(root)

    if fixture_path.is_file():
        if fixture_path.suffix.lower() != ".zip":
            return {}, [f"unsupported macOS fixture file type: {fixture_path.suffix}"], report
        try:
            with zipfile.ZipFile(fixture_path) as archive:
                resource_marker = "/Contents/Resources/"
                for info in archive.infolist():
                    if info.is_dir():
                        continue
                    archive_name = info.filename.replace("\\", "/")
                    marker_index = archive_name.find(resource_marker)
                    if marker_index < 0:
                        continue
                    relative_path = archive_name[marker_index + len(resource_marker) :]
                    maybe_add_entry(relative_path, _read_zip_entry_sha256(archive, info.filename))
        except Exception as exc:  # noqa: BLE001
            return {}, [f"failed to read macOS fixture zip: {exc}"], report
    else:
        if fixture_path.name == "Resources":
            resource_root = fixture_path
        elif (fixture_path / "Contents" / "Resources").is_dir():
            resource_root = fixture_path / "Contents" / "Resources"
        elif (fixture_path / "models").is_dir() or (fixture_path / "zsoda_py").is_dir():
            resource_root = fixture_path
        else:
            return {}, [f"failed to resolve Contents/Resources under macOS fixture: {fixture_path}"], report

        for entry in resource_root.rglob("*"):
            if not entry.is_file():
                continue
            relative_path = entry.relative_to(resource_root).as_posix()
            maybe_add_entry(relative_path, hashlib.sha256(entry.read_bytes()).hexdigest())

    if not entries:
        issues.append(f"macOS fixture does not contain any self-contained resource entries: {fixture_path}")

    report["entry_count"] = len(entries)
    report["roots"] = sorted(roots)
    report["helper_sha256"] = entries.get("zsoda_py/distill_any_depth_remote_service.py", "")
    return entries, issues, report


def collect_windows_embedded_artifact_entries(
    artifact_path: Path,
    *,
    expected_model_id: str,
    require_self_contained: bool,
) -> tuple[dict[str, str], list[str], dict[str, object]]:
    issues: list[str] = []
    report: dict[str, object] = {}
    entries: dict[str, str] = {}

    if not artifact_path.is_file():
        return {}, [f"artifact not found: {artifact_path}"], report

    file_size = artifact_path.stat().st_size
    footer_size = PAYLOAD_FOOTER_STRUCT.size
    if file_size < footer_size:
        return {}, [f"artifact is too small to contain an embedded payload footer: {artifact_path}"], report

    try:
        with artifact_path.open("rb") as stream:
            stream.seek(file_size - footer_size)
            footer_magic, payload_size, payload_digest = PAYLOAD_FOOTER_STRUCT.unpack(
                stream.read(footer_size)
            )
            if footer_magic.rstrip(b"\0") != PAYLOAD_FOOTER_MAGIC:
                return (
                    {},
                    [
                        "embedded payload footer magic mismatch: "
                        f"expected {PAYLOAD_FOOTER_MAGIC.decode('ascii')}, "
                        f"got {footer_magic.rstrip(b'\0').decode('ascii', errors='replace')}"
                    ],
                    report,
                )

            if payload_size == 0 or payload_size > file_size - footer_size:
                return {}, [f"embedded payload footer is inconsistent: payload_size={payload_size}"], report

            payload_offset = file_size - footer_size - payload_size
            stream.seek(payload_offset)

            payload_hasher = hashlib.sha256()
            header_magic = _read_exact(stream, PAYLOAD_HEADER_STRUCT.size, payload_hasher)
            header_name, entry_count, _reserved = PAYLOAD_HEADER_STRUCT.unpack(header_magic)
            if header_name.rstrip(b"\0") != PAYLOAD_HEADER_MAGIC:
                return (
                    {},
                    [
                        "embedded payload header magic mismatch: "
                        f"expected {PAYLOAD_HEADER_MAGIC.decode('ascii')}, "
                        f"got {header_name.rstrip(b'\0').decode('ascii', errors='replace')}"
                    ],
                    report,
                )

            roots: set[str] = set()
            has_service_script = False
            has_python_runtime = False
            has_model_repo_root = False
            has_model_config = False
            has_model_weights = False
            max_relative_path_length = 0
            max_relative_path = ""
            consumed = PAYLOAD_HEADER_STRUCT.size

            for _ in range(entry_count):
                entry_header = _read_exact(stream, PAYLOAD_ENTRY_STRUCT.size, payload_hasher)
                path_length, entry_size = PAYLOAD_ENTRY_STRUCT.unpack(entry_header)
                if path_length == 0 or path_length > 4096:
                    return {}, [f"embedded payload entry path length is invalid: {path_length}"], report

                path_bytes = _read_exact(stream, path_length, payload_hasher)
                try:
                    entry_path = path_bytes.decode("utf-8")
                except UnicodeDecodeError as exc:
                    return {}, [f"embedded payload entry path is not valid UTF-8: {exc}"], report

                if len(entry_path) > max_relative_path_length:
                    max_relative_path_length = len(entry_path)
                    max_relative_path = entry_path

                roots.add(entry_path.split("/", 1)[0])
                if entry_path == "zsoda_py/distill_any_depth_remote_service.py":
                    has_service_script = True
                if is_windows_bundled_python_entry(entry_path):
                    has_python_runtime = True
                if entry_path.startswith(f"models/hf/{expected_model_id}/"):
                    has_model_repo_root = True
                if entry_path == f"models/hf/{expected_model_id}/config.json":
                    has_model_config = True
                if entry_path == f"models/hf/{expected_model_id}/model.safetensors":
                    has_model_weights = True

                entry_hasher = hashlib.sha256()
                remaining = entry_size
                while remaining > 0:
                    chunk = stream.read(min(remaining, 1024 * 1024))
                    if not chunk:
                        return (
                            {},
                            [f"unexpected end of embedded payload data while reading {entry_path}"],
                            report,
                        )
                    payload_hasher.update(chunk)
                    entry_hasher.update(chunk)
                    remaining -= len(chunk)

                entries[entry_path] = entry_hasher.hexdigest()
                consumed += PAYLOAD_ENTRY_STRUCT.size + path_length + entry_size

            if consumed != payload_size:
                return (
                    {},
                    [
                        "embedded payload size mismatch after inspection: "
                        f"consumed={consumed}, expected={payload_size}"
                    ],
                    report,
                )

            if payload_hasher.digest() != payload_digest:
                return {}, ["embedded payload digest mismatch"], report

            sample_local_appdata = os.environ.get("LOCALAPPDATA", r"C:\Users\Default\AppData\Local")
            sample_cache_root = Path(sample_local_appdata) / "ZS" / payload_digest.hex()
            max_absolute_path_length = len(str(sample_cache_root / Path(max_relative_path)))
            safe_cache_root_budget = 259 - max_relative_path_length

            report["entry_count"] = entry_count
            report["payload_digest"] = payload_digest.hex()
            report["sample_cache_root"] = str(sample_cache_root)
            report["max_relative_path_length"] = max_relative_path_length
            report["max_relative_path"] = max_relative_path
            report["max_absolute_path_length"] = max_absolute_path_length
            report["safe_cache_root_budget"] = safe_cache_root_budget
            report["roots"] = sorted(roots)
            report["helper_sha256"] = entries.get("zsoda_py/distill_any_depth_remote_service.py", "")

            if max_absolute_path_length >= 260:
                issues.append(
                    "embedded payload path would exceed MAX_PATH under the default Windows cache root: "
                    f"max_absolute_path_length={max_absolute_path_length}, "
                    f"safe_cache_root_budget={safe_cache_root_budget}, "
                    f"entry={max_relative_path}"
                )

            if require_self_contained:
                required_roots = {"models", "zsoda_py"}
                missing_roots = sorted(required_roots.difference(roots))
                if missing_roots:
                    issues.append("missing embedded payload roots: " + ", ".join(missing_roots))

                if not has_service_script:
                    issues.append(
                        "missing bundled remote service script: zsoda_py/distill_any_depth_remote_service.py"
                    )
                if not has_python_runtime:
                    issues.append("missing bundled Python runtime executable under zsoda_py/")
                if not has_model_repo_root:
                    issues.append(
                        f"missing bundled model repo root under models/hf/{expected_model_id}/"
                    )
                if not has_model_config:
                    issues.append(
                        f"missing bundled model config: models/hf/{expected_model_id}/config.json"
                    )
                if not has_model_weights:
                    issues.append(
                        f"missing bundled model weights: models/hf/{expected_model_id}/model.safetensors"
                    )

    except Exception as exc:  # noqa: BLE001
        return {}, [f"failed to inspect embedded Windows payload: {exc}"], report

    return entries, issues, report


def compare_windows_artifact_to_macos_fixture(
    windows_entries: dict[str, str],
    mac_entries: dict[str, str],
    *,
    expected_model_id: str,
) -> tuple[list[str], list[str]]:
    issues: list[str] = []
    notes: list[str] = []

    mac_roots = {path.split("/", 1)[0] for path in mac_entries}
    windows_roots = {path.split("/", 1)[0] for path in windows_entries}
    missing_roots = sorted(mac_roots.difference(windows_roots))
    if missing_roots:
        issues.append("windows payload is missing mac fixture roots: " + ", ".join(missing_roots))

    model_prefix = f"models/hf/{expected_model_id}/"
    ignored_model_prefixes = (
        f"{model_prefix}.cache/huggingface/download/",
    )
    ignored_model_suffixes = (".metadata",)

    def include_model_contract_entry(path: str) -> bool:
        if not path.startswith(model_prefix):
            return False
        if path.endswith(ignored_model_suffixes):
            return False
        return not any(path.startswith(prefix) for prefix in ignored_model_prefixes)

    mac_model_entries = {path: digest for path, digest in mac_entries.items() if path.startswith(model_prefix)}
    windows_model_entries = {path: digest for path, digest in windows_entries.items() if path.startswith(model_prefix)}
    mac_model_contract_entries = {
        path: digest for path, digest in mac_entries.items() if include_model_contract_entry(path)
    }
    windows_model_contract_entries = {
        path: digest for path, digest in windows_entries.items() if include_model_contract_entry(path)
    }
    if not mac_model_entries:
        issues.append(f"macOS fixture is missing bundled model subtree: {model_prefix}")
    else:
        missing_model_entries = sorted(
            set(mac_model_contract_entries).difference(windows_model_contract_entries)
        )
        unexpected_model_entries = sorted(
            set(windows_model_contract_entries).difference(mac_model_contract_entries)
        )
        mismatched_model_entries = sorted(
            path
            for path in set(mac_model_contract_entries).intersection(windows_model_contract_entries)
            if mac_model_contract_entries[path] != windows_model_contract_entries[path]
        )
        if missing_model_entries:
            issues.append(
                "windows payload is missing mac fixture model entries: "
                + ", ".join(missing_model_entries[:10])
                + (" ..." if len(missing_model_entries) > 10 else "")
            )
        if unexpected_model_entries:
            issues.append(
                "windows payload has unexpected model entries relative to mac fixture: "
                + ", ".join(unexpected_model_entries[:10])
                + (" ..." if len(unexpected_model_entries) > 10 else "")
            )
        if mismatched_model_entries:
            issues.append(
                "windows payload model hashes differ from mac fixture: "
                + ", ".join(mismatched_model_entries[:10])
                + (" ..." if len(mismatched_model_entries) > 10 else "")
            )

    helper_path = "zsoda_py/distill_any_depth_remote_service.py"
    if helper_path not in mac_entries:
        issues.append(f"macOS fixture is missing bundled helper script: {helper_path}")
    if helper_path not in windows_entries:
        issues.append(f"windows payload is missing bundled helper script: {helper_path}")
    if helper_path in mac_entries and helper_path in windows_entries:
        if mac_entries[helper_path] == windows_entries[helper_path]:
            notes.append("helper script matches mac fixture exactly")
        else:
            notes.append("helper script differs from mac fixture (expected if Windows carries a targeted fix)")

    mac_python_entries = [path for path in mac_entries if path.startswith("zsoda_py/python/")]
    windows_python_entries = [path for path in windows_entries if path.startswith("zsoda_py/python/")]
    if not mac_python_entries:
        issues.append("macOS fixture is missing a bundled Python runtime subtree under zsoda_py/python/")
    if not windows_python_entries:
        issues.append("windows payload is missing a bundled Python runtime subtree under zsoda_py/python/")

    notes.append(
        "mac fixture roots: " + (", ".join(sorted(mac_roots)) if mac_roots else "<none>")
    )
    notes.append(
        "windows payload roots: " + (", ".join(sorted(windows_roots)) if windows_roots else "<none>")
    )
    notes.append(
        f"compared model subtree: {model_prefix} ({len(mac_model_contract_entries)} contract entries, "
        f"ignored {len(mac_model_entries) - len(mac_model_contract_entries)} metadata entries)"
    )
    return issues, notes


def inspect_windows_embedded_artifact(
    artifact_path: Path,
    *,
    expected_model_id: str,
    require_self_contained: bool,
    report: dict[str, object] | None = None,
) -> list[str]:
    _entries, issues, collected_report = collect_windows_embedded_artifact_entries(
        artifact_path,
        expected_model_id=expected_model_id,
        require_self_contained=require_self_contained,
    )
    if report is not None:
        report.update(collected_report)
    return issues


def _load_zip_entries(zip_path: Path) -> tuple[set[str], list[str]]:
    if not zip_path.is_file():
        return set(), [f"zip not found: {zip_path}"]

    try:
        with zipfile.ZipFile(zip_path) as archive:
            entries = {
                info.filename.replace("\\", "/").strip("/")
                for info in archive.infolist()
                if not info.is_dir()
            }
    except Exception as exc:  # noqa: BLE001
        return set(), [f"failed to read zip: {exc}"]

    return entries, []


def inspect_windows_sidecar_zip(zip_path: Path) -> list[str]:
    entries, issues = _load_zip_entries(zip_path)
    if issues:
        return issues

    root = WINDOWS_SIDECAR_PACKAGE_ROOT
    required_entries = {
        f"{root}/ZSoda.aex",
        f"{root}/models/models.manifest",
        *(f"{root}/zsoda_ort/{name}" for name in WINDOWS_SIDECAR_REQUIRED_RUNTIME_FILES),
    }
    missing_entries = sorted(path for path in required_entries if path not in entries)
    if missing_entries:
        issues.append("missing Windows sidecar entries: " + ", ".join(missing_entries))

    onnx_entries = sorted(path for path in entries if path.startswith(f"{root}/models/") and path.endswith(".onnx"))
    if not onnx_entries:
        issues.append(f"missing ONNX payload under {root}/models/")

    legacy_flat_entries = sorted(
        path
        for path in entries
        if path in {"ZSoda.aex", "models/models.manifest"}
        or path.startswith("models/")
        or path.startswith("zsoda_ort/")
    )
    if legacy_flat_entries:
        issues.append(
            "found legacy flat Windows zip entries outside Z-Soda/: "
            + ", ".join(legacy_flat_entries[:10])
            + (" ..." if len(legacy_flat_entries) > 10 else "")
        )

    return issues


def inspect_macos_sidecar_zip(zip_path: Path) -> list[str]:
    entries, issues = _load_zip_entries(zip_path)
    if issues:
        return issues

    bundle_root = "ZSoda.plugin/Contents"
    required_entries = {
        f"{bundle_root}/MacOS/ZSoda",
        f"{bundle_root}/Resources/models/models.manifest",
        *(f"{bundle_root}/Resources/zsoda_ort/{name}" for name in MACOS_SIDECAR_REQUIRED_RUNTIME_FILES),
    }
    missing_entries = sorted(path for path in required_entries if path not in entries)
    if missing_entries:
        issues.append("missing macOS sidecar entries: " + ", ".join(missing_entries))

    onnx_entries = sorted(
        path
        for path in entries
        if path.startswith(f"{bundle_root}/Resources/models/") and path.endswith(".onnx")
    )
    if not onnx_entries:
        issues.append(f"missing ONNX payload under {bundle_root}/Resources/models/")

    return issues


def main() -> int:
    args = parse_args()

    if args.inspect_windows_artifact:
        artifact_path = Path(args.inspect_windows_artifact)
        report: dict[str, object] = {}
        windows_entries, issues, report = collect_windows_embedded_artifact_entries(
            artifact_path,
            expected_model_id=args.expected_model_id,
            require_self_contained=args.require_self_contained,
        )
        comparison_notes: list[str] = []
        if not issues and args.compare_windows_artifact_to_macos_fixture:
            mac_fixture_path = Path(args.compare_windows_artifact_to_macos_fixture)
            mac_entries, mac_issues, mac_report = collect_macos_fixture_entries(mac_fixture_path)
            if mac_issues:
                issues.extend(mac_issues)
            else:
                comparison_issues, comparison_notes = compare_windows_artifact_to_macos_fixture(
                    windows_entries,
                    mac_entries,
                    expected_model_id=args.expected_model_id,
                )
                issues.extend(comparison_issues)
                report["mac_fixture_entry_count"] = mac_report.get("entry_count", 0)
                report["mac_fixture_roots"] = mac_report.get("roots", [])
                report["mac_fixture_helper_sha256"] = mac_report.get("helper_sha256", "")

        print("Windows embedded payload inspection")
        print(f"artifact: {artifact_path}")
        if artifact_path.is_file():
            print(f"size: {artifact_path.stat().st_size}")
        if args.compare_windows_artifact_to_macos_fixture:
            print(f"mac fixture: {args.compare_windows_artifact_to_macos_fixture}")

        if issues:
            print("status: FAILED")
            for issue in issues:
                print(f"- {issue}")
            for note in comparison_notes:
                print(f"- note: {note}")
            return 1

        print("status: OK")
        print("- embedded payload footer/header contract is valid")
        if report:
            print(f"- embedded payload entries: {report['entry_count']}")
            print(f"- max relative path length: {report['max_relative_path_length']}")
            print(f"- sample cache root: {report['sample_cache_root']}")
            print(f"- max absolute path length: {report['max_absolute_path_length']}")
        if args.require_self_contained:
            print(f"- bundled model repo present: models/hf/{args.expected_model_id}")
            print("- bundled Python runtime and service script present")
        for note in comparison_notes:
            print(f"- {note}")
        return 0

    build_win = Path(args.build_win)
    build_mac = Path(args.build_mac)
    dist_win = Path(args.dist_win)
    dist_mac = Path(args.dist_mac)

    checks: list[tuple[str, bool, str]] = []

    repo_model_manifest = Path("models") / "models.manifest"
    build_win_aex = build_win / "plugin" / "Release" / "ZSoda.aex"
    build_mac_bundle = build_mac / "plugin" / "Release" / "ZSoda.plugin"
    dist_win_zip = dist_win / "ZSoda-windows.zip"
    dist_mac_zip = dist_mac / "ZSoda-macos.zip"

    windows_sidecar_issues = inspect_windows_sidecar_zip(dist_win_zip) if dist_win_zip.is_file() else [
        f"zip not found: {dist_win_zip}"
    ]
    macos_sidecar_issues = inspect_macos_sidecar_zip(dist_mac_zip) if dist_mac_zip.is_file() else [
        f"zip not found: {dist_mac_zip}"
    ]

    checks.append(("repo model manifest", repo_model_manifest.is_file(), str(repo_model_manifest)))
    checks.append(("built Windows plugin", build_win_aex.is_file(), str(build_win_aex)))
    checks.append(("built macOS plugin", build_mac_bundle.is_dir(), str(build_mac_bundle)))
    checks.append(("packaged Windows zip", dist_win_zip.is_file(), str(dist_win_zip)))
    checks.append(("packaged macOS zip", dist_mac_zip.is_file(), str(dist_mac_zip)))
    checks.append(("Windows sidecar zip layout", not windows_sidecar_issues, str(dist_win_zip)))
    checks.append(("macOS sidecar zip layout", not macos_sidecar_issues, str(dist_mac_zip)))

    ok_count = sum(1 for _, ok, _ in checks if ok)
    total_count = len(checks)

    print("Z-Soda release readiness")
    print(f"score: {ok_count}/{total_count}")
    for label, ok, detail in checks:
        print(status_line(label, ok, detail))

    blockers: list[str] = []
    if not repo_model_manifest.is_file():
        blockers.append("restore repo model metadata under models/models.manifest")
    if not build_win_aex.is_file():
        blockers.append(f"produce a Windows Release build ({build_win_aex})")
    if not build_mac_bundle.is_dir():
        blockers.append(f"produce a macOS Release build ({build_mac_bundle})")
    if not dist_win_zip.is_file():
        blockers.append(f"package the Windows sidecar zip ({dist_win_zip})")
    if not dist_mac_zip.is_file():
        blockers.append(f"package the macOS sidecar zip ({dist_mac_zip})")
    if dist_win_zip.is_file() and windows_sidecar_issues:
        blockers.append("fix Windows sidecar zip layout under Z-Soda/")
    if dist_mac_zip.is_file() and macos_sidecar_issues:
        blockers.append("fix macOS sidecar bundle layout under ZSoda.plugin/Contents")

    if blockers:
        print("\nremaining blockers:")
        for blocker in blockers:
            print(f"- {blocker}")
    else:
        print("\nremaining blockers:")
        print("- none in repository packaging state")
        print("- final external steps still remain: release commit/tag, mac notarization if needed, GitHub Releases upload")

    print("\nWindows sidecar zip summary:")
    if windows_sidecar_issues:
        print("- status: FAILED")
        for issue in windows_sidecar_issues:
            print(f"- {issue}")
    else:
        print("- status: OK")
        print(f"- zip: {dist_win_zip}")
        print(f"- required root: {WINDOWS_SIDECAR_PACKAGE_ROOT}/")

    print("\nmacOS sidecar zip summary:")
    if macos_sidecar_issues:
        print("- status: FAILED")
        for issue in macos_sidecar_issues:
            print(f"- {issue}")
    else:
        print("- status: OK")
        print(f"- zip: {dist_mac_zip}")
        print("- required root: ZSoda.plugin/Contents")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
