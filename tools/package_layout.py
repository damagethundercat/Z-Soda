from __future__ import annotations

import shutil
from dataclasses import asdict, dataclass
from pathlib import Path

SELF_CONTAINED_ROOTS = ("models", "zsoda_py", "zsoda_ort")
PACKAGE_MODE_EMBEDDED_WINDOWS = "embedded-windows"
PACKAGE_MODE_SIDECAR_ORT = "sidecar-ort"
WINDOWS_SIDECAR_PACKAGE_ROOT = "Z-Soda"
SERVICE_SCRIPT_RELATIVE_PATH = Path("tools") / "distill_any_depth_remote_service.py"
WINDOWS_BUNDLED_PYTHON_CANDIDATES = (
    "python.exe",
    "python/python.exe",
    "runtime/python.exe",
)
MACOS_BUNDLED_PYTHON_CANDIDATES = (
    "bin/python3",
    "python/bin/python3",
    "runtime/bin/python3",
)


@dataclass
class PackageStagePlan:
    platform: str
    package_mode: str
    artifact_source: str
    artifact_name: str
    archive_name: str
    output_dir: str
    payload_stage_dir: str
    package_resource_subdir: str
    package_root_dir_name: str
    staged_roots: list[str]
    staged_root_paths: dict[str, str]
    warnings: list[str]

    def to_json_dict(self) -> dict[str, object]:
        return asdict(self)


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def _resolve_if_dir(path_text: str | None) -> Path | None:
    if not path_text:
        return None
    path = Path(path_text)
    if path.is_dir():
        return path.resolve()
    return None


def _resolve_release_asset_default(platform: str, kind: str) -> Path | None:
    root = repo_root() / "release-assets"
    if kind == "models":
        return (root / "models").resolve() if (root / "models").is_dir() else None
    if kind == "hf-cache":
        return (root / "hf-cache").resolve() if (root / "hf-cache").is_dir() else None
    if kind == "python":
        leaf = "python-win" if platform == "windows" else "python-macos"
        return (root / leaf).resolve() if (root / leaf).is_dir() else None
    raise ValueError(f"unsupported asset kind: {kind}")


def resolve_artifact_path(platform: str, build_dir: Path) -> Path:
    build_dir = build_dir.resolve()
    if platform == "windows":
        candidates = (
            build_dir / "plugin" / "Release" / "ZSoda.aex",
            build_dir / "plugin" / "ZSoda.aex",
        )
        for candidate in candidates:
            if candidate.is_file():
                return candidate.resolve()
        raise FileNotFoundError(
            f"Artifact not found for platform '{platform}' under build dir '{build_dir}'."
        )

    if platform == "macos":
        candidates = (
            build_dir / "plugin" / "Release" / "ZSoda.plugin",
            build_dir / "plugin" / "ZSoda.plugin",
        )
        for candidate in candidates:
            if candidate.is_dir():
                return candidate.resolve()
        raise FileNotFoundError(
            f"Artifact not found for platform '{platform}' under build dir '{build_dir}'."
        )

    raise ValueError(f"unsupported platform: {platform}")


def resolve_build_packaged_dir(
    root_name: str,
    *,
    build_dir: Path,
    artifact_path: Path,
) -> Path | None:
    artifact_dir = artifact_path.parent
    candidates = (
        artifact_dir / root_name,
        build_dir / "plugin" / "Release" / root_name,
        build_dir / "plugin" / root_name,
    )
    for candidate in candidates:
        if candidate.is_dir():
            return candidate.resolve()
    return None


def copy_dir_contents(source_dir: Path, destination_dir: Path) -> None:
    destination_dir.mkdir(parents=True, exist_ok=True)
    for entry in source_dir.iterdir():
        target = destination_dir / entry.name
        if entry.is_dir():
            if target.exists():
                shutil.rmtree(target)
            shutil.copytree(entry, target)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(entry, target)


def replace_dir(source_dir: Path, destination_dir: Path) -> None:
    if destination_dir.exists():
        shutil.rmtree(destination_dir)
    destination_dir.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source_dir, destination_dir)


def stage_models_metadata(destination_dir: Path) -> None:
    destination_dir.mkdir(parents=True, exist_ok=True)
    models_root = repo_root() / "models"
    for metadata_name in ("models.manifest", "README.md"):
        source = models_root / metadata_name
        if source.is_file():
            shutil.copy2(source, destination_dir / metadata_name)


def stage_model_repos(source_root: Path, destination_root: Path) -> None:
    destination_root.mkdir(parents=True, exist_ok=True)
    repo_dirs = sorted(entry for entry in source_root.iterdir() if entry.is_dir())
    if not repo_dirs:
        raise FileNotFoundError(
            f"Model repo directory does not contain any model subdirectories: {source_root}"
        )
    for repo_dir in repo_dirs:
        replace_dir(repo_dir, destination_root / repo_dir.name)


