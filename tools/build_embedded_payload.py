#!/usr/bin/env python3
"""Append a deterministic embedded payload archive to a plugin binary."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path
from typing import List


HEADER_MAGIC = b"ZSODA_PAYLOAD_V1"
FOOTER_MAGIC = b"ZSODA_FOOTER_V1"
HEADER_STRUCT = struct.Struct("<16sII")
ENTRY_STRUCT = struct.Struct("<IQ")
FOOTER_STRUCT = struct.Struct("<16sQ32s")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact", required=True, help="Target binary to append payload to.")
    parser.add_argument(
        "--root",
        dest="roots",
        action="append",
        default=[],
        help="Directory to embed. Stored under its basename inside the payload.",
    )
    return parser.parse_args()


def iter_payload_entries(root: Path):
    base_name = root.name
    for file_path in sorted(p for p in root.rglob("*") if p.is_file()):
        relative = file_path.relative_to(root)
        payload_path = Path(base_name) / relative
        yield file_path, payload_path.as_posix().encode("utf-8")


def build_payload_bytes(roots: List[Path]) -> bytes:
    entries = []
    for root in roots:
        entries.extend(iter_payload_entries(root))

    payload = bytearray()
    payload.extend(HEADER_STRUCT.pack(HEADER_MAGIC, len(entries), 0))
    for source_path, payload_path in entries:
        file_size = source_path.stat().st_size
        payload.extend(ENTRY_STRUCT.pack(len(payload_path), file_size))
        payload.extend(payload_path)
        payload.extend(source_path.read_bytes())
    return bytes(payload)


def main() -> int:
    args = parse_args()
    artifact = Path(args.artifact)
    if not artifact.is_file():
        raise SystemExit(f"artifact not found: {artifact}")

    roots = []
    for root_text in args.roots:
        root = Path(root_text)
        if not root.is_dir():
            raise SystemExit(f"payload root not found: {root}")
        roots.append(root)

    if not roots:
        return 0

    payload = build_payload_bytes(roots)
    digest = hashlib.sha256(payload).digest()
    footer = FOOTER_STRUCT.pack(FOOTER_MAGIC, len(payload), digest)

    with artifact.open("ab") as stream:
        stream.write(payload)
        stream.write(footer)

    print(
        f"Embedded payload appended: artifact={artifact} entries={len(roots)} payload_bytes={len(payload)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
