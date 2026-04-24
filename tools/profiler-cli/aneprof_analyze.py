"""
aneprof_analyze.py — reconstruct allocation state from a native .aneprof capture.

This is the quick leak triage CLI. It validates the container, walks allocation
events, rebuilds the live pointer table, and reports probable leak sites by
method, AS3 type, allocation stack, and allocation site.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any


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
}


def parse_header(raw: bytes) -> tuple[int, dict[str, Any]]:
    if len(raw) < HEADER_SIZE:
        raise ValueError("file too small for header")
    magic, version, _reserved, header_len, started_utc = struct.unpack_from("<8sHHIQ", raw, 0)
    if magic != HEADER_MAGIC:
        raise ValueError(f"bad header magic {magic!r}")
    if version != VERSION:
        raise ValueError(f"unsupported version {version}")
    end = HEADER_SIZE + header_len
    if end > len(raw):
        raise ValueError("header JSON exceeds file size")
    header = json.loads(raw[HEADER_SIZE:end].decode("utf-8")) if header_len else {}
    header["_startedUtc"] = started_utc
    return end, header


def parse_footer(raw: bytes) -> dict[str, int]:
    if len(raw) != FOOTER_SIZE:
        raise ValueError("footer size mismatch")
    fields = struct.unpack_from("<8sHHIQQQQQQII", raw, 0)
    if fields[0] != FOOTER_MAGIC:
        raise ValueError(f"bad footer magic {fields[0]!r}")
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


def snapshot_label(payload: bytes) -> str:
    if len(payload) < 56:
        return ""
    label_len = struct.unpack_from("<I", payload, 48)[0]
    if label_len == 0 or 56 + label_len > len(payload):
        return ""
    return payload[56 : 56 + label_len].decode("utf-8", "replace")


def parse_as3_payload(payload: bytes) -> tuple[int, int, str, str]:
    if len(payload) < 24:
        raise ValueError("truncated AS3 object payload")
    sample_id, size, type_len, stack_len = struct.unpack_from("<QQII", payload, 0)
    strings_start = 24
    type_end = strings_start + type_len
    stack_end = type_end + stack_len
    if stack_end > len(payload):
        raise ValueError("AS3 object strings exceed payload size")
    type_name = payload[strings_start:type_end].decode("utf-8", "replace")
    stack = payload[type_end:stack_end].decode("utf-8", "replace")
    return sample_id, size, type_name, stack


def normalize_stack_frame(frame: str) -> str:
    text = frame.strip()
    if text.startswith("#"):
        parts = text.split(" ", 1)
        text = parts[1] if len(parts) == 2 else text
    left, sep, right = text.rpartition(" ")
    if sep and right.startswith("0x"):
        text = left
    return text.strip() or "<unknown>"


def is_framework_frame(frame: str) -> bool:
    return (
        frame.startswith("CodeContext@")
        or frame.startswith("flash.external::ExtensionContext/")
        or frame.startswith("AneAwesomeUtils/profiler")
        or frame.startswith("flash.utils::Timer/")
        or frame.startswith("SetIntervalTimer/")
        or frame.startswith("global/flash.utils::setTimeout")
    )


def stack_frames(stack: str) -> list[str]:
    return [normalize_stack_frame(frame) for frame in stack.splitlines() if frame.strip()]


def allocation_site(stack: str) -> str:
    frames = stack_frames(stack)
    for frame in frames:
        if "/" in frame and not is_framework_frame(frame):
            return frame
    for frame in frames:
        if not is_framework_frame(frame):
            return frame
    return frames[0] if frames else "<no stack>"


def is_custom_as3_type(type_name: str) -> bool:
    if type_name in {"Array", "Object", "String", "<unknown>"}:
        return False
    if "/" in type_name:
        return False
    return not (
        type_name.startswith("flash.")
        or type_name.startswith("builtin.")
        or type_name.startswith("global.")
    )


def sorted_counter_rows(counter: dict[str, list[int]], limit: int = 8) -> list[dict[str, Any]]:
    return [
        {"type_name": key, "count": vals[0], "bytes": vals[1]}
        for key, vals in sorted(counter.items(), key=lambda kv: (kv[1][1], kv[1][0]), reverse=True)[:limit]
    ]


def snapshot_diffs(snapshots: list[dict[str, Any]]) -> list[dict[str, Any]]:
    diffs: list[dict[str, Any]] = []
    for prev, cur in zip(snapshots, snapshots[1:]):
        diffs.append(
            {
                "from": prev.get("label") or "<unnamed>",
                "to": cur.get("label") or "<unnamed>",
                "live_allocations_delta": cur["live_allocations"] - prev["live_allocations"],
                "live_bytes_delta": cur["live_bytes"] - prev["live_bytes"],
                "total_allocations_delta": cur["total_allocations"] - prev["total_allocations"],
                "total_frees_delta": cur["total_frees"] - prev["total_frees"],
            }
        )
    return diffs


def build_leak_suspects(sites: list[dict[str, Any]]) -> list[dict[str, Any]]:
    suspects: list[dict[str, Any]] = []
    for site in sites:
        type_names = [item["type_name"] for item in site["top_types"]]
        reasons: list[str] = []
        if site["count"] >= 100:
            reasons.append("many live AS3 objects from this site")
        if site["bytes"] >= 64 * 1024:
            reasons.append("large live AS3 byte total from this site")
        if any("MethodClosure" in name for name in type_names):
            reasons.append("live MethodClosure objects suggest listener/closure retention")
        if any(name == "flash.utils::ByteArray" for name in type_names):
            reasons.append("live ByteArray payloads retained with this site")
        if any(is_custom_as3_type(name) for name in type_names):
            reasons.append("custom AS3 types are still live")
        stack_text = site["sample_stack"]
        if "addEventListener" in stack_text or "EventDispatcher" in stack_text or "listener" in site["site"].lower():
            reasons.append("listener-related allocation stack")
        if not reasons:
            continue

        confidence = "medium"
        if len(reasons) >= 3 and site["count"] >= 100:
            confidence = "high"
        elif site["count"] < 20 and site["bytes"] < 64 * 1024:
            confidence = "low"

        suspects.append(
            {
                "site": site["site"],
                "confidence": confidence,
                "count": site["count"],
                "bytes": site["bytes"],
                "reasons": reasons,
                "top_types": site["top_types"][:5],
                "sample_stack": site["sample_stack"],
            }
        )
    return sorted(suspects, key=lambda item: (item["confidence"] == "high", item["bytes"], item["count"]), reverse=True)


def analyze(path: Path) -> tuple[dict[str, Any], list[str]]:
    raw = path.read_bytes()
    if len(raw) < HEADER_SIZE + FOOTER_SIZE:
        raise ValueError(f"file too small ({len(raw)} B)")

    event_start, header = parse_header(raw)
    footer = parse_footer(raw[-FOOTER_SIZE:])
    event_end = len(raw) - FOOTER_SIZE

    counts: Counter[str] = Counter()
    live: dict[int, dict[str, int]] = {}
    alloc_by_method: dict[int, list[int]] = defaultdict(lambda: [0, 0])
    live_by_method: dict[int, list[int]] = defaultdict(lambda: [0, 0])
    as3_live: dict[int, dict[str, Any]] = {}
    as3_alloc_by_type: dict[str, list[int]] = defaultdict(lambda: [0, 0])
    as3_live_by_type: dict[str, list[int]] = defaultdict(lambda: [0, 0])
    as3_live_by_stack: dict[tuple[str, str], list[int]] = defaultdict(lambda: [0, 0])
    as3_live_by_site: dict[str, dict[str, Any]] = {}
    as3_unknown_frees = 0
    unknown_frees = 0
    unknown_reallocs = 0
    payload_bytes = 0
    event_count = 0
    snapshots: list[dict[str, Any]] = []

    off = event_start
    while off < event_end:
        if off + EVENT_HEADER_SIZE > event_end:
            raise ValueError(f"truncated event header at offset {off}")
        typ, _flags, payload_size, timestamp_ns, thread_id, _reserved = struct.unpack_from(
            "<HHIQII", raw, off
        )
        off += EVENT_HEADER_SIZE
        if typ not in EVENT_TYPES:
            raise ValueError(f"unknown event type {typ} at offset {off - EVENT_HEADER_SIZE}")
        if off + payload_size > event_end:
            raise ValueError(f"event payload exceeds file size at offset {off}")
        payload = raw[off : off + payload_size]
        off += payload_size

        name = EVENT_TYPES[typ]
        counts[name] += 1
        event_count += 1
        payload_bytes += payload_size

        if name == "alloc":
            if len(payload) < 40:
                raise ValueError("truncated alloc payload")
            ptr, size, _old_ptr, _old_size, method_id, stack_id = struct.unpack_from(
                "<QQQQII", payload, 0
            )
            live[ptr] = {
                "size": size,
                "method_id": method_id,
                "stack_id": stack_id,
                "timestamp_ns": timestamp_ns,
                "thread_id": thread_id,
            }
            alloc_by_method[method_id][0] += 1
            alloc_by_method[method_id][1] += size
        elif name == "free":
            if len(payload) < 40:
                raise ValueError("truncated free payload")
            ptr = struct.unpack_from("<Q", payload, 0)[0]
            if live.pop(ptr, None) is None:
                unknown_frees += 1
        elif name == "realloc":
            if len(payload) < 40:
                raise ValueError("truncated realloc payload")
            ptr, size, old_ptr, _old_size, method_id, stack_id = struct.unpack_from(
                "<QQQQII", payload, 0
            )
            if old_ptr and live.pop(old_ptr, None) is None:
                unknown_reallocs += 1
            if ptr:
                live[ptr] = {
                    "size": size,
                    "method_id": method_id,
                    "stack_id": stack_id,
                    "timestamp_ns": timestamp_ns,
                    "thread_id": thread_id,
                }
                alloc_by_method[method_id][0] += 1
                alloc_by_method[method_id][1] += size
        elif name == "snapshot":
            if len(payload) >= 56:
                values = struct.unpack_from("<QQQQQQII", payload, 0)
                snapshots.append(
                    {
                        "label": snapshot_label(payload),
                        "live_allocations": values[0],
                        "live_bytes": values[1],
                        "total_allocations": values[2],
                        "total_frees": values[3],
                        "total_reallocations": values[4],
                        "unknown_frees": values[5],
                    }
                )
        elif name == "as3_alloc":
            sample_id, size, type_name, stack = parse_as3_payload(payload)
            as3_live[sample_id] = {
                "size": size,
                "type_name": type_name or "<unknown>",
                "stack": stack,
                "timestamp_ns": timestamp_ns,
                "thread_id": thread_id,
            }
            as3_alloc_by_type[type_name or "<unknown>"][0] += 1
            as3_alloc_by_type[type_name or "<unknown>"][1] += size
        elif name == "as3_free":
            sample_id, _size, _type_name, _stack = parse_as3_payload(payload)
            if as3_live.pop(sample_id, None) is None:
                as3_unknown_frees += 1

    if off != event_end:
        raise ValueError("event walk ended at wrong offset")

    live_bytes = 0
    for meta in live.values():
        size = meta["size"]
        method_id = meta["method_id"]
        live_bytes += size
        live_by_method[method_id][0] += 1
        live_by_method[method_id][1] += size

    as3_live_bytes = 0
    for meta in as3_live.values():
        size = meta["size"]
        type_name = meta["type_name"]
        stack = meta["stack"]
        site = allocation_site(stack)
        as3_live_bytes += size
        as3_live_by_type[type_name][0] += 1
        as3_live_by_type[type_name][1] += size
        as3_live_by_stack[(type_name, stack)][0] += 1
        as3_live_by_stack[(type_name, stack)][1] += size
        site_meta = as3_live_by_site.setdefault(
            site,
            {
                "count": 0,
                "bytes": 0,
                "types": defaultdict(lambda: [0, 0]),
                "sample_stack": stack,
            },
        )
        site_meta["count"] += 1
        site_meta["bytes"] += size
        site_meta["types"][type_name][0] += 1
        site_meta["types"][type_name][1] += size

    warnings: list[str] = []
    if event_count != footer["event_count"]:
        warnings.append(f"footer.event_count={footer['event_count']} actual={event_count}")
    if payload_bytes != footer["payload_bytes"]:
        warnings.append(f"footer.payload_bytes={footer['payload_bytes']} actual={payload_bytes}")
    if len(live) != footer["live_allocations"]:
        warnings.append(
            f"footer.live_allocations={footer['live_allocations']} reconstructed={len(live)}"
        )
    if live_bytes != footer["live_bytes"]:
        warnings.append(f"footer.live_bytes={footer['live_bytes']} reconstructed={live_bytes}")

    top_as3_allocation_sites = [
        {
            "site": site,
            "count": vals["count"],
            "bytes": vals["bytes"],
            "top_types": sorted_counter_rows(vals["types"]),
            "sample_stack": vals["sample_stack"],
        }
        for site, vals in sorted(
            as3_live_by_site.items(),
            key=lambda kv: (kv[1]["bytes"], kv[1]["count"]),
            reverse=True,
        )
    ]

    result: dict[str, Any] = {
        "path": str(path),
        "file_size": len(raw),
        "header": header,
        "footer": footer,
        "event_count": event_count,
        "payload_bytes": payload_bytes,
        "counts": dict(sorted(counts.items())),
        "live_allocations": len(live),
        "live_bytes": live_bytes,
        "unknown_frees": unknown_frees,
        "unknown_reallocs": unknown_reallocs,
        "as3_live_allocations": len(as3_live),
        "as3_live_bytes": as3_live_bytes,
        "as3_unknown_frees": as3_unknown_frees,
        "snapshots": snapshots,
        "snapshot_diffs": snapshot_diffs(snapshots),
        "top_live_methods": [
            {"method_id": mid, "count": vals[0], "bytes": vals[1]}
            for mid, vals in sorted(live_by_method.items(), key=lambda kv: kv[1][1], reverse=True)
        ],
        "top_allocation_methods": [
            {"method_id": mid, "count": vals[0], "bytes": vals[1]}
            for mid, vals in sorted(alloc_by_method.items(), key=lambda kv: kv[1][1], reverse=True)
        ],
        "top_as3_live_types": [
            {"type_name": type_name, "count": vals[0], "bytes": vals[1]}
            for type_name, vals in sorted(
                as3_live_by_type.items(), key=lambda kv: (kv[1][1], kv[1][0]), reverse=True
            )
        ],
        "top_as3_allocation_types": [
            {"type_name": type_name, "count": vals[0], "bytes": vals[1]}
            for type_name, vals in sorted(
                as3_alloc_by_type.items(), key=lambda kv: (kv[1][1], kv[1][0]), reverse=True
            )
        ],
        "top_as3_live_stacks": [
            {"type_name": key[0], "stack": key[1], "count": vals[0], "bytes": vals[1]}
            for key, vals in sorted(
                as3_live_by_stack.items(), key=lambda kv: (kv[1][1], kv[1][0]), reverse=True
            )
        ],
        "top_as3_allocation_sites": top_as3_allocation_sites,
    }
    result["leak_suspects"] = build_leak_suspects(top_as3_allocation_sites)
    return result, warnings


def stack_lines_for_report(stack: str, stack_frames: int) -> tuple[list[str], int]:
    frames = stack.splitlines() or ["<no stack>"]
    if stack_frames <= 0:
        return frames, 0
    return frames[:stack_frames], max(0, len(frames) - stack_frames)


def print_report(result: dict[str, Any], warnings: list[str], top: int, stack_frames: int) -> None:
    print(f"=== .aneprof leak analysis: {Path(result['path']).name} ===")
    print(f"  events          : {result['event_count']:>12}")
    print(f"  alloc/free/reall: {result['counts'].get('alloc', 0):>6} / "
          f"{result['counts'].get('free', 0):>6} / {result['counts'].get('realloc', 0):>6}")
    print(f"  live allocations: {result['live_allocations']:>12}")
    print(f"  live bytes      : {result['live_bytes']:>12}")
    print(f"  unknown frees   : {result['unknown_frees']:>12}")
    print(f"  unknown reallocs: {result['unknown_reallocs']:>12}")
    print(f"  AS3 alloc/free  : {result['counts'].get('as3_alloc', 0):>6} / "
          f"{result['counts'].get('as3_free', 0):>6}")
    print(f"  AS3 live objects: {result['as3_live_allocations']:>12}")
    print(f"  AS3 live bytes  : {result['as3_live_bytes']:>12}")
    print(f"  AS3 unknown free: {result['as3_unknown_frees']:>12}")

    if warnings:
        print("  warnings:")
        for warning in warnings:
            print(f"    - {warning}")

    if result["counts"].get("alloc", 0) and not (
        result["counts"].get("free", 0) or result["counts"].get("realloc", 0)
    ):
        print("  diagnostic      : incomplete; alloc events exist but no free/realloc events were captured")
    elif result["live_allocations"]:
        print("  diagnostic      : probable live allocations remain at stop")
    else:
        print("  diagnostic      : no reconstructed live allocations at stop")

    growth = [
        item for item in result["snapshot_diffs"]
        if item["live_bytes_delta"] > 0 or item["live_allocations_delta"] > 0
    ]
    if growth:
        print("\nSnapshot growth:")
        for item in growth[-min(top, len(growth)):]:
            print(
                f"  {item['from']} -> {item['to']}: "
                f"liveCount {item['live_allocations_delta']:+} "
                f"liveBytes {item['live_bytes_delta']:+}"
            )

    suspects = result["leak_suspects"][:top]
    if suspects:
        print("\nLeak suspects:")
        for item in suspects:
            top_types = ", ".join(
                f"{typ['type_name']} x{typ['count']}" for typ in item["top_types"][:3]
            )
            print(
                f"  [{item['confidence']}] {item['site']} "
                f"count={item['count']} bytes={item['bytes']}"
            )
            if top_types:
                print(f"    types: {top_types}")
            for reason in item["reasons"][:3]:
                print(f"    - {reason}")

    live_methods = result["top_live_methods"][:top]
    if live_methods:
        print("\nTop live methods:")
        for item in live_methods:
            print(
                f"  method={item['method_id']:<10} count={item['count']:<8} bytes={item['bytes']}"
            )

    alloc_methods = result["top_allocation_methods"][:top]
    if alloc_methods:
        print("\nTop allocation methods:")
        for item in alloc_methods:
            print(
                f"  method={item['method_id']:<10} count={item['count']:<8} bytes={item['bytes']}"
            )

    as3_types = result["top_as3_live_types"][:top]
    if as3_types:
        print("\nTop AS3 live types:")
        for item in as3_types:
            print(
                f"  type={item['type_name']:<48} count={item['count']:<8} bytes={item['bytes']}"
            )

    as3_sites = result["top_as3_allocation_sites"][:top]
    if as3_sites:
        print("\nTop AS3 allocation sites:")
        for item in as3_sites:
            top_types = ", ".join(
                f"{typ['type_name']} x{typ['count']}" for typ in item["top_types"][:3]
            )
            print(
                f"  site={item['site']:<48} count={item['count']:<8} bytes={item['bytes']}"
            )
            if top_types:
                print(f"    types: {top_types}")

    as3_stacks = result["top_as3_live_stacks"][:top]
    if as3_stacks:
        print("\nTop AS3 live stacks:")
        for item in as3_stacks:
            frames, hidden = stack_lines_for_report(item["stack"], stack_frames)
            print(
                f"  type={item['type_name']:<40} count={item['count']:<8} "
                f"bytes={item['bytes']} at {frames[0]}"
            )
            for frame in frames[1:]:
                print(f"    {frame}")
            if hidden:
                print(f"    ... {hidden} more frame(s)")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("file", type=Path)
    parser.add_argument("--json", type=Path, help="write machine-readable summary")
    parser.add_argument("--top", type=int, default=10)
    parser.add_argument("--require-free-events", action="store_true")
    parser.add_argument("--fail-on-leak", action="store_true")
    parser.add_argument("--min-live-bytes", type=int, default=1)
    parser.add_argument("--min-live-count", type=int, default=1)
    parser.add_argument(
        "--stack-frames",
        type=int,
        default=0,
        help="AS3 frames to print per live stack; 0 prints the full captured stack",
    )
    args = parser.parse_args()

    try:
        result, warnings = analyze(args.file)
    except Exception as exc:
        print(f"ERR: {exc}", file=sys.stderr)
        return 2

    print_report(result, warnings, max(0, args.top), args.stack_frames)
    if args.json:
        args.json.write_text(json.dumps(result, indent=2), encoding="utf-8")

    allocs = result["counts"].get("alloc", 0)
    frees = result["counts"].get("free", 0)
    reallocs = result["counts"].get("realloc", 0)
    if args.require_free_events and allocs and not (frees or reallocs):
        return 6
    if warnings:
        return 4
    if args.fail_on_leak:
        if (result["live_bytes"] >= args.min_live_bytes and
                result["live_allocations"] >= args.min_live_count):
            return 8
    return 0


if __name__ == "__main__":
    sys.exit(main())