def stage_native_model_root(source_root: Path, destination_root: Path) -> None:
    if not source_root.is_dir():
        raise FileNotFoundError(f"Native model root directory was not found: {source_root}")
    copy_dir_contents(source_root, destination_root)


def _bundled_python_candidates(platform: str) -> tuple[str, ...]:
    if platform == "windows":
        return WINDOWS_BUNDLED_PYTHON_CANDIDATES
    return MACOS_BUNDLED_PYTHON_CANDIDATES


def assert_self_contained_payload(stage_root: Path, platform: str) -> None:
    python_stage_dir = stage_root / "zsoda_py"
    service_script = python_stage_dir / "distill_any_depth_remote_service.py"
    if not service_script.is_file():
        raise FileNotFoundError(
            f"Self-contained packaging requires bundled service script: {service_script}"
        )

    python_candidates = [python_stage_dir / candidate for candidate in _bundled_python_candidates(platform)]
    if not any(candidate.is_file() for candidate in python_candidates):
        raise FileNotFoundError(
            "Self-contained packaging requires a bundled Python runtime under zsoda_py/."
        )

    models_hf_dir = stage_root / "models" / "hf"
    if not models_hf_dir.is_dir():
        raise FileNotFoundError(
            "Self-contained packaging requires bundled local model repos under models/hf/."
        )

    repo_dirs = [entry for entry in models_hf_dir.iterdir() if entry.is_dir()]
    if not repo_dirs:
        raise FileNotFoundError(
            "Self-contained packaging requires at least one local model repo under models/hf/."
        )


def assert_sidecar_ort_payload(stage_root: Path) -> None:
    models_dir = stage_root / "models"
    manifest_path = models_dir / "models.manifest"
    ort_dir = stage_root / "zsoda_ort"
    ort_dll_path = ort_dir / "onnxruntime.dll"
    providers_shared_path = ort_dir / "onnxruntime_providers_shared.dll"

    if not manifest_path.is_file():
        raise FileNotFoundError(
            f"Sidecar ORT packaging requires models.manifest under models/: {manifest_path}"
        )

    onnx_files = sorted(models_dir.rglob("*.onnx"))
    if not onnx_files:
        raise FileNotFoundError(
            f"Sidecar ORT packaging requires at least one ONNX model under: {models_dir}"
        )

    if not ort_dll_path.is_file():
        raise FileNotFoundError(
            f"Sidecar ORT packaging requires bundled ORT runtime at: {ort_dll_path}"
        )
    if not providers_shared_path.is_file():
        raise FileNotFoundError(
            "Sidecar ORT packaging requires bundled ORT providers shared DLL at: "
            f"{providers_shared_path}"
        )


