"""
flmc_validate.py — parse and validate a .flmc profiler capture.

Checks the header magic, extracts the header JSON, inflates the zlib body,
verifies the footer, and prints a short summary. Optionally writes the
inflated wire stream to a sidecar .bin for manual inspection (or feeding
into flash-profiler-core's analyze_dump).

Exit codes:
    0   valid file, non-trivial stream
    2   bad magic / malformed structure
    3   zlib inflate failed
    4   footer mismatch / trivial stream
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path


MAGIC_HEADER = b"FLMC"
MAGIC_FOOTER = b"FMFT"
MAGIC_END    = b"END!"
FOOTER_SIZE  = 64


def parse_footer(buf: bytes) -> dict:
    if len(buf) != FOOTER_SIZE:
        raise ValueError(f"footer size {len(buf)} != 64")
    # Layout from FileFormat.hpp (packed, little-endian):
    #   char magic[4]                      (+0)
    #   u16 version                        (+4)
    #   u16 reserved                       (+6)
    #   u64 total_bytes_raw                (+8)
    #   u64 total_bytes_compressed         (+16)
    #   u64 record_count                   (+24)
    #   u64 dropped_count                  (+32)
    #   u64 ended_utc                      (+40)
    #   u32 crc32_stream                   (+48)
    #   u32 reserved2                      (+52)
    #   char end_magic[4]                  (+56)
    #   char pad[4]                        (+60)
    magic = buf[0:4]
    version, reserved = struct.unpack_from("<HH", buf, 4)
    total_raw, total_comp, records, dropped, ended_utc = struct.unpack_from("<QQQQQ", buf, 8)
    crc32_stream, reserved2 = struct.unpack_from("<II", buf, 48)
    end_magic = buf[56:60]
    return {
        "magic": magic,
        "version": version,
        "total_bytes_raw": total_raw,
        "total_bytes_compressed": total_comp,
        "record_count": records,
        "dropped_count": dropped,
        "ended_utc": ended_utc,
        "crc32_stream": crc32_stream,
        "end_magic": end_magic,
    }


def parse_header(buf: bytes) -> tuple[int, dict]:
    if len(buf) < 12:
        raise ValueError("file too small for header")
    magic = buf[0:4]
    if magic != MAGIC_HEADER:
        raise ValueError(f"header magic {magic!r} != {MAGIC_HEADER!r}")
    version, reserved = struct.unpack_from("<HH", buf, 4)
    header_len, = struct.unpack_from("<I", buf, 8)
    if 12 + header_len > len(buf):
        raise ValueError("header_len exceeds file size")
    header_json_raw = buf[12 : 12 + header_len]
    try:
        header_json = json.loads(header_json_raw.decode("utf-8"))
    except Exception as e:
        raise ValueError(f"header JSON invalid: {e}")
    return 12 + header_len, header_json


def validate(path: Path, out_bin: Path | None = None, allow_partial: bool = False) -> int:
    raw = path.read_bytes()
    if len(raw) < 12:
        print(f"ERR: file too small ({len(raw)} B)", file=sys.stderr)
        return 2

    try:
        stream_offset, header_json = parse_header(raw)
    except ValueError as e:
        print(f"ERR: {e}", file=sys.stderr)
        return 2

    # Detect partial (kill-mid-capture) files: footer absent or malformed.
    is_partial = False
    footer = None
    if len(raw) >= stream_offset + FOOTER_SIZE:
        footer_raw = raw[-FOOTER_SIZE:]
        try:
            footer = parse_footer(footer_raw)
            if footer["magic"] != MAGIC_FOOTER or footer["end_magic"] != MAGIC_END:
                footer = None
                is_partial = True
        except ValueError:
            footer = None
            is_partial = True
    else:
        is_partial = True

    if is_partial:
        compressed = raw[stream_offset:]
    else:
        compressed = raw[stream_offset : len(raw) - FOOTER_SIZE]

    # Prefer streaming inflate — tolerant of truncated / SYNC-FLUSHed tails.
    d = zlib.decompressobj()
    try:
        inflated = d.decompress(compressed)
        try:
            inflated += d.flush(zlib.Z_SYNC_FLUSH)
        except zlib.error:
            pass
    except zlib.error as e:
        if not is_partial:
            print(f"ERR: zlib inflate failed: {e}", file=sys.stderr)
            return 3
        inflated = b""

    print(f"=== .flmc summary: {path.name} ===")
    if is_partial:
        print(f"  [PARTIAL — footer absent, likely killed mid-capture]")
    print(f"  file size           : {len(raw):>12} B")
    print(f"  header JSON length  : {stream_offset - 12:>12} B")
    print(f"  compressed stream   : {len(compressed):>12} B")
    print(f"  inflated stream     : {len(inflated):>12} B")
    if len(inflated) > 0:
        print(f"  ratio               :         {len(compressed) / len(inflated):.3f}")
    if footer is not None:
        print(f"  records             : {footer['record_count']:>12}")
        print(f"  dropped             : {footer['dropped_count']:>12}")
        print(f"  version             : {footer['version']}")
        print(f"  ended_utc           : {footer['ended_utc']}")
        print(f"  footer.total_raw    : {footer['total_bytes_raw']}")
        print(f"  footer.total_comp   : {footer['total_bytes_compressed']}")
    print(f"  header JSON         : {json.dumps(header_json, indent=2)}")

    ok = True
    if footer is not None and footer["total_bytes_raw"] != len(inflated):
        print(f"  WARN: footer.total_bytes_raw ({footer['total_bytes_raw']}) != "
              f"actual inflated ({len(inflated)})")
        ok = False
    if out_bin is not None:
        out_bin.write_bytes(inflated)
        print(f"  inflated stream written to: {out_bin}")

    if len(inflated) == 0:
        print("  WARN: stream is empty")
        ok = False

    if is_partial and not allow_partial:
        # Partial file detected but caller didn't opt-in — surface as exit 5
        # so CI can tell "this is a crash dump" vs "this is healthy".
        return 5 if ok else 4
    return 0 if ok else 4


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("file", type=Path, help="Path to .flmc")
    ap.add_argument("--out-bin", type=Path, default=None,
                    help="Optional: write inflated stream to this file")
    ap.add_argument("--allow-partial", action="store_true",
                    help="Accept files without a footer (kill-mid-capture scenario)")
    args = ap.parse_args()
    sys.exit(validate(args.file, args.out_bin, args.allow_partial))
