from __future__ import annotations

import importlib.util
import json
import struct
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ANALYZER_PATH = ROOT / "tools" / "profiler-cli" / "aneprof_analyze.py"

spec = importlib.util.spec_from_file_location("aneprof_analyze", ANALYZER_PATH)
aneprof_analyze = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(aneprof_analyze)


HEADER_MAGIC = b"ANEPROF\x00"
FOOTER_MAGIC = b"ANEPEND\x00"
VERSION = 1

START = 1
STOP = 2
MARKER = 3
SNAPSHOT = 4
ALLOC = 7
FREE = 8
AS3_ALLOC = 12
AS3_FREE = 13
AS3_REFERENCE = 14
AS3_REFERENCE_EX = 15
AS3_ROOT = 16
AS3_PAYLOAD = 17
FRAME = 18
GC_CYCLE = 19

FLAG_INFERRED = 1
FLAG_REQUESTED = 2


def s(text: str) -> bytes:
    return text.encode("utf-8")


def event(event_type: int, payload: bytes = b"", timestamp_ns: int = 0, flags: int = 0) -> bytes:
    return struct.pack("<HHIQII", event_type, flags, len(payload), timestamp_ns, 7, 0) + payload


def marker(name: str, value: str = "null") -> bytes:
    nb = s(name)
    vb = s(value)
    return struct.pack("<II", len(nb), len(vb)) + nb + vb


def snapshot(label: str, live_allocs: int = 0, live_bytes: int = 0) -> bytes:
    lb = s(label)
    return struct.pack("<QQQQQQII", live_allocs, live_bytes, 0, 0, 0, 0, len(lb), 0) + lb


def alloc(ptr: int, size: int, method_id: int = 0, stack_id: int = 0) -> bytes:
    return struct.pack("<QQQQII", ptr, size, 0, 0, method_id, stack_id)


def as3_object(sample_id: int, size: int, type_name: str, stack: str = "") -> bytes:
    tb = s(type_name)
    sb = s(stack)
    return struct.pack("<QQII", sample_id, size, len(tb), len(sb)) + tb + sb


def as3_ref(owner_id: int, dependent_id: int) -> bytes:
    return struct.pack("<QQ", owner_id, dependent_id)


def as3_ref_ex(owner_id: int, dependent_id: int, kind: int, label: str = "") -> bytes:
    lb = s(label)
    return struct.pack("<QQHHI", owner_id, dependent_id, kind, 0, len(lb)) + lb


def as3_root(object_id: int, kind: int, label: str = "") -> bytes:
    lb = s(label)
    return struct.pack("<QHHI", object_id, kind, 0, len(lb)) + lb


def as3_payload(owner_id: int, payload_id: int, kind: int, logical: int, native: int, label: str = "") -> bytes:
    lb = s(label)
    return struct.pack("<QQQQHHI", owner_id, payload_id, logical, native, kind, 0, len(lb)) + lb


def frame_payload(index: int, duration_ns: int, alloc_bytes: int, alloc_count: int, label: str = "") -> bytes:
    lb = s(label)
    return struct.pack("<QQQII", index, duration_ns, alloc_bytes, alloc_count, len(lb)) + lb


def gc_cycle(gc_id: int, before_count: int, before_bytes: int, after_count: int, after_bytes: int, label: str = "") -> bytes:
    lb = s(label)
    return struct.pack("<QQQQQHHI", gc_id, before_bytes, after_bytes, before_count, after_count, 1, 0, len(lb)) + lb


def write_capture(events: list[bytes]) -> Path:
    header_json = json.dumps({"format": "aneprof", "test": True}).encode("utf-8")
    event_region = b"".join(events)
    header = struct.pack("<8sHHIQ", HEADER_MAGIC, VERSION, 0, len(header_json), 1234)
    footer = struct.pack(
        "<8sHHIQQQQQQII",
        FOOTER_MAGIC,
        VERSION,
        0,
        72,
        len(events),
        0,
        sum(len(item) - 24 for item in events),
        1235,
        0,
        0,
        0,
        0,
    )
    f = tempfile.NamedTemporaryFile(delete=False, suffix=".aneprof")
    path = Path(f.name)
    with f:
        f.write(header)
        f.write(header_json)
        f.write(event_region)
        f.write(footer)
    return path


def analyze_events(events: list[bytes]) -> dict:
    path = write_capture(events)
    try:
        result, warnings = aneprof_analyze.analyze(path)
    finally:
        path.unlink(missing_ok=True)
    assert warnings == [], warnings
    return result


