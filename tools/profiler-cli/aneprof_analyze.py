"""
aneprof_analyze.py — reconstruct allocation state from a native .aneprof capture.

This is the quick leak triage CLI. It validates the container, walks allocation
events, rebuilds the live pointer table, and reports probable leak sites by
method, AS3 type, allocation stack, and allocation site.
"""

from __future__ import annotations

import argparse
import html
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
    14: "as3_reference",
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


def parse_as3_reference_payload(payload: bytes) -> tuple[int, int]:
    if len(payload) < 16:
        raise ValueError("truncated AS3 reference payload")
    return struct.unpack_from("<QQ", payload, 0)


def parse_marker_payload(payload: bytes) -> tuple[str, str]:
    if len(payload) < 8:
        raise ValueError("truncated marker payload")
    name_len, value_len = struct.unpack_from("<II", payload, 0)
    name_start = 8
    name_end = name_start + name_len
    value_end = name_end + value_len
    if value_end > len(payload):
        raise ValueError("marker strings exceed payload size")
    name = payload[name_start:name_end].decode("utf-8", "replace")
    value_json = payload[name_end:value_end].decode("utf-8", "replace")
    return name, value_json


def parse_method_table(payload: bytes) -> dict[int, str]:
    if not payload:
        return {}
    text = payload.decode("utf-8", "replace").strip()
    out: dict[int, str] = {}
    try:
        data = json.loads(text)
        if isinstance(data, dict):
            methods = data.get("methods", data)
            if isinstance(methods, list):
                for item in methods:
                    if isinstance(item, dict) and "id" in item:
                        name = item.get("name") or item.get("qualifiedName") or item.get("method")
                        if name:
                            out[int(item["id"])] = str(name)
            elif isinstance(methods, dict):
                for key, value in methods.items():
                    out[int(key)] = str(value)
        return out
    except Exception:
        pass
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        left, sep, right = line.partition(" ")
        if not sep:
            left, sep, right = line.partition(",")
        if sep:
            try:
                out[int(left, 0)] = right.strip()
            except ValueError:
                continue
    return out


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


def sorted_count_rows(counter: dict[tuple[str, str], int], limit: int = 8) -> list[dict[str, Any]]:
    return [
        {"type_name": key[0], "site": key[1], "count": count}
        for key, count in sorted(counter.items(), key=lambda kv: kv[1], reverse=True)[:limit]
    ]


def sorted_bytes_rows(counter: dict[tuple[str, str], list[int]], limit: int = 8) -> list[dict[str, Any]]:
    return [
        {"type_name": key[0], "site": key[1], "count": vals[0], "bytes": vals[1]}
        for key, vals in sorted(counter.items(), key=lambda kv: (kv[1][1], kv[1][0]), reverse=True)[:limit]
    ]


def fmt_ms(ns: int | float) -> float:
    return round(float(ns) / 1_000_000.0, 3)


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


def summarize_as3_live_state(as3_live: dict[int, dict[str, Any]], top_limit: int = 50) -> dict[str, Any]:
    by_type: dict[str, list[int]] = defaultdict(lambda: [0, 0])
    by_site: dict[str, list[int]] = defaultdict(lambda: [0, 0])
    by_stack: dict[tuple[str, str], list[int]] = defaultdict(lambda: [0, 0])
    total_bytes = 0

    for meta in as3_live.values():
        size = int(meta.get("size", 0) or 0)
        type_name = str(meta.get("type_name") or "<unknown>")
        stack = str(meta.get("stack") or "")
        site = allocation_site(stack)
        total_bytes += size
        by_type[type_name][0] += 1
        by_type[type_name][1] += size
        by_site[site][0] += 1
        by_site[site][1] += size
        by_stack[(type_name, stack)][0] += 1
        by_stack[(type_name, stack)][1] += size

    return {
        "as3_live_allocations": len(as3_live),
        "as3_live_bytes": total_bytes,
        "top_types": [
            {"type_name": key, "count": vals[0], "bytes": vals[1]}
            for key, vals in sorted(by_type.items(), key=lambda kv: (kv[1][1], kv[1][0]), reverse=True)[:top_limit]
        ],
        "top_sites": [
            {"site": key, "count": vals[0], "bytes": vals[1]}
            for key, vals in sorted(by_site.items(), key=lambda kv: (kv[1][1], kv[1][0]), reverse=True)[:top_limit]
        ],
        "top_stacks": [
            {"type_name": key[0], "stack": key[1], "count": vals[0], "bytes": vals[1]}
            for key, vals in sorted(by_stack.items(), key=lambda kv: (kv[1][1], kv[1][0]), reverse=True)[:top_limit]
        ],
    }


