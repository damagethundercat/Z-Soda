#!/usr/bin/env python3
"""Persistent local HTTP service for official Depth Anything 3 inference."""

from __future__ import annotations

import json
import os
import sys
import threading
import time
import traceback
import types
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


def install_api_stubs() -> None:
    export_module = types.ModuleType("depth_anything_3.utils.export")

    def export_stub(*_args, **_kwargs):
        raise RuntimeError("Export is not supported by official_da3_remote_service.py")

    export_module.export = export_stub
    sys.modules.setdefault("depth_anything_3.utils.export", export_module)

    pose_align_module = types.ModuleType("depth_anything_3.utils.pose_align")

    def align_poses_umeyama_stub(*_args, **_kwargs):
        raise RuntimeError("Pose alignment is not supported by official_da3_remote_service.py")

    pose_align_module.align_poses_umeyama = align_poses_umeyama_stub
    sys.modules.setdefault("depth_anything_3.utils.pose_align", pose_align_module)


def resolve_repo_root() -> Path | None:
    explicit = os.getenv("ZSODA_DA3_REPO_ROOT", "").strip()
    if explicit:
        candidate = Path(explicit).expanduser().resolve()
        if (candidate / "src").exists():
            return candidate
    repo_local = Path(__file__).resolve().parents[1] / ".tmp_external_research" / "Depth-Anything-3"
    if (repo_local / "src").exists():
        return repo_local
    return None


def load_depth_anything_api():
    repo_root = resolve_repo_root()
    if repo_root is not None:
        src_root = repo_root / "src"
        if str(src_root) not in sys.path:
            sys.path.insert(0, str(src_root))

    install_api_stubs()
    from depth_anything_3.api import DepthAnything3  # type: ignore
    import numpy as np  # type: ignore
    import torch  # type: ignore
    from PIL import Image  # type: ignore

    return DepthAnything3, np, torch, Image, repo_root


def resolve_model_repo(model_id: str) -> str:
    mapping = {
        "depth-anything-v3-small": os.getenv("ZSODA_DA3_MODEL_SMALL", "").strip()
        or "depth-anything/da3-small",
        "depth-anything-v3-small-multiview": os.getenv("ZSODA_DA3_MODEL_SMALL", "").strip()
        or "depth-anything/da3-small",
        "depth-anything-v3-base": os.getenv("ZSODA_DA3_MODEL_BASE", "").strip()
        or "depth-anything/da3-base",
        "depth-anything-v3-large": os.getenv("ZSODA_DA3_MODEL_LARGE", "").strip()
        or "depth-anything/da3-large",
        "depth-anything-v3-large-multiview": os.getenv("ZSODA_DA3_MODEL_LARGE", "").strip()
        or "depth-anything/da3-large",
    }
    default_repo = os.getenv("ZSODA_DA3_DEFAULT_MODEL_REPO", "").strip()
    resolved = mapping.get(model_id, default_repo)
    if not resolved:
        raise RuntimeError(
            f"unsupported model_id for official DA3 service: {model_id!r}. "
            "Set ZSODA_DA3_DEFAULT_MODEL_REPO to override."
        )
    return resolved


def resolve_process_res(payload: dict[str, Any]) -> int:
    explicit = payload.get("process_res")
    if isinstance(explicit, int) and explicit > 0:
        return explicit
    quality = payload.get("quality", 2)
    if isinstance(quality, int) and quality in QUALITY_TO_PROCESS_RES:
        return QUALITY_TO_PROCESS_RES[quality]
    return QUALITY_TO_PROCESS_RES[2]


def resolve_resize_mode(payload: dict[str, Any]) -> tuple[str, bool]:
    resize_mode = str(payload.get("resize_mode", "upper_bound_letterbox")).strip().lower()
    if resize_mode == "lower_bound_center_crop":
        return "lower_bound_crop", False
    if resize_mode == "stretch":
        return "upper_bound_resize", True
    return "upper_bound_resize", False


def resolve_device(requested: str, torch_module) -> str:
    normalized = requested.strip().lower()
    if normalized in {"", "auto"}:
        return "cuda" if torch_module.cuda.is_available() else "cpu"
    if normalized == "cuda" and not torch_module.cuda.is_available():
        raise RuntimeError("CUDA device requested, but torch.cuda.is_available() is false")
    return normalized