class AneprofAnalyzeTests(unittest.TestCase):
    def test_retainer_path_and_exact_dominator(self) -> None:
        events = [
            event(START, timestamp_ns=1),
            event(AS3_ALLOC, as3_object(1, 10, "StageRoot", "Game/start"), 2),
            event(AS3_ALLOC, as3_object(2, 20, "Owner", "Game/makeOwner"), 3),
            event(AS3_ALLOC, as3_object(3, 30, "Child", "Game/makeChild"), 4),
            event(AS3_ROOT, as3_root(1, 1, "stage"), 5),
            event(AS3_REFERENCE_EX, as3_ref_ex(1, 2, 4, "display"), 6),
            event(AS3_REFERENCE, as3_ref(2, 3), 7),
            event(SNAPSHOT, snapshot("after-alloc"), 8),
            event(STOP, timestamp_ns=9),
        ]
        result = analyze_events(events)

        child_path = next(item for item in result["retainer_paths"] if item["object_id"] == 3)
        path_names = [
            node.get("root_kind", node.get("type_name"))
            for node in child_path["path"]
        ]
        self.assertEqual(path_names, ["stage", "StageRoot", "Owner", "Child"])
        self.assertEqual(result["dominator_summary"]["mode"], "exact")
        retained = {item["object_id"]: item for item in result["dominator_summary"]["top_objects"]}
        self.assertEqual(retained[1]["retained_bytes"], 60)
        self.assertEqual(retained[2]["retained_bytes"], 50)
        self.assertEqual(retained[3]["retained_bytes"], 30)

    def test_shared_fan_in_is_not_double_counted_as_retained(self) -> None:
        events = [
            event(START, timestamp_ns=1),
            event(AS3_ALLOC, as3_object(1, 10, "Root", "Game/root"), 2),
            event(AS3_ALLOC, as3_object(2, 20, "Left", "Game/left"), 3),
            event(AS3_ALLOC, as3_object(3, 30, "Right", "Game/right"), 4),
            event(AS3_ALLOC, as3_object(4, 40, "Shared", "Game/shared"), 5),
            event(AS3_ROOT, as3_root(1, 3, "static"), 6),
            event(AS3_REFERENCE, as3_ref(1, 2), 7),
            event(AS3_REFERENCE, as3_ref(1, 3), 8),
            event(AS3_REFERENCE, as3_ref(2, 4), 9),
            event(AS3_REFERENCE, as3_ref(3, 4), 10),
            event(STOP, timestamp_ns=11),
        ]
        result = analyze_events(events)

        retained = {item["object_id"]: item for item in result["dominator_summary"]["top_objects"]}
        self.assertEqual(retained[1]["retained_bytes"], 100)
        self.assertEqual(retained[2]["retained_bytes"], 20)
        self.assertEqual(retained[3]["retained_bytes"], 30)
        self.assertEqual(retained[4]["retained_bytes"], 40)

    def test_reference_ids_are_canonicalized_from_nearby_object_pointers(self) -> None:
        events = [
            event(START, timestamp_ns=1),
            event(AS3_ALLOC, as3_object(0x1000, 10, "Root", "Game/root"), 2),
            event(AS3_ALLOC, as3_object(0x2000, 20, "Child", "Game/child"), 3),
            event(AS3_ROOT, as3_root(0x1000, 1, "stage"), 4),
            event(AS3_REFERENCE, as3_ref(0x1000, 0x2010), 5),
            event(STOP, timestamp_ns=6),
        ]
        result = analyze_events(events)

        self.assertEqual(result["as3_reference_edges"], 1)
        self.assertEqual(result["as3_reference_id_aliases"]["dependent"], 1)
        self.assertEqual(result["live_as3_reference_edges"], 1)
        child_path = next(item for item in result["retainer_paths"] if item["object_id"] == 0x2000)
        self.assertEqual([node.get("type_name") for node in child_path["path"][1:]], ["Root", "Child"])

    def test_reference_kinds_are_inferred_for_known_runtime_edges(self) -> None:
        events = [
            event(START, timestamp_ns=1),
            event(AS3_ALLOC, as3_object(1, 10, "flash.utils::SetIntervalTimer", "Timer/start"), 2),
            event(AS3_ALLOC, as3_object(2, 20, "builtin.as$0::MethodClosure", "Timer/callback"), 3),
            event(AS3_ROOT, as3_root(1, 4, "timer"), 4),
            event(AS3_REFERENCE, as3_ref(1, 2), 5),
            event(STOP, timestamp_ns=6),
        ]
        result = analyze_events(events)

        self.assertEqual(result["as3_reference_inferred_typed_edges"], 1)
        self.assertEqual(result["top_as3_reference_kinds"][0]["kind"], "timer_callback")
        child_path = next(item for item in result["retainer_paths"] if item["object_id"] == 2)
        self.assertEqual(child_path["path"][-1]["edge_kind"], "timer_callback")

    def test_frame_events_are_filled_with_allocations_from_the_frame_interval(self) -> None:
        events = [
            event(START, timestamp_ns=1),
            event(FRAME, frame_payload(1, 10_000_000, 0, 0, "enterFrame"), 20_000_000),
            event(ALLOC, alloc(0x1234, 4096), 15_000_000),
            event(FREE, alloc(0x1234, 0), 20_500_000),
            event(STOP, timestamp_ns=21_000_000),
        ]
        result = analyze_events(events)

        self.assertEqual(result["frame_summary"]["frame_count"], 1)
        frame = result["allocation_rate"]["by_frame"][0]
        self.assertEqual(frame["event_allocation_count"], 1)
        self.assertEqual(frame["event_allocation_bytes"], 4096)
        self.assertEqual(frame["allocation_bytes"], 4096)

    def test_payload_lifetime_frame_and_gc_summaries(self) -> None:
        events = [
            event(START, timestamp_ns=1_000),
            event(MARKER, marker("work.start"), 2_000),
            event(AS3_ALLOC, as3_object(10, 16, "BitmapWrapper", "Game/makeBitmap"), 3_000),
            event(AS3_PAYLOAD, as3_payload(10, 99, 1, 4096, 8192, "bitmap"), 4_000),
            event(AS3_ALLOC, as3_object(20, 1024, ".mem.bitmap.data", "native/bitmap"), 5_000),
            event(AS3_ALLOC, as3_object(30, 32, "Temp", "Game/temp"), 5_500),
            event(SNAPSHOT, snapshot("pre-gc"), 6_000),
            event(FRAME, frame_payload(1, 20_000_000, 9216, 2, "enterFrame"), 7_000),
            event(GC_CYCLE, gc_cycle(1, 2, 10000, 1, 8000, "native"), 8_000, FLAG_REQUESTED),
            event(AS3_FREE, as3_object(30, 32, "Temp"), 9_000),
            event(SNAPSHOT, snapshot("run_post_gc"), 9_500),
            event(MARKER, marker("work.end"), 10_000),
            event(STOP, timestamp_ns=11_000),
        ]
        result = analyze_events(events)

        owner_payload = next(item for item in result["payload_by_owner"] if item["owner_type"] == "BitmapWrapper")
        self.assertEqual(owner_payload["known_bytes"], 8192)
        self.assertEqual(owner_payload["payloads"][0]["kind"], "bitmap_data")
        self.assertTrue(any(item["owner_type"] == "<unowned>" for item in result["payload_by_owner"]))
        self.assertEqual(result["frame_summary"]["slow_frame_count"], 1)
        self.assertEqual(result["gc_summary"]["requested_count"], 1)
        self.assertEqual(result["post_native_gc_as3"]["label"], "run_post_gc")
        self.assertEqual(result["gc_summary"]["survivor_as3_live_bytes"], 1040)
        self.assertEqual(result["lifetime_summary"]["as3_freed_objects"], 1)
        self.assertGreaterEqual(result["allocation_rate"]["by_marker"][0]["allocation_bytes"], 8192)

    def test_old_aneprof_without_new_events_still_has_new_json_fields(self) -> None:
        events = [
            event(START, timestamp_ns=1),
            event(AS3_ALLOC, as3_object(1, 10, "LegacyLive", "Game/legacy"), 2),
            event(SNAPSHOT, snapshot("legacy"), 3),
            event(STOP, timestamp_ns=4),
        ]
        result = analyze_events(events)

        self.assertEqual(result["counts"].get("as3_root", 0), 0)
        self.assertIn("retainer_paths", result)
        self.assertIn("dominator_summary", result)
        self.assertIn("payload_by_owner", result)
        self.assertIn("lifetime_summary", result)
        self.assertEqual(result["retainer_paths"][0]["path"][0]["root_kind"], "unknown")
        self.assertTrue(result["retainer_paths"][0]["path"][0]["inferred"])


if __name__ == "__main__":
    unittest.main()