def prepare_package_stage(
    *,
    platform: str,
    build_dir: str,
    output_dir: str,
    package_mode: str,
    include_manifest: bool,
    python_runtime_dir: str = "",
    model_repo_dir: str = "",
    model_root_dir: str = "",
    hf_cache_dir: str = "",
    require_self_contained: bool = False,
    ort_runtime_dir: str = "",
) -> PackageStagePlan:
    build_root = Path(build_dir).resolve()
    output_root = Path(output_dir).resolve()
    output_root.mkdir(parents=True, exist_ok=True)

    artifact_path = resolve_artifact_path(platform, build_root)
    payload_stage_dir = output_root / ".payload-stage"
    if payload_stage_dir.exists():
        shutil.rmtree(payload_stage_dir)
    payload_stage_dir.mkdir(parents=True, exist_ok=True)

    if package_mode not in {PACKAGE_MODE_EMBEDDED_WINDOWS, PACKAGE_MODE_SIDECAR_ORT}:
        raise ValueError(f"unsupported package mode: {package_mode}")

    resolved_model_repo_dir = _resolve_if_dir(model_repo_dir)
    if resolved_model_repo_dir is None:
        resolved_model_repo_dir = _resolve_release_asset_default(platform, "models")

    resolved_model_root_dir = _resolve_if_dir(model_root_dir)
    if resolved_model_root_dir is None and package_mode == PACKAGE_MODE_SIDECAR_ORT:
        repo_models_root = repo_root() / "models"
        if repo_models_root.is_dir() and any(repo_models_root.rglob("*.onnx")):
            resolved_model_root_dir = repo_models_root.resolve()

    resolved_hf_cache_dir = _resolve_if_dir(hf_cache_dir)
    if resolved_hf_cache_dir is None:
        resolved_hf_cache_dir = _resolve_release_asset_default(platform, "hf-cache")

    resolved_python_runtime_dir = _resolve_if_dir(python_runtime_dir)
    if resolved_python_runtime_dir is None:
        resolved_python_runtime_dir = _resolve_release_asset_default(platform, "python")

    warnings: list[str] = []

    if package_mode == PACKAGE_MODE_SIDECAR_ORT:
        if model_root_dir and resolved_model_root_dir is None:
            raise FileNotFoundError(f"Native model root directory was not found: {model_root_dir}")
        if resolved_model_root_dir is not None:
            stage_native_model_root(resolved_model_root_dir, payload_stage_dir / "models")
        elif include_manifest and (repo_root() / "models").is_dir():
            stage_models_metadata(payload_stage_dir / "models")
    else:
        if include_manifest and (repo_root() / "models").is_dir():
            stage_models_metadata(payload_stage_dir / "models")

        if model_repo_dir and resolved_model_repo_dir is None:
            raise FileNotFoundError(f"Model repo directory was not found: {model_repo_dir}")
        if resolved_model_repo_dir is not None:
            stage_model_repos(resolved_model_repo_dir, payload_stage_dir / "models" / "hf")

        if hf_cache_dir and resolved_hf_cache_dir is None:
            raise FileNotFoundError(f"HF cache directory was not found: {hf_cache_dir}")
        if resolved_hf_cache_dir is not None:
            replace_dir(resolved_hf_cache_dir, payload_stage_dir / "models" / "hf-cache")

    resolved_ort_runtime_dir = _resolve_if_dir(ort_runtime_dir)
    if resolved_ort_runtime_dir is None:
        resolved_ort_runtime_dir = resolve_build_packaged_dir(
            "zsoda_ort",
            build_dir=build_root,
            artifact_path=artifact_path,
        )
    if resolved_ort_runtime_dir is not None:
        replace_dir(resolved_ort_runtime_dir, payload_stage_dir / "zsoda_ort")
    elif platform == "windows":
        warnings.append(
            "zsoda_ort runtime directory was not resolved. This is expected for the default "
            "DAD-only remote build; local ORT runtime files are optional."
        )

    if package_mode != PACKAGE_MODE_SIDECAR_ORT:
        resolved_build_python_dir = resolve_build_packaged_dir(
            "zsoda_py",
            build_dir=build_root,
            artifact_path=artifact_path,
        )
        if resolved_build_python_dir is not None:
            replace_dir(resolved_build_python_dir, payload_stage_dir / "zsoda_py")

        if python_runtime_dir and resolved_python_runtime_dir is None:
            raise FileNotFoundError(f"Python runtime directory was not found: {python_runtime_dir}")
        if resolved_python_runtime_dir is not None:
            runtime_destination = payload_stage_dir / "zsoda_py" / "python"
            replace_dir(resolved_python_runtime_dir, runtime_destination)

        service_script = repo_root() / SERVICE_SCRIPT_RELATIVE_PATH
        if service_script.is_file():
            (payload_stage_dir / "zsoda_py").mkdir(parents=True, exist_ok=True)
            shutil.copy2(service_script, payload_stage_dir / "zsoda_py" / service_script.name)
        elif require_self_contained and platform in {"windows", "macos"}:
            raise FileNotFoundError(f"Self-contained packaging requires service script: {service_script}")

    if require_self_contained:
        if package_mode == PACKAGE_MODE_SIDECAR_ORT:
            assert_sidecar_ort_payload(payload_stage_dir)
        else:
            assert_self_contained_payload(payload_stage_dir, platform)

    staged_root_paths: dict[str, str] = {}
    for root_name in SELF_CONTAINED_ROOTS:
        staged_path = payload_stage_dir / root_name
        if staged_path.is_dir():
            staged_root_paths[root_name] = str(staged_path.resolve())

    archive_name = "ZSoda-windows.zip" if platform == "windows" else "ZSoda-macos.zip"
    package_resource_subdir = "" if platform == "windows" else "Contents/Resources"
    package_root_dir_name = (
        WINDOWS_SIDECAR_PACKAGE_ROOT
        if platform == "windows" and package_mode == PACKAGE_MODE_SIDECAR_ORT
        else ""
    )
    return PackageStagePlan(
        platform=platform,
        package_mode=package_mode,
        artifact_source=str(artifact_path),
        artifact_name=artifact_path.name,
        archive_name=archive_name,
        output_dir=str(output_root),
        payload_stage_dir=str(payload_stage_dir.resolve()),
        package_resource_subdir=package_resource_subdir,
        package_root_dir_name=package_root_dir_name,
        staged_roots=list(staged_root_paths.keys()),
        staged_root_paths=staged_root_paths,
        warnings=warnings,
    )
