from __future__ import annotations

import argparse
import shutil
from pathlib import Path


DEFAULT_MODEL_ID = "distill-any-depth-base"
DEFAULT_MODEL_DISPLAY_NAME = "DistillAnyDepth Base"
DEFAULT_MODEL_RELATIVE_PATH = Path("distill-any-depth") / "distill_any_depth_base.onnx"
WINDOWS_REQUIRED_RUNTIME_FILES = (
    "onnxruntime.dll",
    "onnxruntime_providers_shared.dll",
)
WINDOWS_OPTIONAL_RUNTIME_FILES = (
    "DirectML.dll",
)
MACOS_REQUIRED_RUNTIME_FILES = (
    "libonnxruntime.dylib",
)
MACOS_RUNTIME_GLOBS = (
    "libonnxruntime*.dylib*",
)


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def copy_file(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def write_models_manifest(
    *,
    destination: Path,
    model_id: str,
    display_name: str,
    relative_path: str,
) -> None:
    lines = [
        "# id|display_name|relative_path|download_url|preferred_default|auxiliary_assets(relative_path::download_url;...)",
        f"{model_id}|{display_name}|{relative_path}|bundled://{model_id}|true",
    ]
    destination.write_text("\n".join(lines) + "\n", encoding="utf-8")


def prepare_windows_runtime(runtime_dir: Path, destination_dir: Path) -> list[Path]:
    copied: list[Path] = []
    for filename in WINDOWS_REQUIRED_RUNTIME_FILES:
        source = runtime_dir / filename
        if not source.is_file():
            raise FileNotFoundError(f"Required ORT runtime file was not found: {source}")
        destination = destination_dir / filename
        copy_file(source, destination)
        copied.append(destination)

    for filename in WINDOWS_OPTIONAL_RUNTIME_FILES:
        source = runtime_dir / filename
        if source.is_file():
            destination = destination_dir / filename
            copy_file(source, destination)
            copied.append(destination)
    return copied


def prepare_macos_runtime(runtime_dir: Path, destination_dir: Path) -> list[Path]:
    missing = [runtime_dir / filename for filename in MACOS_REQUIRED_RUNTIME_FILES if not (runtime_dir / filename).is_file()]
    if missing:
        raise FileNotFoundError(
            "Required macOS ORT runtime file was not found: " + ", ".join(str(path) for path in missing)
        )

    copied: list[Path] = []
    seen_names: set[str] = set()
    for pattern in MACOS_RUNTIME_GLOBS:
        for source in sorted(runtime_dir.glob(pattern)):
            if not source.is_file() or source.name in seen_names:
                continue
            destination = destination_dir / source.name
            copy_file(source, destination)
            copied.append(destination)
            seen_names.add(source.name)

    if not copied:
        raise FileNotFoundError(
            f"macOS ORT runtime directory does not contain any libonnxruntime*.dylib files: {runtime_dir}"
        )
    return copied


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Prepare a native ORT sidecar release asset directory."
    )
    parser.add_argument("--platform", choices=("windows", "macos"), default="windows")
    parser.add_argument("--onnx-model-path", required=True)
    parser.add_argument("--ort-runtime-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--model-id", default=DEFAULT_MODEL_ID)
    parser.add_argument("--model-display-name", default=DEFAULT_MODEL_DISPLAY_NAME)
    parser.add_argument(
        "--model-relative-path",
        default=str(DEFAULT_MODEL_RELATIVE_PATH).replace("\\", "/"),
    )
    parser.add_argument("--readme-template", default=str(repo_root() / "models" / "README.md"))
    parser.add_argument("--overwrite", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    model_source = Path(args.onnx_model_path).resolve()
    runtime_source = Path(args.ort_runtime_dir).resolve()
    output_root = Path(args.output_dir).resolve()
    readme_template = Path(args.readme_template).resolve()

    if not model_source.is_file():
        raise FileNotFoundError(f"ONNX model was not found: {model_source}")
    if not runtime_source.is_dir():
        raise FileNotFoundError(f"ORT runtime directory was not found: {runtime_source}")
    if readme_template.exists() and not readme_template.is_file():
        raise FileNotFoundError(f"README template is not a file: {readme_template}")

    if output_root.exists():
        if not args.overwrite:
            raise FileExistsError(
                f"Output directory already exists: {output_root} (pass --overwrite to replace it)"
            )
        shutil.rmtree(output_root)

    models_root = output_root / "models"
    runtime_root = output_root / "zsoda_ort"
    model_relative_path = Path(args.model_relative_path)
    model_destination = models_root / model_relative_path
    manifest_destination = models_root / "models.manifest"

    copy_file(model_source, model_destination)
    write_models_manifest(
        destination=manifest_destination,
        model_id=args.model_id,
        display_name=args.model_display_name,
        relative_path=args.model_relative_path.replace("\\", "/"),
    )
    if readme_template.is_file():
        copy_file(readme_template, models_root / "README.md")

    if args.platform == "macos":
        copied_runtime_files = prepare_macos_runtime(runtime_source, runtime_root)
    else:
        copied_runtime_files = prepare_windows_runtime(runtime_source, runtime_root)

    print("Prepared ORT sidecar assets:")
    print(f"  platform: {args.platform}")
    print(f"  output: {output_root}")
    print(f"  model:  {model_destination} ({model_destination.stat().st_size} bytes)")
    print(f"  manifest: {manifest_destination}")
    for runtime_file in copied_runtime_files:
        print(f"  runtime: {runtime_file.name} ({runtime_file.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
