#!/usr/bin/env python3
"""Persistent local HTTP service for DistillAnyDepth remote inference."""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time
import traceback
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any


QUALITY_TO_PROCESS_RES = {
    1: 256,
    2: 512,
    3: 768,
    4: 1024,
    5: 1280,
    6: 1536,
    7: 1920,
    8: 2048,
}

MODEL_SPECS = {
    "distill-any-depth": {
        "repo_env": "ZSODA_DAD_MODEL_SMALL",
        "default_repo": "xingyang1/Distill-Any-Depth-Small-hf",
    },
    "distill-any-depth-base": {
        "repo_env": "ZSODA_DAD_MODEL_BASE",
        "default_repo": "lc700x/Distill-Any-Depth-Base-hf",
    },
    "distill-any-depth-large": {
        "repo_env": "ZSODA_DAD_MODEL_LARGE",
        "default_repo": "xingyang1/Distill-Any-Depth-Large-hf",
    },
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Persistent local HTTP service for DistillAnyDepth inference."
    )
    parser.add_argument("--host", default=None, help="Override bind host")
    parser.add_argument("--port", type=int, default=None, help="Override bind port")
    parser.add_argument(
        "--repo-root",
        default=None,
        help="Compatibility flag accepted for RemoteInferenceBackend auto-start parity.",
    )
    parser.add_argument(
        "--preload-model-id",
        default=None,
        help="Optional model id to preload on service start.",
    )
    return parser.parse_args()


def resolve_model_repo(model_id: str) -> str:
    spec = MODEL_SPECS.get(model_id)
    if spec is None:
        raise RuntimeError(
            f"unsupported model_id for DistillAnyDepth service: {model_id!r}. "
            f"Supported ids: {', '.join(sorted(MODEL_SPECS))}"
        )
    configured = os.getenv(spec["repo_env"], "").strip()
    if configured:
        return configured
    return spec["default_repo"]


def resolve_process_res(payload: dict[str, Any]) -> int:
    explicit = payload.get("process_res")
    if isinstance(explicit, int) and explicit > 0:
        return explicit
    quality = payload.get("quality", 2)
    if isinstance(quality, int) and quality in QUALITY_TO_PROCESS_RES:
        return QUALITY_TO_PROCESS_RES[quality]
    return QUALITY_TO_PROCESS_RES[2]


def resolve_resize_mode(payload: dict[str, Any]) -> str:
    resize_mode = str(payload.get("resize_mode", "upper_bound_letterbox")).strip().lower()
    if resize_mode in {"upper_bound_letterbox", "lower_bound_center_crop", "stretch"}:
        return resize_mode
    return "upper_bound_letterbox"


def resolve_device(requested: str, torch_module) -> str:
    normalized = requested.strip().lower()
    if normalized in {"", "auto"}:
        return "cuda" if torch_module.cuda.is_available() else "cpu"
    if normalized == "cuda" and not torch_module.cuda.is_available():
        raise RuntimeError("CUDA device requested, but torch.cuda.is_available() is false")
    return normalized


def rounded_model_input_size(process_res: int) -> int:
    if process_res <= 0:
        process_res = QUALITY_TO_PROCESS_RES[2]
    return max(14, int(round(process_res / 14.0)) * 14)


def bicubic_resample() -> int:
    try:
        from PIL import Image  # type: ignore

        return Image.Resampling.BICUBIC
    except AttributeError:
        from PIL import Image  # type: ignore

        return Image.BICUBIC


def resize_image_for_process_res(pil_image, process_res: int, resize_mode: str):
    width, height = pil_image.size
    if width <= 0 or height <= 0:
        raise RuntimeError("source image has invalid size")
    resample = bicubic_resample()

    if resize_mode == "stretch":
        target_size = rounded_model_input_size(process_res)
        return pil_image.resize((target_size, target_size), resample)

    longest = max(width, height)
    if longest <= 0 or process_res <= 0:
        return pil_image
    scale = float(process_res) / float(longest)
    resized_width = max(1, int(round(width * scale)))
    resized_height = max(1, int(round(height * scale)))
    if resized_width == width and resized_height == height:
        return pil_image
    return pil_image.resize((resized_width, resized_height), resample)


