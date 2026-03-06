from __future__ import annotations

import dis
import json
import os
import sys
import threading
from datetime import datetime
from pathlib import Path
from types import FrameType


ARTIFACT_ROOT = Path(r"C:\Users\Yongkyu\code\Z-Soda\artifacts\qd3_runtime_trace")
FLAG_PATH = ARTIFACT_ROOT / "enable.flag"
LOG_PATH = ARTIFACT_ROOT / "trace_log.txt"
DIS_PATH = ARTIFACT_ROOT / "trace_disassembly.txt"
META_PATH = ARTIFACT_ROOT / "trace_meta.json"
PARAMS_PATH = ARTIFACT_ROOT / "trace_params.jsonl"

TARGET_FILENAMES = {"<string>", "plugin.py", "framework.py"}
TARGET_NAMES = {
    "render",
    "render_dad",
    "handle_e",
    "get_size",
    "apply_colormap",
    "getPILImageFromInputWorld",
    "writePILImageToOutputWorld",
    "should_abort",
    "report_error",
}

_LOCK = threading.Lock()
_DUMPED_KEYS: set[tuple[str, str, int]] = set()
_ORIGINAL_TRACE = None


def _enabled() -> bool:
    return FLAG_PATH.exists()


def _ensure_dirs() -> None:
    ARTIFACT_ROOT.mkdir(parents=True, exist_ok=True)


def _append_text(path: Path, text: str) -> None:
    _ensure_dirs()
    with path.open("a", encoding="utf-8") as handle:
        handle.write(text)


def _write_meta(frame: FrameType) -> None:
    code = frame.f_code
    payload = {
        "timestamp": datetime.now().isoformat(),
        "python_executable": sys.executable,
        "python_version": sys.version,
        "co_filename": code.co_filename,
        "co_name": code.co_name,
        "co_firstlineno": code.co_firstlineno,
        "co_varnames": list(code.co_varnames),
        "co_names": list(code.co_names),
        "co_consts_preview": [repr(value)[:200] for value in code.co_consts[:32]],
    }
    _ensure_dirs()
    META_PATH.write_text(json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8")


def _append_json_line(path: Path, payload: dict[str, object]) -> None:
    _append_text(path, json.dumps(payload, ensure_ascii=False) + "\n")


def _dump_runtime_params(frame: FrameType) -> None:
    plugin = frame.f_locals.get("plugin")
    params = getattr(plugin, "params", None)
    if not isinstance(params, dict):
        return

    payload = {
        "timestamp": datetime.now().isoformat(),
        "pid": os.getpid(),
        "co_filename": frame.f_code.co_filename,
        "co_name": frame.f_code.co_name,
        "co_firstlineno": frame.f_code.co_firstlineno,
        "params": {
            "quality": params.get("quality"),
            "boost": params.get("boost"),
            "keep_aspect_ratio": params.get("keep_aspect_ratio"),
            "time_consistency": params.get("time_consistency"),
        },
        "locals": {
            "user_quality": frame.f_locals.get("user_quality"),
            "n_tiles": frame.f_locals.get("n_tiles"),
            "time_consistency": frame.f_locals.get("time_consistency"),
        },
    }
    _append_json_line(PARAMS_PATH, payload)


def _dump_frame(frame: FrameType) -> None:
    code = frame.f_code
    key = (code.co_filename, code.co_name, code.co_firstlineno)
    with _LOCK:
      if key in _DUMPED_KEYS:
          return
      _DUMPED_KEYS.add(key)

    _append_text(
        LOG_PATH,
        (
            f"[{datetime.now().isoformat()}] dump {code.co_filename} "
            f"{code.co_name} line={code.co_firstlineno}\n"
        ),
    )
    _write_meta(frame)
    _append_text(DIS_PATH, "\n" + "=" * 80 + "\n")
    _append_text(
        DIS_PATH,
        f"{datetime.now().isoformat()} {code.co_filename} {code.co_name} line={code.co_firstlineno}\n",
    )
    _append_text(DIS_PATH, dis.code_info(code) + "\n")
    _append_text(DIS_PATH, dis.Bytecode(code).dis() + "\n")


def _trace(frame: FrameType, event: str, arg):  # type: ignore[no-untyped-def]
    if event != "call":
        return _trace

    code = frame.f_code
    if code.co_filename not in TARGET_FILENAMES or code.co_name not in TARGET_NAMES:
        return _trace

    try:
        if code.co_filename == "<string>" and code.co_name in {"render", "render_dad"}:
            _dump_runtime_params(frame)
        _dump_frame(frame)
    except Exception as exc:  # pragma: no cover - diagnostic path
        _append_text(LOG_PATH, f"[{datetime.now().isoformat()}] trace_error {exc!r}\n")
    return _trace


def install() -> bool:
    global _ORIGINAL_TRACE
    if not _enabled():
        return False
    _ensure_dirs()
    _append_text(LOG_PATH, f"[{datetime.now().isoformat()}] install trace pid={os.getpid()}\n")
    _ORIGINAL_TRACE = sys.gettrace()
    sys.settrace(_trace)
    threading.settrace(_trace)
    return True


def uninstall() -> None:
    sys.settrace(_ORIGINAL_TRACE)


if __name__ == "__main__":
    installed = install()
    print(f"installed={installed}")