class ModelManager:
    def __init__(self) -> None:
        self.lock = threading.RLock()
        self.model_repo = ""
        self.device = ""
        self.model = None
        self.depth_anything_class = None
        self.numpy = None
        self.torch = None
        self.image_module = None
        self.repo_root = None
        self.loaded_at = 0.0

    def _ensure_imports(self) -> None:
        if self.depth_anything_class is not None:
            return
        (
            self.depth_anything_class,
            self.numpy,
            self.torch,
            self.image_module,
            self.repo_root,
        ) = load_depth_anything_api()

    def get_model(self, model_id: str):
        requested_device = os.getenv("ZSODA_DA3_SERVICE_DEVICE", "auto")
        with self.lock:
            self._ensure_imports()
            assert self.torch is not None
            device = resolve_device(requested_device, self.torch)
            model_repo = resolve_model_repo(model_id)
            if self.model is None or self.model_repo != model_repo or self.device != device:
                self.model = self.depth_anything_class.from_pretrained(model_repo)
                self.model = self.model.to(device=device)
                self.model.eval()
                self.model_repo = model_repo
                self.device = device
                self.loaded_at = time.time()
            return self.model

    def status(self) -> dict[str, Any]:
        return {
            "model_repo": self.model_repo,
            "device": self.device,
            "loaded": self.model is not None,
            "loaded_at": self.loaded_at,
            "repo_root": "" if self.repo_root is None else str(self.repo_root),
        }


STATE = ModelManager()


def build_error_response(message: str, *, detail: str = "") -> dict[str, Any]:
    payload: dict[str, Any] = {"ok": False, "error": message}
    if detail:
        payload["detail"] = detail
    return payload


class Handler(BaseHTTPRequestHandler):
    server_version = "ZSodaOfficialDA3/0.1"

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
            payload = json.loads(raw_body.decode("utf-8"))
            response = self._run_inference(payload)
            self._write_json(HTTPStatus.OK, response)
        except Exception as exc:  # noqa: BLE001
            self._write_json(
                HTTPStatus.BAD_REQUEST,
                build_error_response(str(exc), detail=traceback.format_exc()),
            )

    def _run_inference(self, payload: dict[str, Any]) -> dict[str, Any]:
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
        process_res_method, pre_stretch = resolve_resize_mode(payload)
        started = time.perf_counter()

        with STATE.lock:
            model = STATE.get_model(model_id)
            np = STATE.numpy
            Image = STATE.image_module
            assert np is not None
            assert Image is not None

            image_input: list[Any]
            if pre_stretch:
                pil_image = Image.open(source_file).convert("RGB")
                pil_image = pil_image.resize((process_res, process_res), Image.BICUBIC)
                image_input = [np.asarray(pil_image, dtype=np.uint8)]
            else:
                image_input = [str(source_file)]

            prediction = model.inference(
                image=image_input,
                process_res=process_res,
                process_res_method=process_res_method,
                ref_view_strategy="middle",
            )
            depth = np.asarray(prediction.depth[0], dtype=np.float32)
        elapsed_ms = (time.perf_counter() - started) * 1000.0

        return {
            "ok": True,
            "depth": {
                "width": int(depth.shape[1]),
                "height": int(depth.shape[0]),
                "values": depth.reshape(-1).astype(float).tolist(),
            },
            "meta": {
                "elapsed_ms": elapsed_ms,
                "model_id": model_id,
                "model_repo": STATE.model_repo,
                "device": STATE.device,
                "process_res": process_res,
                "process_res_method": process_res_method,
                "stretch_prepass": pre_stretch,
            },
        }

    def _write_json(self, status: HTTPStatus, payload: dict[str, Any]) -> None:
        encoded = json.dumps(payload, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)


def main() -> int:
    host = os.getenv("ZSODA_DA3_SERVICE_HOST", "127.0.0.1").strip() or "127.0.0.1"
    port = int(os.getenv("ZSODA_DA3_SERVICE_PORT", "8345").strip() or "8345")
    server = ThreadingHTTPServer((host, port), Handler)
    print(
        json.dumps(
            {
                "service": "official_da3_remote_service",
                "host": host,
                "port": port,
                "endpoint": f"http://{host}:{port}/zsoda/depth",
                "status": f"http://{host}:{port}/status",
            },
            ensure_ascii=False,
        )
    )
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