def snapshot_growth_rows(
    before_rows: list[dict[str, Any]],
    after_rows: list[dict[str, Any]],
    key_fields: tuple[str, ...],
    limit: int = 10,
) -> list[dict[str, Any]]:
    before = {tuple(row.get(field) for field in key_fields): row for row in before_rows}
    after = {tuple(row.get(field) for field in key_fields): row for row in after_rows}
    rows: list[dict[str, Any]] = []
    for key in sorted(set(before) | set(after)):
        b = before.get(key, {})
        a = after.get(key, {})
        row = {field: key[idx] for idx, field in enumerate(key_fields)}
        for field in ("count", "bytes"):
            b_val = int(b.get(field, 0) or 0)
            a_val = int(a.get(field, 0) or 0)
            row[f"baseline_{field}"] = b_val
            row[f"target_{field}"] = a_val
            row[f"delta_{field}"] = a_val - b_val
        rows.append(row)
    rows.sort(key=lambda item: (item["delta_bytes"], item["delta_count"]), reverse=True)
    return rows[:limit]


def as3_snapshot_diffs(summaries: list[dict[str, Any]], limit: int = 10) -> list[dict[str, Any]]:
    diffs: list[dict[str, Any]] = []
    for prev, cur in zip(summaries, summaries[1:]):
        diffs.append(
            {
                "from": prev.get("label") or "<unnamed>",
                "to": cur.get("label") or "<unnamed>",
                "elapsed_ms_delta": round(cur.get("elapsed_ms", 0) - prev.get("elapsed_ms", 0), 3),
                "as3_live_allocations_delta": (
                    cur["as3_live_allocations"] - prev["as3_live_allocations"]
                ),
                "as3_live_bytes_delta": cur["as3_live_bytes"] - prev["as3_live_bytes"],
                "type_growth": snapshot_growth_rows(prev["top_types"], cur["top_types"], ("type_name",), limit),
                "site_growth": snapshot_growth_rows(prev["top_sites"], cur["top_sites"], ("site",), limit),
                "stack_growth": snapshot_growth_rows(prev["top_stacks"], cur["top_stacks"], ("type_name", "stack"), limit),
            }
        )
    return diffs


