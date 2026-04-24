"""
aneprof_validate.py — parse and validate a native .aneprof capture.

The validator checks the fixed header/footer, walks every event record, and
prints event counts by type. It does not interpret method-table payloads or
symbolicate stacks; it is a structural validator for CI and quick triage.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from collections import Counter
from pathlib import Path


HEADER_MAGIC = b"ANEPROF\x00"
FOOTER_MAGIC = b"ANEPEND\x00"
VERSION = 1
HEADER_SIZE = 24
EVENT_HEADER_SIZE = 24
FOOTER_SIZE = 72

EVENT_TYPES = {
    1: "start",
    2: "stop",
    3: "marker",
    4: "snapshot",
    5: "method_enter",
    6: "method_exit",
    7: "alloc",
    8: "free",
    9: "realloc",
    10: "live_allocation",
    11: "method_table",
    12: "as3_alloc",
    13: "as3_free",
    14: "as3_reference",
    15: "as3_reference_ex",
    16: "as3_root",
    17: "as3_payload",
    18: "frame",
    19: "gc_cycle",
}


def parse_header(raw: bytes) -> tuple[int, dict]:
    if len(raw) < HEADER_SIZE:
        raise ValueError("file too small for header")
    magic, version, reserved, header_len, started_utc = struct.unpack_from("<8sHHIQ", raw, 0)
    if magic != HEADER_MAGIC:
        raise ValueError(f"bad header magic {magic!r}")
    if version != VERSION:
        raise ValueError(f"unsupported version {version}")
    end = HEADER_SIZE + header_len
    if end > len(raw):
        raise ValueError("header JSON exceeds file size")
    try:
        header_json = json.loads(raw[HEADER_SIZE:end].decode("utf-8")) if header_len else {}
    except Exception as exc:
        raise ValueError(f"invalid header JSON: {exc}") from exc
    header_json["_startedUtc"] = started_utc
    return end, header_json


def parse_footer(raw: bytes) -> dict:
    if len(raw) != FOOTER_SIZE:
        raise ValueError("footer size mismatch")
    fields = struct.unpack_from("<8sHHIQQQQQQII", raw, 0)
    magic = fields[0]
    if magic != FOOTER_MAGIC:
        raise ValueError(f"bad footer magic {magic!r}")
    if fields[1] != VERSION:
        raise ValueError(f"bad footer version {fields[1]}")
    if fields[3] != FOOTER_SIZE:
        raise ValueError(f"bad footer size {fields[3]}")
    return {
        "event_count": fields[4],
        "dropped_count": fields[5],
        "payload_bytes": fields[6],
        "ended_utc": fields[7],
        "live_allocations": fields[8],
        "live_bytes": fields[9],
        "crc32_events": fields[10],
    }


def walk_events(raw: bytes, start: int, end: int) -> tuple[int, int, Counter[str]]:
    off = start
    count = 0
    payload_bytes = 0
    counts: Counter[str] = Counter()
    while off < end:
        if off + EVENT_HEADER_SIZE > end:
            raise ValueError(f"truncated event header at offset {off}")
        typ, flags, payload_size, timestamp_ns, thread_id, reserved = struct.unpack_from(
            "<HHIQII", raw, off
        )
        off += EVENT_HEADER_SIZE
        if typ not in EVENT_TYPES:
            raise ValueError(f"unknown event type {typ} at offset {off - EVENT_HEADER_SIZE}")
        if off + payload_size > end:
            raise ValueError(f"event payload exceeds file size at offset {off}")
        off += payload_size
        count += 1
        payload_bytes += payload_size
        counts[EVENT_TYPES[typ]] += 1
    if off != end:
        raise ValueError("event walk ended at wrong offset")
    return count, payload_bytes, counts


def validate(path: Path) -> int:
    raw = path.read_bytes()
    if len(raw) < HEADER_SIZE + FOOTER_SIZE:
        print(f"ERR: file too small ({len(raw)} B)", file=sys.stderr)
        return 2
    try:
        event_start, header = parse_header(raw)
        footer = parse_footer(raw[-FOOTER_SIZE:])
        event_count, payload_bytes, counts = walk_events(raw, event_start, len(raw) - FOOTER_SIZE)
    except ValueError as exc:
        print(f"ERR: {exc}", file=sys.stderr)
        return 2

    ok = True
    if event_count != footer["event_count"]:
        print(f"WARN: footer.event_count={footer['event_count']} actual={event_count}")
        ok = False
    if payload_bytes != footer["payload_bytes"]:
        print(f"WARN: footer.payload_bytes={footer['payload_bytes']} actual={payload_bytes}")
        ok = False

    print(f"=== .aneprof summary: {path.name} ===")
    print(f"  file size       : {len(raw):>12} B")
    print(f"  header JSON     : {json.dumps(header, indent=2)}")
    print(f"  events          : {event_count:>12}")
    print(f"  payload bytes   : {payload_bytes:>12}")
    print(f"  dropped         : {footer['dropped_count']:>12}")
    print(f"  live allocations: {footer['live_allocations']:>12}")
    print(f"  live bytes      : {footer['live_bytes']:>12}")
    for name, value in sorted(counts.items()):
        print(f"  {name:<16}: {value:>12}")
    return 0 if ok else 4


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("file", type=Path)
    args = parser.parse_args()
    sys.exit(validate(args.file))