class ModelManager:
    def __init__(self) -> None:
        self.lock = threading.RLock()
        self.model_repo = ""
        self.device = ""
        self.processor = None
        self.model = None
        self.loaded_model_id = ""
        self.loaded_at = 0.0
        self.transformers = None
        self.torch = None
        self.image_module = None

    def _ensure_imports(self) -> None:
        if self.transformers is not None:
            return
        import torch  # type: ignore
        from PIL import Image  # type: ignore
        from transformers import AutoImageProcessor, AutoModelForDepthEstimation  # type: ignore

        self.transformers = {
            "AutoImageProcessor": AutoImageProcessor,
            "AutoModelForDepthEstimation": AutoModelForDepthEstimation,
        }
        self.torch = torch
        self.image_module = Image

    def get_model(self, model_id: str):
        requested_device = (
            os.getenv("ZSODA_DAD_SERVICE_DEVICE", "").strip()
            or os.getenv("ZSODA_REMOTE_SERVICE_DEVICE", "").strip()
            or "auto"
        )
        with self.lock:
            self._ensure_imports()
            assert self.torch is not None
            assert self.transformers is not None

            device = resolve_device(requested_device, self.torch)
            model_repo = resolve_model_repo(model_id)
            if (
                self.model is None
                or self.loaded_model_id != model_id
                or self.model_repo != model_repo
                or self.device != device
            ):
                processor_cls = self.transformers["AutoImageProcessor"]
                model_cls = self.transformers["AutoModelForDepthEstimation"]
                self.processor = processor_cls.from_pretrained(model_repo, use_fast=True)
                self.model = model_cls.from_pretrained(model_repo)
                self.model = self.model.to(device=device)
                self.model.eval()
                self.loaded_model_id = model_id
                self.model_repo = model_repo
                self.device = device
                self.loaded_at = time.time()
            return self.processor, self.model

    def status(self) -> dict[str, Any]:
        return {
            "loaded": self.model is not None,
            "loaded_model_id": self.loaded_model_id,
            "model_repo": self.model_repo,
            "device": self.device,
            "loaded_at": self.loaded_at,
        }

    def preload(self, model_id: str) -> None:
        if not model_id:
            return
        self.get_model(model_id)


STATE = ModelManager()


def build_error_response(message: str, *, detail: str = "") -> dict[str, Any]:
    payload: dict[str, Any] = {"ok": False, "error": message}
    if detail:
        payload["detail"] = detail
    return payload


def parse_positive_int_header(headers, name: str) -> int:
    raw = headers.get(name, "").strip()
    if not raw:
        raise RuntimeError(f"missing required header: {name}")
    try:
        value = int(raw)
    except ValueError as exc:
        raise RuntimeError(f"invalid integer header {name}: {raw!r}") from exc
    if value <= 0:
        raise RuntimeError(f"header {name} must be > 0")
    return value


def resolve_preload_model_id(args: argparse.Namespace) -> str:
    candidates = [
        args.preload_model_id,
        os.getenv("ZSODA_DAD_PRELOAD_MODEL_ID", "").strip(),
        os.getenv("ZSODA_LOCKED_MODEL_ID", "").strip(),
        "distill-any-depth-base",
    ]
    for candidate in candidates:
        if candidate:
            return candidate
    return "distill-any-depth-base"


