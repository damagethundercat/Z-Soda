#!/usr/bin/env python3
"""Append a deterministic embedded payload archive to a plugin binary."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path
from typing import Iterable


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
        payload_path = (Path(base_name) / relative).as_posix().encode("utf-8")
        yield file_path, payload_path, file_path.stat().st_size


def collect_entries(roots: Iterable[Path]) -> list[tuple[Path, bytes, int]]:
    entries: list[tuple[Path, bytes, int]] = []
    for root in roots:
        entries.extend(iter_payload_entries(root))
    return entries


def write_payload(stream, roots: list[Path]) -> tuple[int, int, bytes]:
    entries = collect_entries(roots)
    hasher = hashlib.sha256()
    payload_bytes = 0

    def write_chunk(chunk: bytes) -> None:
        nonlocal payload_bytes
        stream.write(chunk)
        hasher.update(chunk)
        payload_bytes += len(chunk)

    write_chunk(HEADER_STRUCT.pack(HEADER_MAGIC, len(entries), 0))
    for source_path, payload_path, file_size in entries:
        write_chunk(ENTRY_STRUCT.pack(len(payload_path), file_size))
        write_chunk(payload_path)
        with source_path.open("rb") as source_stream:
            while True:
                chunk = source_stream.read(1024 * 1024)
                if not chunk:
                    break
                write_chunk(chunk)
    return len(entries), payload_bytes, hasher.digest()


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

    with artifact.open("ab") as stream:
        entry_count, payload_size, digest = write_payload(stream, roots)
        footer = FOOTER_STRUCT.pack(FOOTER_MAGIC, payload_size, digest)
        stream.write(footer)

    print(
        f"Embedded payload appended: artifact={artifact} entries={entry_count} payload_bytes={payload_size}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
