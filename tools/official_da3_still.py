#!/usr/bin/env python3

from __future__ import annotations

import argparse
import importlib.util
import json
import sys
import time
import traceback
import types
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run official Depth Anything 3 still-image inference and export depth artifacts."
    )
    parser.add_argument("--input", required=True, help="Input image path")
    parser.add_argument("--output-dir", required=True, help="Output directory")
    parser.add_argument(
        "--repo-root",
        default=None,
        help="Depth-Anything-3 repository root (defaults to .tmp_external_research/Depth-Anything-3)",
    )
    parser.add_argument(
        "--model-repo",
        default="depth-anything/DA3-LARGE-1.1",
        help="Hugging Face model repo or local pretrained directory",
    )
    parser.add_argument("--process-res", type=int, default=504, help="Official process_res value")
    parser.add_argument(
        "--process-res-method",
        default="upper_bound_resize",
        help="Official process_res_method value",
    )
    parser.add_argument(
        "--reference-view-strategy",
        default="middle",
        help="Reference view strategy for multi-view capable checkpoints",
    )
    parser.add_argument(
        "--device",
        default="auto",
        choices=("auto", "cpu", "cuda"),
        help="Torch execution device",
    )
    return parser.parse_args()


def resolve_repo_root(args: argparse.Namespace) -> Path:
    if args.repo_root:
        return Path(args.repo_root).resolve()
    return Path(__file__).resolve().parents[1] / ".tmp_external_research" / "Depth-Anything-3"


def resolve_device(torch_module, requested: str) -> str:
    if requested == "auto":
        return "cuda" if torch_module.cuda.is_available() else "cpu"
    if requested == "cuda" and not torch_module.cuda.is_available():
        raise RuntimeError("CUDA device requested, but torch.cuda.is_available() is false")
    return requested


def missing_repo_dependencies() -> list[str]:
    # This wrapper stubs export/pose-align modules and only requires the
    # minimal packages needed for still-image inference.
    required_modules = [
        "addict",
        "einops",
        "imageio",
    ]
    return [name for name in required_modules if importlib.util.find_spec(name) is None]


def install_api_stubs() -> None:
    export_module = types.ModuleType("depth_anything_3.utils.export")

    def export_stub(*_args, **_kwargs):
        raise RuntimeError("Export is not supported by official_da3_still.py")

    export_module.export = export_stub
    sys.modules.setdefault("depth_anything_3.utils.export", export_module)

    pose_align_module = types.ModuleType("depth_anything_3.utils.pose_align")

    def align_poses_umeyama_stub(*_args, **_kwargs):
        raise RuntimeError("Pose alignment is not supported by official_da3_still.py")

    pose_align_module.align_poses_umeyama = align_poses_umeyama_stub
    sys.modules.setdefault("depth_anything_3.utils.pose_align", pose_align_module)


def save_depth_png(depth, output_path: Path, pil_image_module) -> dict[str, float]:
    import numpy as np

    depth = np.asarray(depth, dtype=np.float32)
    finite_mask = np.isfinite(depth)
    stats = {
        "min": 0.0,
        "max": 0.0,
        "mean": 0.0,
        "stddev": 0.0,
    }
    visualization = np.zeros(depth.shape, dtype=np.uint8)
    if finite_mask.any():
        finite_values = depth[finite_mask]
        stats["min"] = float(finite_values.min())
        stats["max"] = float(finite_values.max())
        stats["mean"] = float(finite_values.mean())
        stats["stddev"] = float(finite_values.std())
        lo = float(np.percentile(finite_values, 1.0))
        hi = float(np.percentile(finite_values, 99.0))
        if hi <= lo:
            lo = stats["min"]
            hi = stats["max"]
        scale = max(hi - lo, 1e-6)
        normalized = np.clip((depth - lo) / scale, 0.0, 1.0)
        visualization = (normalized * 255.0 + 0.5).astype(np.uint8)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    pil_image_module.fromarray(visualization, mode="L").save(output_path)
    return stats


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8")


def main() -> int:
    args = parse_args()
    input_path = Path(args.input).resolve()
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    metadata_path = output_dir / "official_da3_metadata.json"

    started = time.perf_counter()
    try:
        repo_root = resolve_repo_root(args)
        src_root = repo_root / "src"
        if not src_root.exists():
            raise FileNotFoundError(f"Depth Anything 3 repo not found: {src_root}")

        sys.path.insert(0, str(src_root))

        import numpy as np
        import torch
        from PIL import Image

        missing = missing_repo_dependencies()
        if missing:
            raise RuntimeError(
                "Missing Depth-Anything-3 runtime dependencies: "
                + ", ".join(missing)
                + ". Install these packages or pass a pre-rendered --official-image."
            )

        install_api_stubs()
        from depth_anything_3.api import DepthAnything3

        device = resolve_device(torch, args.device)
        model = DepthAnything3.from_pretrained(args.model_repo)
        model = model.to(device=device)
        model.eval()

        prediction = model.inference(
            image=[str(input_path)],
            process_res=args.process_res,
            process_res_method=args.process_res_method,
            ref_view_strategy=args.reference_view_strategy,
        )

        depth = np.asarray(prediction.depth[0], dtype=np.float32)
        npy_path = output_dir / "official_da3_depth.npy"
        png_path = output_dir / "official_da3_depth.png"
        processed_path = output_dir / "official_da3_processed_input.png"
        np.save(npy_path, depth)
        stats = save_depth_png(depth, png_path, Image)

        processed_image_written = False
        if getattr(prediction, "processed_images", None) is not None:
            processed = np.asarray(prediction.processed_images[0])
            if processed.ndim == 3:
                processed = np.clip(processed, 0.0, 255.0)
                if processed.dtype != np.uint8:
                    if processed.max() <= 1.0:
                        processed = processed * 255.0
                    processed = processed.astype(np.uint8)
                Image.fromarray(processed).save(processed_path)
                processed_image_written = True

        elapsed_ms = (time.perf_counter() - started) * 1000.0
        metadata = {
            "tool": "official_da3_still",
            "input": str(input_path),
            "repo_root": str(repo_root),
            "model_repo": args.model_repo,
            "device": device,
            "process_res": args.process_res,
            "process_res_method": args.process_res_method,
            "reference_view_strategy": args.reference_view_strategy,
            "elapsed_ms": elapsed_ms,
            "artifacts": {
                "depth_png": str(png_path),
                "depth_npy": str(npy_path),
                "processed_input_png": str(processed_path) if processed_image_written else "",
            },
            "stats": stats,
        }
        write_json(metadata_path, metadata)
        print(json.dumps(metadata, ensure_ascii=False))
        return 0
    except Exception as exc:  # noqa: BLE001
        elapsed_ms = (time.perf_counter() - started) * 1000.0
        error_payload = {
            "tool": "official_da3_still",
            "input": str(input_path),
            "error": str(exc),
            "elapsed_ms": elapsed_ms,
            "traceback": traceback.format_exc(),
        }
        write_json(metadata_path, error_payload)
        print(error_payload["traceback"], file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