class Handler(BaseHTTPRequestHandler):
    server_version = "ZSodaDistillAnyDepth/0.1"

    def log_message(self, format: str, *args) -> None:  # noqa: A003
        timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
        sys.stdout.write(f"[{timestamp}] {self.address_string()} {format % args}\n")
        sys.stdout.flush()

    def do_GET(self) -> None:  # noqa: N802
        if self.path == "/status":
            self._write_json(HTTPStatus.OK, {"ok": True, "status": STATE.status()})
            return
        self._write_json(HTTPStatus.NOT_FOUND, build_error_response("not_found"))

    def do_POST(self) -> None:  # noqa: N802
        if self.path != "/zsoda/depth":
            self._write_json(HTTPStatus.NOT_FOUND, build_error_response("not_found"))
            return

        try:
            content_length = int(self.headers.get("Content-Length", "0"))
            if content_length <= 0:
                raise RuntimeError("request body is empty")
            raw_body = self.rfile.read(content_length)
            content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
            if content_type == "application/json":
                payload = json.loads(raw_body.decode("utf-8"))
                response = self._run_json_inference(payload)
                self._write_json(HTTPStatus.OK, response)
            else:
                response = self._run_binary_inference(raw_body)
                self._write_binary_depth(HTTPStatus.OK, response)
        except Exception as exc:  # noqa: BLE001
            self._write_json(
                HTTPStatus.BAD_REQUEST,
                build_error_response(str(exc), detail=traceback.format_exc()),
            )

    def _infer_depth_array(self, pil_image, model_id: str, process_res: int, resize_mode: str):
        started = time.perf_counter()

        with STATE.lock:
            processor, model = STATE.get_model(model_id)
            torch_module = STATE.torch
            Image = STATE.image_module
            assert torch_module is not None
            assert Image is not None

            processed_image = resize_image_for_process_res(pil_image.convert("RGB"), process_res, resize_mode)
            target_size = rounded_model_input_size(process_res)
            inputs = processor(
                images=processed_image,
                return_tensors="pt",
                size={"height": target_size, "width": target_size},
            )
            inputs = {key: value.to(STATE.device) for key, value in inputs.items()}
            with torch_module.no_grad():
                outputs = model(**inputs)
            post = processor.post_process_depth_estimation(outputs, [processed_image.size[::-1]])[0]
            predicted = post["predicted_depth"]
            if hasattr(predicted, "detach"):
                depth = predicted.detach().cpu().numpy()
            else:
                depth = predicted

        import numpy as np  # type: ignore

        depth_array = np.asarray(depth, dtype=np.float32)
        depth_array = np.squeeze(depth_array)
        if depth_array.ndim != 2:
            raise RuntimeError(f"unsupported depth output shape: {depth_array.shape}")
        elapsed_ms = (time.perf_counter() - started) * 1000.0
        return depth_array, elapsed_ms

    def _run_json_inference(self, payload: dict[str, Any]) -> dict[str, Any]:
        source = payload.get("source")
        if not isinstance(source, dict):
            raise RuntimeError("request.source must be an object")
        source_path = str(source.get("path", "")).strip()
        if not source_path:
            raise RuntimeError("request.source.path is required")

        source_file = Path(source_path)
        if not source_file.exists():
            raise RuntimeError(f"source image file not found: {source_file}")

        model_id = str(payload.get("model_id", "")).strip()
        if not model_id:
            raise RuntimeError("request.model_id is required")

        process_res = resolve_process_res(payload)
        resize_mode = resolve_resize_mode(payload)
        Image = STATE.image_module
        if Image is None:
            STATE._ensure_imports()
            Image = STATE.image_module
        assert Image is not None
        pil_image = Image.open(source_file).convert("RGB")
        depth_array, elapsed_ms = self._infer_depth_array(pil_image, model_id, process_res, resize_mode)
        return {
            "ok": True,
            "depth": {
                "width": int(depth_array.shape[1]),
                "height": int(depth_array.shape[0]),
                "values": depth_array.reshape(-1).astype(float).tolist(),
            },
            "meta": {
                "elapsed_ms": elapsed_ms,
                "model_id": model_id,
                "model_repo": STATE.model_repo,
                "device": STATE.device,
                "process_res": process_res,
                "resize_mode": resize_mode,
                "processed_width": int(depth_array.shape[1]),
                "processed_height": int(depth_array.shape[0]),
            },
        }

    def _run_binary_inference(self, raw_body: bytes) -> dict[str, Any]:
        model_id = self.headers.get("X-ZSoda-Model-Id", "").strip()
        if not model_id:
            raise RuntimeError("X-ZSoda-Model-Id header is required")

        width = parse_positive_int_header(self.headers, "X-ZSoda-Source-Width")
        height = parse_positive_int_header(self.headers, "X-ZSoda-Source-Height")
        channels = parse_positive_int_header(self.headers, "X-ZSoda-Source-Channels")
        if channels != 3:
            raise RuntimeError(f"unsupported binary channel count: {channels}")

        expected = width * height * channels
        if len(raw_body) != expected:
            raise RuntimeError(
                f"binary payload size mismatch: expected {expected} bytes, got {len(raw_body)}"
            )

        quality_raw = self.headers.get("X-ZSoda-Quality", "").strip()
        try:
            quality = int(quality_raw) if quality_raw else 2
        except ValueError as exc:
            raise RuntimeError(f"invalid X-ZSoda-Quality header: {quality_raw!r}") from exc
        resize_mode = self.headers.get("X-ZSoda-Resize-Mode", "upper_bound_letterbox").strip()
        process_res = resolve_process_res({"quality": quality})
        resize_mode = resolve_resize_mode({"resize_mode": resize_mode})

        import numpy as np  # type: ignore

        Image = STATE.image_module
        if Image is None:
            STATE._ensure_imports()
            Image = STATE.image_module
        assert Image is not None

        rgb = np.frombuffer(raw_body, dtype=np.uint8).reshape((height, width, channels))
        pil_image = Image.fromarray(rgb, mode="RGB")
        depth_array, elapsed_ms = self._infer_depth_array(pil_image, model_id, process_res, resize_mode)
        return {
            "depth_array": depth_array,
            "elapsed_ms": elapsed_ms,
            "model_id": model_id,
        }

    def _write_json(self, status: HTTPStatus, payload: dict[str, Any]) -> None:
        encoded = json.dumps(payload, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def _write_binary_depth(self, status: HTTPStatus, payload: dict[str, Any]) -> None:
        depth_array = payload["depth_array"]
        encoded = depth_array.astype("<f4", copy=False).tobytes()
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("X-ZSoda-Depth-Width", str(int(depth_array.shape[1])))
        self.send_header("X-ZSoda-Depth-Height", str(int(depth_array.shape[0])))
        self.send_header("X-ZSoda-Elapsed-Ms", f"{float(payload['elapsed_ms']):.3f}")
        self.send_header("X-ZSoda-Resolved-Model-Id", str(payload["model_id"]))
        self.end_headers()
        self.wfile.write(encoded)


def main() -> int:
    args = parse_args()
    if args.repo_root:
        os.environ["ZSODA_DAD_REPO_ROOT"] = args.repo_root
    preload_model_id = resolve_preload_model_id(args)
    host = (args.host or os.getenv("ZSODA_DAD_SERVICE_HOST", "127.0.0.1")).strip() or "127.0.0.1"
    port = args.port or int(os.getenv("ZSODA_DAD_SERVICE_PORT", "8345").strip() or "8345")
    STATE.preload(preload_model_id)
    server = ThreadingHTTPServer((host, port), Handler)
    print(
        json.dumps(
            {
                "service": "distill_any_depth_remote_service",
                "host": host,
                "port": port,
                "preloaded_model_id": preload_model_id,
                "models": sorted(MODEL_SPECS),
            },
            ensure_ascii=True,
        ),
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