def snapshot_by_label(summaries: list[dict[str, Any]], label: str) -> dict[str, Any] | None:
    for item in summaries:
        if item.get("label") == label:
            return item
    return None


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
        if site.get("retainer_hints"):
            reasons.append("runtime reference edges point at this allocation site")
            if any("MethodClosure" in item["type_name"] for item in site["retainer_hints"]):
                reasons.append("MethodClosure retains objects from this site")
        if site.get("owned_dependent_refs", 0):
            reasons.append("live AS3 objects from this site own runtime dependent refs")
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
                "retainer_hints": site.get("retainer_hints", [])[:5],
                "owned_dependent_refs": site.get("owned_dependent_refs", 0),
                "dependent_ref_hints": site.get("dependent_ref_hints", [])[:5],
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
    as3_references: list[tuple[int, int]] = []
    as3_unknown_frees = 0
    unknown_frees = 0
    unknown_reallocs = 0
    payload_bytes = 0
    event_count = 0
    snapshots: list[dict[str, Any]] = []
    as3_snapshot_summaries: list[dict[str, Any]] = []
    markers: list[dict[str, Any]] = []
    method_names: dict[int, str] = {}
    method_stack: list[dict[str, int]] = []
    method_stats: dict[int, dict[str, int]] = defaultdict(
        lambda: {"count": 0, "inclusive_ns": 0, "exclusive_ns": 0, "max_ns": 0}
    )
    method_stack_mismatches = 0
    first_event_ns: int | None = None
    last_event_ns: int | None = None
    current_snapshot: dict[str, Any] | None = None

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
        if first_event_ns is None:
            first_event_ns = timestamp_ns
        last_event_ns = timestamp_ns

        if name == "marker":
            marker_name, marker_value = parse_marker_payload(payload)
            markers.append(
                {
                    "name": marker_name,
                    "value": marker_value,
                    "timestamp_ns": timestamp_ns,
                    "thread_id": thread_id,
                }
            )
        elif name == "method_enter":
            if len(payload) < 8:
                raise ValueError("truncated method_enter payload")
            method_id, depth = struct.unpack_from("<II", payload, 0)
            method_stack.append(
                {
                    "method_id": method_id,
                    "depth": depth,
                    "start_ns": timestamp_ns,
                    "child_ns": 0,
                }
            )
        elif name == "method_exit":
            if len(payload) < 8:
                raise ValueError("truncated method_exit payload")
            method_id, _depth = struct.unpack_from("<II", payload, 0)
            frame = method_stack.pop() if method_stack else None
            if frame is None:
                method_stack_mismatches += 1
            else:
                if frame["method_id"] != method_id:
                    method_stack_mismatches += 1
                inclusive_ns = max(0, timestamp_ns - frame["start_ns"])
                exclusive_ns = max(0, inclusive_ns - frame["child_ns"])
                stats = method_stats[frame["method_id"]]
                stats["count"] += 1
                stats["inclusive_ns"] += inclusive_ns
                stats["exclusive_ns"] += exclusive_ns
                stats["max_ns"] = max(stats["max_ns"], inclusive_ns)
                if method_stack:
                    method_stack[-1]["child_ns"] += inclusive_ns
        elif name == "method_table":
            method_names.update(parse_method_table(payload))
        elif name == "alloc":
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
                label = snapshot_label(payload)
                snap = {
                    "label": label,
                    "timestamp_ns": timestamp_ns,
                    "elapsed_ms": fmt_ms(timestamp_ns - (first_event_ns or timestamp_ns)),
                    "thread_id": thread_id,
                    "live_allocations": values[0],
                    "live_bytes": values[1],
                    "total_allocations": values[2],
                    "total_frees": values[3],
                    "total_reallocations": values[4],
                    "unknown_frees": values[5],
                    "sampled_live_allocations": 0,
                    "sampled_live_bytes": 0,
                    "_sampled_live_by_method": defaultdict(lambda: [0, 0]),
                }
                snapshots.append(snap)
                current_snapshot = snap
                as3_summary = summarize_as3_live_state(as3_live)
                as3_summary.update(
                    {
                        "label": label,
                        "timestamp_ns": timestamp_ns,
                        "elapsed_ms": snap["elapsed_ms"],
                    }
                )
                as3_snapshot_summaries.append(as3_summary)
        elif name == "live_allocation":
            if len(payload) < 32:
                raise ValueError("truncated live_allocation payload")
            _ptr, size, _alloc_ts, _alloc_thread, method_id = struct.unpack_from(
                "<QQQII", payload, 0
            )
            if current_snapshot is not None:
                current_snapshot["sampled_live_allocations"] += 1
                current_snapshot["sampled_live_bytes"] += size
                sampled = current_snapshot["_sampled_live_by_method"][method_id]
                sampled[0] += 1
                sampled[1] += size
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
        elif name == "as3_reference":
            owner_id, dependent_id = parse_as3_reference_payload(payload)
            as3_references.append((owner_id, dependent_id))

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

    sample_site: dict[int, str] = {}
    for sample_id, meta in as3_live.items():
        sample_site[sample_id] = allocation_site(meta["stack"])

    live_reference_edges: list[tuple[int, int]] = []
    live_edges_by_owner: dict[int, list[int]] = defaultdict(list)
    retainer_hints_by_site: dict[str, dict[str, Any]] = {}
    dependent_refs_by_site: dict[str, dict[str, Any]] = {}
    reference_type_edges: dict[tuple[str, str], int] = defaultdict(int)
    reference_owner_edges: dict[tuple[str, str], int] = defaultdict(int)
    direct_retained_by_owner: dict[tuple[str, str], list[int]] = defaultdict(lambda: [0, 0])
    as3_reference_edges_with_live_owner = 0
    for owner_id, dependent_id in as3_references:
        owner = as3_live.get(owner_id)
        dependent = as3_live.get(dependent_id)
        if owner:
            owner_site = sample_site.get(owner_id, "<no stack>")
            owner_type = owner["type_name"]
            as3_reference_edges_with_live_owner += 1
            reference_owner_edges[(owner_type, owner_site)] += 1
            owned_hint = dependent_refs_by_site.setdefault(
                owner_site,
                {
                    "count": 0,
                    "owner_types": defaultdict(int),
                },
            )
            owned_hint["count"] += 1
            owned_hint["owner_types"][(owner_type, owner_site)] += 1

        if not owner or not dependent:
            continue
        live_reference_edges.append((owner_id, dependent_id))
        live_edges_by_owner[owner_id].append(dependent_id)
        dependent_site = sample_site.get(dependent_id, "<no stack>")
        dependent_type = dependent["type_name"]
        reference_type_edges[(owner_type, dependent_type)] += 1
        direct = direct_retained_by_owner[(owner_type, owner_site)]
        direct[0] += 1
        direct[1] += dependent["size"]

        hint = retainer_hints_by_site.setdefault(
            dependent_site,
            {
                "count": 0,
                "owner_types": defaultdict(int),
            },
        )
        hint["count"] += 1
        hint["owner_types"][(owner_type, owner_site)] += 1

    retained_by_owner: dict[tuple[str, str], list[int]] = defaultdict(lambda: [0, 0])
    if len(live_edges_by_owner) <= 2000 and len(live_reference_edges) <= 20000:
        for owner_id, children in live_edges_by_owner.items():
            owner = as3_live.get(owner_id)
            if not owner:
                continue
            owner_key = (owner["type_name"], sample_site.get(owner_id, "<no stack>"))
            seen: set[int] = set()
            stack = list(children)
            retained_count = 0
            retained_bytes = 0
            while stack and len(seen) < 5000:
                dep_id = stack.pop()
                if dep_id in seen:
                    continue
                seen.add(dep_id)
                dep = as3_live.get(dep_id)
                if not dep:
                    continue
                retained_count += 1
                retained_bytes += dep["size"]
                stack.extend(live_edges_by_owner.get(dep_id, []))
            retained_by_owner[owner_key][0] += retained_count
            retained_by_owner[owner_key][1] += retained_bytes

    marker_open: dict[str, list[dict[str, Any]]] = defaultdict(list)
    marker_spans: list[dict[str, Any]] = []
    for marker in markers:
        marker_name = marker["name"]
        if marker_name.endswith(".start"):
            marker_open[marker_name[:-6]].append(marker)
        elif marker_name.endswith(".end"):
            base_name = marker_name[:-4]
            if marker_open[base_name]:
                start = marker_open[base_name].pop()
                marker_spans.append(
                    {
                        "name": base_name,
                        "duration_ms": fmt_ms(marker["timestamp_ns"] - start["timestamp_ns"]),
                        "start_ns": start["timestamp_ns"],
                        "end_ns": marker["timestamp_ns"],
                        "start_value": start["value"],
                        "end_value": marker["value"],
                    }
                )

    capture_duration_ns = max(0, (last_event_ns or 0) - (first_event_ns or 0))
    capture_duration_sec = capture_duration_ns / 1_000_000_000.0 if capture_duration_ns else 0.0
    event_region_bytes = event_count * EVENT_HEADER_SIZE + payload_bytes

    top_method_timing = []
    for method_id, vals in method_stats.items():
        count = vals["count"]
        if count <= 0:
            continue
        name = method_names.get(method_id, f"method#{method_id}")
        top_method_timing.append(
            {
                "method_id": method_id,
                "name": name,
                "count": count,
                "inclusive_ms": fmt_ms(vals["inclusive_ns"]),
                "exclusive_ms": fmt_ms(vals["exclusive_ns"]),
                "avg_inclusive_ms": fmt_ms(vals["inclusive_ns"] / count),
                "avg_exclusive_ms": fmt_ms(vals["exclusive_ns"] / count),
                "max_inclusive_ms": fmt_ms(vals["max_ns"]),
            }
        )
    top_method_timing.sort(key=lambda item: (item["inclusive_ms"], item["count"]), reverse=True)

    dependent_refs_by_type: Counter[str] = Counter()
    for (owner_type, _owner_site), count in reference_owner_edges.items():
        dependent_refs_by_type[owner_type] += count
    retained_by_type: dict[str, list[int]] = defaultdict(lambda: [0, 0])
    for (owner_type, _owner_site), vals in retained_by_owner.items():
        retained_by_type[owner_type][0] += vals[0]
        retained_by_type[owner_type][1] += vals[1]
    top_as3_memory_by_type = []
    for type_name, vals in as3_live_by_type.items():
        retained_vals = retained_by_type.get(type_name, [0, 0])
        top_as3_memory_by_type.append(
            {
                "type_name": type_name,
                "count": vals[0],
                "shallow_bytes": vals[1],
                "runtime_dependent_refs": dependent_refs_by_type.get(type_name, 0),
                "retained_count": retained_vals[0],
                "retained_bytes": retained_vals[1],
            }
        )
    top_as3_memory_by_type.sort(
        key=lambda item: (
            item["retained_bytes"],
            item["shallow_bytes"],
            item["runtime_dependent_refs"],
            item["count"],
        ),
        reverse=True,
    )

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
    if method_stack:
        warnings.append(f"method stack still has {len(method_stack)} frame(s) at end")
    if method_stack_mismatches:
        warnings.append(f"method enter/exit mismatch count={method_stack_mismatches}")

    top_as3_allocation_sites = [
        {
            "site": site,
            "count": vals["count"],
            "bytes": vals["bytes"],
            "top_types": sorted_counter_rows(vals["types"]),
            "owned_dependent_refs": dependent_refs_by_site.get(site, {}).get("count", 0),
            "dependent_ref_hints": sorted_count_rows(
                dependent_refs_by_site.get(site, {}).get("owner_types", {})
            ),
            "retainer_hints": sorted_count_rows(
                retainer_hints_by_site.get(site, {}).get("owner_types", {})
            ),
            "sample_stack": vals["sample_stack"],
        }
        for site, vals in sorted(
            as3_live_by_site.items(),
            key=lambda kv: (kv[1]["bytes"], kv[1]["count"]),
            reverse=True,
        )
    ]

    for snap in snapshots:
        sampled_by_method = snap.pop("_sampled_live_by_method", {})
        snap["sampled_top_live_methods"] = [
            {"method_id": method_id, "count": vals[0], "bytes": vals[1]}
            for method_id, vals in sorted(
                sampled_by_method.items(),
                key=lambda kv: (kv[1][1], kv[1][0]),
                reverse=True,
            )[:50]
        ]

    as3_snap_diffs = as3_snapshot_diffs(as3_snapshot_summaries)
    post_native_gc_as3 = snapshot_by_label(as3_snapshot_summaries, "post-native-gc-pre-stop")

    result: dict[str, Any] = {
        "path": str(path),
        "file_size": len(raw),
        "header": header,
        "footer": footer,
        "event_count": event_count,
        "payload_bytes": payload_bytes,
        "counts": dict(sorted(counts.items())),
        "duration_ms": fmt_ms(capture_duration_ns),
        "overhead": {
            "event_region_bytes": event_region_bytes,
            "event_header_bytes": event_count * EVENT_HEADER_SIZE,
            "payload_bytes": payload_bytes,
            "events_per_sec": round(event_count / capture_duration_sec, 2) if capture_duration_sec else 0,
            "payload_bytes_per_sec": round(payload_bytes / capture_duration_sec, 2) if capture_duration_sec else 0,
            "event_region_bytes_per_sec": round(event_region_bytes / capture_duration_sec, 2) if capture_duration_sec else 0,
            "avg_event_bytes": round(event_region_bytes / event_count, 2) if event_count else 0,
            "dropped_ratio": (
                round(footer["dropped_count"] / (footer["dropped_count"] + event_count), 6)
                if footer["dropped_count"] + event_count else 0
            ),
        },
        "markers": markers,
        "marker_spans": sorted(marker_spans, key=lambda item: item["duration_ms"], reverse=True),
        "top_method_timing": top_method_timing,
        "live_allocations": len(live),
        "live_bytes": live_bytes,
        "unknown_frees": unknown_frees,
        "unknown_reallocs": unknown_reallocs,
        "as3_live_allocations": len(as3_live),
        "as3_live_bytes": as3_live_bytes,
        "as3_unknown_frees": as3_unknown_frees,
        "as3_reference_edges": len(as3_references),
        "as3_reference_edges_with_live_owner": as3_reference_edges_with_live_owner,
        "live_as3_reference_edges": len(live_reference_edges),
        "top_as3_reference_edges": [
            {"owner_type": key[0], "dependent_type": key[1], "count": count}
            for key, count in sorted(reference_type_edges.items(), key=lambda kv: kv[1], reverse=True)
        ][:50],
        "top_as3_reference_owners": [
            {"owner_type": key[0], "owner_site": key[1], "count": count}
            for key, count in sorted(reference_owner_edges.items(), key=lambda kv: kv[1], reverse=True)
        ][:50],
        "top_as3_direct_retained_owners": sorted_bytes_rows(direct_retained_by_owner, 50),
        "top_as3_retained_owners": sorted_bytes_rows(retained_by_owner, 50),
        "retained_graph": {
            "live_as3_nodes": len(as3_live),
            "live_as3_edges": len(live_reference_edges),
            "transitive_retained_available": bool(retained_by_owner),
        },
        "snapshots": snapshots,
        "snapshot_diffs": snapshot_diffs(snapshots),
        "as3_snapshot_summaries": as3_snapshot_summaries,
        "as3_snapshot_diffs": as3_snap_diffs,
        "post_native_gc_as3": post_native_gc_as3,
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
        "top_as3_memory_by_type": top_as3_memory_by_type,
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
    print(f"  duration        : {result['duration_ms']:>12.3f} ms")
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
    print(f"  AS3 ref edges   : {result['live_as3_reference_edges']:>6} live AS3-AS3 / "
          f"{result['as3_reference_edges_with_live_owner']:>6} live-owner / "
          f"{result['as3_reference_edges']:>6} total")
    overhead = result["overhead"]
    print(f"  event rate      : {overhead['events_per_sec']:>12.2f} events/s")
    print(f"  payload rate    : {overhead['payload_bytes_per_sec']:>12.2f} B/s")

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

    as3_growth = [
        item for item in result["as3_snapshot_diffs"]
        if item["as3_live_bytes_delta"] > 0 or item["as3_live_allocations_delta"] > 0
    ]
    if as3_growth:
        print("\nAS3 snapshot growth:")
        for item in as3_growth[-min(top, len(as3_growth)):]:
            top_types = [
                row for row in item["type_growth"]
                if row["delta_count"] > 0 or row["delta_bytes"] > 0
            ][:3]
            type_text = ", ".join(
                f"{row['type_name']} {row['delta_count']:+}/{row['delta_bytes']:+}B"
                for row in top_types
            )
            suffix = f" [{type_text}]" if type_text else ""
            print(
                f"  {item['from']} -> {item['to']}: "
                f"as3Count {item['as3_live_allocations_delta']:+} "
                f"as3Bytes {item['as3_live_bytes_delta']:+}{suffix}"
            )

    post_gc = result.get("post_native_gc_as3")
    if post_gc and post_gc.get("top_types"):
        print("\nPost-native-GC AS3 live types:")
        for item in post_gc["top_types"][:top]:
            print(
                f"  type={item['type_name']:<48} count={item['count']:<8} bytes={item['bytes']}"
            )

    spans = result["marker_spans"][:top]
    if spans:
        print("\nMarker spans:")
        for item in spans:
            print(f"  {item['name']:<48} {item['duration_ms']:>10.3f} ms")

    timed_methods = result["top_method_timing"][:top]
    if timed_methods:
        print("\nTop method timing:")
        for item in timed_methods:
            print(
                f"  {item['name']:<48} count={item['count']:<8} "
                f"incl={item['inclusive_ms']:.3f}ms excl={item['exclusive_ms']:.3f}ms "
                f"avg={item['avg_inclusive_ms']:.3f}ms"
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
            retainers = ", ".join(
                f"{hint['type_name']} x{hint['count']} @ {hint['site']}"
                for hint in item.get("retainer_hints", [])[:3]
            )
            if retainers:
                print(f"    retained by: {retainers}")
            if item.get("owned_dependent_refs", 0):
                print(f"    runtime dependent refs: {item['owned_dependent_refs']}")
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

    as3_memory = result["top_as3_memory_by_type"][:top]
    if as3_memory:
        print("\nTop AS3 memory by type:")
        for item in as3_memory:
            print(
                f"  type={item['type_name']:<48} count={item['count']:<8} "
                f"shallow={item['shallow_bytes']:<10} retained={item['retained_bytes']:<10} "
                f"runtimeRefs={item['runtime_dependent_refs']}"
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
            retainers = ", ".join(
                f"{hint['type_name']} x{hint['count']} @ {hint['site']}"
                for hint in item.get("retainer_hints", [])[:3]
            )
            if retainers:
                print(f"    retained by: {retainers}")
            if item.get("owned_dependent_refs", 0):
                print(f"    runtime dependent refs: {item['owned_dependent_refs']}")

    as3_refs = result["top_as3_reference_edges"][:top]
    if as3_refs:
        print("\nTop AS3 reference edges:")
        for item in as3_refs:
            print(
                f"  {item['owner_type']} -> {item['dependent_type']} count={item['count']}"
            )

    as3_ref_owners = result["top_as3_reference_owners"][:top]
    if as3_ref_owners:
        print("\nTop AS3 reference owners:")
        for item in as3_ref_owners:
            print(
                f"  type={item['owner_type']:<48} count={item['count']:<8} "
                f"site={item['owner_site']}"
            )

    retained = result["top_as3_retained_owners"][:top]
    if retained:
        print("\nTop AS3 retained owners:")
        for item in retained:
            print(
                f"  type={item['type_name']:<48} retainedCount={item['count']:<8} "
                f"retainedBytes={item['bytes']:<10} site={item['site']}"
            )

    direct_retained = result["top_as3_direct_retained_owners"][:top]
    if direct_retained and not retained:
        print("\nTop AS3 direct retained owners:")
        for item in direct_retained:
            print(
                f"  type={item['type_name']:<48} childCount={item['count']:<8} "
                f"childBytes={item['bytes']:<10} site={item['site']}"
            )

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


def row_map(rows: list[dict[str, Any]], key_fields: tuple[str, ...]) -> dict[tuple[Any, ...], dict[str, Any]]:
    return {tuple(row.get(field) for field in key_fields): row for row in rows}


def diff_rows(
    baseline_rows: list[dict[str, Any]],
    target_rows: list[dict[str, Any]],
    key_fields: tuple[str, ...],
    value_fields: tuple[str, ...],
    limit: int = 50,
) -> list[dict[str, Any]]:
    base = row_map(baseline_rows, key_fields)
    target = row_map(target_rows, key_fields)
    out: list[dict[str, Any]] = []
    for key in sorted(set(base) | set(target)):
        b = base.get(key, {})
        t = target.get(key, {})
        row: dict[str, Any] = {field: key[idx] for idx, field in enumerate(key_fields)}
        for field in value_fields:
            before = b.get(field, 0) or 0
            after = t.get(field, 0) or 0
            row[f"baseline_{field}"] = before
            row[f"target_{field}"] = after
            row[f"delta_{field}"] = after - before
        out.append(row)
    primary = f"delta_{value_fields[-1]}"
    secondary = f"delta_{value_fields[0]}"
    out.sort(key=lambda item: (item.get(primary, 0), item.get(secondary, 0)), reverse=True)
    return out[:limit]


def build_diff(baseline: dict[str, Any], target: dict[str, Any]) -> dict[str, Any]:
    baseline_post_gc = baseline.get("post_native_gc_as3") or {}
    target_post_gc = target.get("post_native_gc_as3") or {}
    return {
        "baseline_path": baseline["path"],
        "target_path": target["path"],
        "native_live_allocations_delta": target["live_allocations"] - baseline["live_allocations"],
        "native_live_bytes_delta": target["live_bytes"] - baseline["live_bytes"],
        "as3_live_allocations_delta": target["as3_live_allocations"] - baseline["as3_live_allocations"],
        "as3_live_bytes_delta": target["as3_live_bytes"] - baseline["as3_live_bytes"],
        "event_rate_delta": target["overhead"]["events_per_sec"] - baseline["overhead"]["events_per_sec"],
        "payload_rate_delta": target["overhead"]["payload_bytes_per_sec"] - baseline["overhead"]["payload_bytes_per_sec"],
        "as3_type_growth": diff_rows(
            baseline["top_as3_live_types"],
            target["top_as3_live_types"],
            ("type_name",),
            ("count", "bytes"),
        ),
        "as3_site_growth": diff_rows(
            baseline["top_as3_allocation_sites"],
            target["top_as3_allocation_sites"],
            ("site",),
            ("count", "owned_dependent_refs", "bytes"),
        ),
        "as3_stack_growth": diff_rows(
            baseline["top_as3_live_stacks"],
            target["top_as3_live_stacks"],
            ("type_name", "stack"),
            ("count", "bytes"),
        ),
        "post_gc_as3_type_growth": diff_rows(
            baseline_post_gc.get("top_types", []),
            target_post_gc.get("top_types", []),
            ("type_name",),
            ("count", "bytes"),
        ),
        "post_gc_as3_site_growth": diff_rows(
            baseline_post_gc.get("top_sites", []),
            target_post_gc.get("top_sites", []),
            ("site",),
            ("count", "bytes"),
        ),
        "method_timing_growth": diff_rows(
            baseline["top_method_timing"],
            target["top_method_timing"],
            ("method_id", "name"),
            ("count", "exclusive_ms", "inclusive_ms"),
        ),
    }


def print_diff_report(diff: dict[str, Any], top: int) -> None:
    print("=== .aneprof diff analysis ===")
    print(f"  baseline: {Path(diff['baseline_path']).name}")
    print(f"  target  : {Path(diff['target_path']).name}")
    print(f"  native live delta: count {diff['native_live_allocations_delta']:+} "
          f"bytes {diff['native_live_bytes_delta']:+}")
    print(f"  AS3 live delta   : count {diff['as3_live_allocations_delta']:+} "
          f"bytes {diff['as3_live_bytes_delta']:+}")
    print(f"  event rate delta : {diff['event_rate_delta']:+.2f} events/s")
    print(f"  payload delta    : {diff['payload_rate_delta']:+.2f} B/s")

    sections = [
        ("AS3 type growth", "as3_type_growth", "type_name", ("count", "bytes")),
        ("AS3 site growth", "as3_site_growth", "site", ("count", "bytes", "owned_dependent_refs")),
        ("Post-native-GC AS3 type growth", "post_gc_as3_type_growth", "type_name", ("count", "bytes")),
        ("Post-native-GC AS3 site growth", "post_gc_as3_site_growth", "site", ("count", "bytes")),
        ("Method timing growth", "method_timing_growth", "name", ("count", "inclusive_ms", "exclusive_ms")),
    ]
    for title, key, label_field, fields in sections:
        rows = [
            row for row in diff[key]
            if any(row.get(f"delta_{field}", 0) for field in fields)
        ][:top]
        if not rows:
            continue
        print(f"\n{title}:")
        for row in rows:
            label = row.get(label_field)
            deltas = " ".join(f"{field}={row.get(f'delta_{field}', 0):+}" for field in fields)
            print(f"  {label}: {deltas}")


def html_table(title: str, rows: list[dict[str, Any]], columns: list[str], limit: int) -> str:
    if not rows:
        return ""
    head = "".join(f"<th>{html.escape(col)}</th>" for col in columns)
    body_rows = []
    for row in rows[:limit]:
        cells = "".join(f"<td>{html.escape(str(row.get(col, '')))}</td>" for col in columns)
        body_rows.append(f"<tr>{cells}</tr>")
    return f"<section><h2>{html.escape(title)}</h2><table><thead><tr>{head}</tr></thead><tbody>{''.join(body_rows)}</tbody></table></section>"


def write_html_report(
    path: Path,
    result: dict[str, Any] | None,
    warnings: list[str],
    top: int,
    diff: dict[str, Any] | None = None,
) -> None:
    style = """
body{font-family:Segoe UI,Arial,sans-serif;margin:24px;color:#172026;background:#f7f9fb}
h1{font-size:24px;margin:0 0 16px}h2{font-size:18px;margin:24px 0 8px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:8px}
.metric{background:#fff;border:1px solid #d8e0e8;border-radius:6px;padding:10px}
.metric b{display:block;font-size:12px;color:#536271}.metric span{font-size:20px}
table{width:100%;border-collapse:collapse;background:#fff;border:1px solid #d8e0e8}
th,td{padding:7px 8px;border-bottom:1px solid #edf1f5;text-align:left;font-size:13px;vertical-align:top}
th{background:#eef3f7}.warn{color:#9a4b00}
"""
    parts = [f"<!doctype html><meta charset='utf-8'><title>.aneprof report</title><style>{style}</style>"]
    if diff:
        parts.append("<h1>.aneprof Diff Report</h1>")
        metrics = {
            "Native Live Bytes Delta": diff["native_live_bytes_delta"],
            "AS3 Live Bytes Delta": diff["as3_live_bytes_delta"],
            "AS3 Live Count Delta": diff["as3_live_allocations_delta"],
            "Payload Rate Delta": f"{diff['payload_rate_delta']:.2f} B/s",
        }
        parts.append("<div class='grid'>" + "".join(
            f"<div class='metric'><b>{html.escape(k)}</b><span>{html.escape(str(v))}</span></div>"
            for k, v in metrics.items()
        ) + "</div>")
        parts.append(html_table("AS3 Type Growth", diff["as3_type_growth"],
                                ["type_name", "delta_count", "delta_bytes", "baseline_count", "target_count"], top))
        parts.append(html_table("AS3 Site Growth", diff["as3_site_growth"],
                                ["site", "delta_count", "delta_bytes", "delta_owned_dependent_refs"], top))
        parts.append(html_table("Post-native-GC AS3 Type Growth", diff["post_gc_as3_type_growth"],
                                ["type_name", "delta_count", "delta_bytes", "baseline_count", "target_count"], top))
        parts.append(html_table("Post-native-GC AS3 Site Growth", diff["post_gc_as3_site_growth"],
                                ["site", "delta_count", "delta_bytes", "baseline_count", "target_count"], top))
        parts.append(html_table("Method Timing Growth", diff["method_timing_growth"],
                                ["name", "delta_count", "delta_inclusive_ms", "delta_exclusive_ms"], top))
    elif result:
        parts.append(f"<h1>.aneprof Report: {html.escape(Path(result['path']).name)}</h1>")
        metrics = {
            "Duration ms": result["duration_ms"],
            "Events": result["event_count"],
            "Event Rate": f"{result['overhead']['events_per_sec']:.2f}/s",
            "Native Live Bytes": result["live_bytes"],
            "AS3 Live Bytes": result["as3_live_bytes"],
            "AS3 Live Objects": result["as3_live_allocations"],
            "AS3 Ref Live Owner": result["as3_reference_edges_with_live_owner"],
            "Dropped": result["footer"]["dropped_count"],
        }
        parts.append("<div class='grid'>" + "".join(
            f"<div class='metric'><b>{html.escape(k)}</b><span>{html.escape(str(v))}</span></div>"
            for k, v in metrics.items()
        ) + "</div>")
        if warnings:
            parts.append("<section><h2>Warnings</h2><ul>" + "".join(
                f"<li class='warn'>{html.escape(w)}</li>" for w in warnings
            ) + "</ul></section>")
        parts.append(html_table("Leak Suspects", result["leak_suspects"],
                                ["confidence", "site", "count", "bytes", "owned_dependent_refs"], top))
        parts.append(html_table("AS3 Snapshot Growth", result["as3_snapshot_diffs"],
                                ["from", "to", "as3_live_allocations_delta", "as3_live_bytes_delta"], top))
        if result.get("post_native_gc_as3"):
            parts.append(html_table("Post-native-GC AS3 Live Types",
                                    result["post_native_gc_as3"].get("top_types", []),
                                    ["type_name", "count", "bytes"], top))
        parts.append(html_table("Top AS3 Memory By Type", result["top_as3_memory_by_type"],
                                ["type_name", "count", "shallow_bytes", "retained_bytes", "runtime_dependent_refs"], top))
        parts.append(html_table("Top AS3 Allocation Sites", result["top_as3_allocation_sites"],
                                ["site", "count", "bytes", "owned_dependent_refs"], top))
        parts.append(html_table("Top Method Timing", result["top_method_timing"],
                                ["name", "count", "inclusive_ms", "exclusive_ms", "avg_inclusive_ms"], top))
        parts.append(html_table("Marker Spans", result["marker_spans"],
                                ["name", "duration_ms", "start_value", "end_value"], top))
    path.write_text("\n".join(parts), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("file", type=Path)
    parser.add_argument("compare_file", nargs="?", type=Path)
    parser.add_argument("--diff", action="store_true", help="compare file against compare_file")
    parser.add_argument("--json", type=Path, help="write machine-readable summary")
    parser.add_argument("--html", type=Path, help="write a standalone HTML report")
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
        if args.diff:
            if args.compare_file is None:
                print("ERR: --diff requires baseline and target files", file=sys.stderr)
                return 2
            baseline, baseline_warnings = analyze(args.file)
            target, target_warnings = analyze(args.compare_file)
            diff = build_diff(baseline, target)
            print_diff_report(diff, max(0, args.top))
            if args.html:
                write_html_report(args.html, None, baseline_warnings + target_warnings, max(0, args.top), diff=diff)
            if args.json:
                args.json.write_text(
                    json.dumps(
                        {
                            "baseline": baseline,
                            "target": target,
                            "diff": diff,
                            "warnings": baseline_warnings + target_warnings,
                        },
                        indent=2,
                    ),
                    encoding="utf-8",
                )
            return 4 if baseline_warnings or target_warnings else 0

        result, warnings = analyze(args.file)
    except Exception as exc:
        print(f"ERR: {exc}", file=sys.stderr)
        return 2

    print_report(result, warnings, max(0, args.top), args.stack_frames)
    if args.html:
        write_html_report(args.html, result, warnings, max(0, args.top))
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
