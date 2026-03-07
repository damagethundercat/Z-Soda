#!/usr/bin/env python3

from __future__ import annotations

import argparse
import html
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

VIDEO_EXTENSIONS = {
    ".avi",
    ".m4v",
    ".mkv",
    ".mov",
    ".mp4",
    ".mpeg",
    ".mpg",
    ".webm",
    ".wmv",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run Z-Soda still harness and build a side-by-side HTML comparison bundle."
    )
    parser.add_argument("--input", required=True, help="Input image path")
    parser.add_argument("--output-dir", required=True, help="Comparison bundle output directory")
    parser.add_argument("--ffmpeg-path", default="ffmpeg", help="ffmpeg executable used for video frame extraction")
    parser.add_argument(
        "--video-time-seconds",
        type=float,
        default=0.0,
        help="If --input is a video, extract the frame at this timestamp before running still comparison",
    )
    parser.add_argument("--qd3-image", default="", help="QD3 reference depth image")
    parser.add_argument("--official-image", default="", help="Pre-rendered official DA3 depth image")
    parser.add_argument("--skip-official", action="store_true", help="Skip official DA3 execution")
    parser.add_argument("--require-official", action="store_true", help="Fail if official DA3 image is unavailable")
    parser.add_argument("--harness-exe", default="", help="Path to zsoda_still_quality_harness.exe")
    parser.add_argument("--official-python", default=sys.executable, help="Python interpreter for official wrapper")
    parser.add_argument(
        "--official-script",
        default="",
        help="Path to tools/official_da3_still.py (defaults to repo copy)",
    )
    parser.add_argument("--official-repo-root", default="", help="Depth-Anything-3 repo root")
    parser.add_argument(
        "--official-model-repo",
        default="depth-anything/DA3-LARGE-1.1",
        help="Official DA3 Hugging Face repo or local pretrained directory",
    )
    parser.add_argument("--official-process-res", type=int, default=504)
    parser.add_argument("--official-process-res-method", default="upper_bound_resize")
    parser.add_argument("--official-device", default="auto", choices=("auto", "cpu", "cuda"))
    parser.add_argument("--model-root", default="models")
    parser.add_argument("--model-id", default="depth-anything-v3-large-multiview")
    parser.add_argument("--backend", default="auto")
    parser.add_argument("--resize-mode", default="upper_bound_letterbox")
    parser.add_argument("--quality", type=int, default=1)
    parser.add_argument("--mapping-mode", default="v2-style")
    parser.add_argument("--invert", action="store_true")
    parser.add_argument("--guided-low", type=float, default=0.02)
    parser.add_argument("--guided-high", type=float, default=0.98)
    parser.add_argument("--guided-alpha", type=float, default=0.12)
    parser.add_argument("--temporal-alpha", type=float, default=1.0)
    parser.add_argument("--edge-enhancement", type=float, default=0.18)
    parser.add_argument("--tile-size", type=int, default=512)
    parser.add_argument("--overlap", type=int, default=32)
    parser.add_argument("--vram-budget-mb", type=int, default=0)
    parser.add_argument("--raw-visualization", default="minmax", choices=("minmax", "clamp"))
    return parser.parse_args()


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def locate_harness_exe(explicit: str) -> Path:
    if explicit:
        path = Path(explicit).resolve()
        if path.exists():
            return path
        raise FileNotFoundError(f"Harness executable not found: {path}")

    root = repo_root()
    candidates = [
        root / "build-win" / "tools" / "RelWithDebInfo" / "zsoda_still_quality_harness.exe",
        root / "build-win" / "tools" / "Release" / "zsoda_still_quality_harness.exe",
        root / "build-win" / "tools" / "Debug" / "zsoda_still_quality_harness.exe",
        root / "build-win-aex" / "tools" / "RelWithDebInfo" / "zsoda_still_quality_harness.exe",
        root / "build-win-aex" / "tools" / "Release" / "zsoda_still_quality_harness.exe",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise FileNotFoundError("Unable to locate zsoda_still_quality_harness.exe; build the target first.")


def default_official_script(explicit: str) -> Path:
    if explicit:
        return Path(explicit).resolve()
    return repo_root() / "tools" / "official_da3_still.py"


def ensure_parent(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)


def write_text(path: Path, content: str) -> None:
    ensure_parent(path)
    path.write_text(content, encoding="utf-8")


def copy_optional(source: str, destination: Path) -> str:
    if not source:
        return ""
    source_path = Path(source).resolve()
    if not source_path.exists():
        return ""
    ensure_parent(destination)
    shutil.copy2(source_path, destination)
    return str(destination)


def run_command(command: list[str], cwd: Path, stdout_path: Path, stderr_path: Path) -> dict:
    ensure_parent(stdout_path)
    ensure_parent(stderr_path)
    completed = subprocess.run(
        command,
        cwd=str(cwd),
        text=True,
        capture_output=True,
        check=False,
    )
    stdout_path.write_text(completed.stdout, encoding="utf-8")
    stderr_path.write_text(completed.stderr, encoding="utf-8")
    return {
        "command": subprocess.list2cmdline(command),
        "returncode": completed.returncode,
        "stdout_path": str(stdout_path),
        "stderr_path": str(stderr_path),
    }


def is_video_input(path: Path) -> bool:
    return path.suffix.lower() in VIDEO_EXTENSIONS


def extract_video_frame(
    input_path: Path,
    ffmpeg_path: str,
    video_time_seconds: float,
    bundle_dir: Path,
    logs_dir: Path,
) -> tuple[Path, dict]:
    extracted_frame = bundle_dir / "references" / "source_video_frame.png"
    ensure_parent(extracted_frame)
    stdout_path = logs_dir / "video_extract.stdout.txt"
    stderr_path = logs_dir / "video_extract.stderr.txt"
    command = [
        ffmpeg_path,
        "-y",
        "-ss",
        f"{max(0.0, video_time_seconds):.6f}",
        "-i",
        str(input_path),
        "-update",
        "1",
        "-frames:v",
        "1",
        str(extracted_frame),
    ]
    result = run_command(command, repo_root(), stdout_path, stderr_path)
    if result["returncode"] != 0 or not extracted_frame.exists():
        raise RuntimeError("Video frame extraction failed. Check logs in the bundle.")
    result["status"] = "extracted"
    return extracted_frame, result


def load_json_if_exists(path: Path) -> dict | None:
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return None


def rel(base: Path, target: str | Path) -> str:
    target_path = Path(target)
    return os.path.relpath(target_path, base)


def image_card(title: str, image_path: str, subtitle: str) -> str:
    if image_path:
        return (
            f"<section class='card'><h2>{html.escape(title)}</h2>"
            f"<p>{html.escape(subtitle)}</p>"
            f"<img src='{html.escape(image_path)}' alt='{html.escape(title)}'></section>"
        )
    return (
        f"<section class='card missing'><h2>{html.escape(title)}</h2>"
        f"<p>{html.escape(subtitle)}</p><div class='missing-box'>missing</div></section>"
    )


def render_report(bundle_dir: Path, manifest: dict, zsoda_config: dict | None, official_meta: dict | None) -> None:
    zsoda_paths = manifest["artifacts"]["zsoda"]
    reference_paths = manifest["artifacts"]["references"]
    source_meta = manifest.get("source", {})
    source_summary = "single fixed input"
    if source_meta.get("kind") == "video":
        source_summary = (
            f"video input sampled at {float(source_meta.get('video_time_seconds', 0.0)):.3f}s"
        )

    cards = [
        image_card("Source", rel(bundle_dir, zsoda_paths["input_copy"]), "Original input copied from harness"),
        image_card("QD3", rel(bundle_dir, reference_paths["qd3"]) if reference_paths["qd3"] else "", "Quick Depth 3 reference"),
        image_card(
            "Official DA3",
            rel(bundle_dir, reference_paths["official"]) if reference_paths["official"] else "",
            manifest["official"]["status"],
        ),
        image_card(
            "Z-Soda Raw",
            rel(bundle_dir, zsoda_paths["raw_png"]) if zsoda_paths["raw_png"] else "",
            "ManagedInferenceEngine direct output",
        ),
        image_card(
            "Z-Soda Pipeline",
            rel(bundle_dir, zsoda_paths["pipeline_png"]) if zsoda_paths["pipeline_png"] else "",
            manifest["zsoda"]["status"],
        ),
    ]

    manifest_json = json.dumps(manifest, indent=2, ensure_ascii=False)
    zsoda_json = json.dumps(zsoda_config, indent=2, ensure_ascii=False) if zsoda_config else "null"
    official_json = json.dumps(official_meta, indent=2, ensure_ascii=False) if official_meta else "null"

    report = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Z-Soda Still Quality Comparison</title>
  <style>
    :root {{
      color-scheme: light;
      --bg: #f2efe8;
      --panel: #fffaf2;
      --ink: #1f1d1a;
      --muted: #6f665b;
      --line: #d7cebf;
      --accent: #a54522;
    }}
    body {{
      margin: 0;
      padding: 24px;
      background:
        radial-gradient(circle at top left, rgba(165,69,34,0.12), transparent 30%),
        linear-gradient(180deg, #f6f3ed 0%, var(--bg) 100%);
      color: var(--ink);
      font: 15px/1.5 "Segoe UI", sans-serif;
    }}
    h1, h2 {{ margin: 0 0 8px; }}
    p {{ margin: 0; color: var(--muted); }}
    .top {{ margin-bottom: 20px; }}
    .grid {{
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
      gap: 16px;
      margin-bottom: 20px;
    }}
    .card {{
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 16px;
      padding: 14px;
      box-shadow: 0 10px 30px rgba(31, 29, 26, 0.06);
    }}
    .card img {{
      width: 100%;
      display: block;
      margin-top: 12px;
      border-radius: 10px;
      border: 1px solid var(--line);
      background: #0f0f0f;
    }}
    .missing-box {{
      margin-top: 12px;
      padding: 48px 12px;
      border-radius: 10px;
      border: 1px dashed var(--line);
      text-align: center;
      color: var(--muted);
    }}
    .meta {{
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(320px, 1fr));
      gap: 16px;
    }}
    pre {{
      margin: 0;
      white-space: pre-wrap;
      word-break: break-word;
      color: var(--ink);
      background: #f7f2e8;
      border-radius: 12px;
      padding: 12px;
      border: 1px solid var(--line);
    }}
    a {{ color: var(--accent); }}
  </style>
</head>
<body>
  <section class="top">
    <h1>Z-Soda Still Quality Comparison</h1>
    <p>Compare QD3, official DA3, Z-Soda raw inference, and Z-Soda pipeline output from a {html.escape(source_summary)}.</p>
  </section>
  <section class="grid">
    {''.join(cards)}
  </section>
  <section class="meta">
    <section class="card">
      <h2>Bundle Manifest</h2>
      <pre>{html.escape(manifest_json)}</pre>
    </section>
    <section class="card">
      <h2>Z-Soda Resolved Config</h2>
      <pre>{html.escape(zsoda_json)}</pre>
    </section>
    <section class="card">
      <h2>Official DA3 Metadata</h2>
      <pre>{html.escape(official_json)}</pre>
    </section>
  </section>
</body>
</html>
"""
    write_text(bundle_dir / "comparison_report.html", report)


def main() -> int:
    args = parse_args()
    bundle_dir = Path(args.output_dir).resolve()
    bundle_dir.mkdir(parents=True, exist_ok=True)
    zsoda_dir = bundle_dir / "zsoda"
    official_dir = bundle_dir / "official_da3"
    references_dir = bundle_dir / "references"
    logs_dir = bundle_dir / "logs"

    input_path = Path(args.input).resolve()
    resolved_input_path = input_path
    source_info = {
        "kind": "video" if is_video_input(input_path) else "image",
        "original_input": str(input_path),
        "resolved_input": "",
        "video_time_seconds": float(args.video_time_seconds),
    }

    if is_video_input(input_path):
        try:
            resolved_input_path, extraction = extract_video_frame(
                input_path,
                args.ffmpeg_path,
                args.video_time_seconds,
                bundle_dir,
                logs_dir,
            )
            source_info["video_extract"] = extraction
        except RuntimeError as exc:
            print(str(exc), file=sys.stderr)
            return 1

    source_info["resolved_input"] = str(resolved_input_path)

    harness_exe = locate_harness_exe(args.harness_exe)
    harness_stdout = logs_dir / "zsoda_harness.stdout.txt"
    harness_stderr = logs_dir / "zsoda_harness.stderr.txt"
    harness_command = [
        str(harness_exe),
        "--input",
        str(resolved_input_path),
        "--output-dir",
        str(zsoda_dir),
        "--model-root",
        str(Path(args.model_root).resolve()),
        "--model-id",
        args.model_id,
        "--backend",
        args.backend,
        "--resize-mode",
        args.resize_mode,
        "--quality",
        str(args.quality),
        "--mapping-mode",
        args.mapping_mode,
        "--guided-low",
        str(args.guided_low),
        "--guided-high",
        str(args.guided_high),
        "--guided-alpha",
        str(args.guided_alpha),
        "--temporal-alpha",
        str(args.temporal_alpha),
        "--edge-enhancement",
        str(args.edge_enhancement),
        "--tile-size",
        str(args.tile_size),
        "--overlap",
        str(args.overlap),
        "--vram-budget-mb",
        str(args.vram_budget_mb),
        "--raw-visualization",
        args.raw_visualization,
    ]
    if args.invert:
        harness_command.append("--invert")

    manifest = {
        "input": str(input_path),
        "source": source_info,
        "zsoda": run_command(harness_command, repo_root(), harness_stdout, harness_stderr),
        "official": {},
        "artifacts": {
            "zsoda": {
                "input_copy": str(zsoda_dir / "input_source.png"),
                "raw_png": str(zsoda_dir / "zsoda_raw_depth.png"),
                "pipeline_png": str(zsoda_dir / "zsoda_pipeline_depth.png"),
                "resolved_config": str(zsoda_dir / "resolved_config.json"),
            },
            "references": {
                "qd3": copy_optional(args.qd3_image, references_dir / "qd3_reference.png"),
                "official": "",
            },
        },
    }
    manifest["zsoda"]["status"] = (
        "ok" if manifest["zsoda"]["returncode"] == 0 else "failed"
    )

    official_meta = None
    if args.official_image:
        manifest["artifacts"]["references"]["official"] = copy_optional(
            args.official_image, references_dir / "official_da3_reference.png"
        )
        manifest["official"] = {
            "status": "provided image",
            "returncode": 0,
            "command": "",
            "stdout_path": "",
            "stderr_path": "",
        }
    elif not args.skip_official:
        official_stdout = logs_dir / "official_da3.stdout.txt"
        official_stderr = logs_dir / "official_da3.stderr.txt"
        official_script = default_official_script(args.official_script)
        official_command = [
            args.official_python,
            str(official_script),
            "--input",
            str(resolved_input_path),
            "--output-dir",
            str(official_dir),
            "--model-repo",
            args.official_model_repo,
            "--process-res",
            str(args.official_process_res),
            "--process-res-method",
            args.official_process_res_method,
            "--device",
            args.official_device,
        ]
        if args.official_repo_root:
            official_command.extend(["--repo-root", str(Path(args.official_repo_root).resolve())])
        manifest["official"] = run_command(official_command, repo_root(), official_stdout, official_stderr)
        if manifest["official"]["returncode"] == 0:
            manifest["official"]["status"] = "auto-generated"
            official_png = official_dir / "official_da3_depth.png"
            if official_png.exists():
                manifest["artifacts"]["references"]["official"] = str(official_png)
        else:
            manifest["official"]["status"] = "auto-generation failed"
    else:
        manifest["official"] = {
            "status": "skipped",
            "returncode": 0,
            "command": "",
            "stdout_path": "",
            "stderr_path": "",
        }

    resolved_config = load_json_if_exists(zsoda_dir / "resolved_config.json")
    official_meta = load_json_if_exists(official_dir / "official_da3_metadata.json")
    write_text(bundle_dir / "comparison_manifest.json", json.dumps(manifest, indent=2, ensure_ascii=False))
    render_report(bundle_dir, manifest, resolved_config, official_meta)

    if args.require_official and not manifest["artifacts"]["references"]["official"]:
        print("Official DA3 artifact is missing.", file=sys.stderr)
        return 1
    if manifest["zsoda"]["returncode"] != 0:
        print("Z-Soda harness execution failed. Check logs in the bundle.", file=sys.stderr)
        return 1

    print(str(bundle_dir / "comparison_report.html"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
