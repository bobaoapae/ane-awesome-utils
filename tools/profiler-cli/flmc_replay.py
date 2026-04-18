"""
flmc_replay.py — replay a .flmc capture as if it were a live Scout session.

Opens a TCP connection to <host>:<port> (default 127.0.0.1:7934) and streams
the inflated wire bytes. Adobe Scout or any compatible listener sees a
normal-looking session; timestamps in the stream are relative to its
SessionStart, so the replay appears live.

Two paces:
  --fast  (default): send chunks as fast as the peer accepts them
  --realtime       : TODO — parse message timestamps and sleep between
                     them so the replay unfolds at wall-clock pace

Usage:
  python flmc_replay.py capture.flmc --host 127.0.0.1 --port 7934
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import time
import zlib
from pathlib import Path

FOOTER_SIZE = 64
MAGIC_HEADER = b"FLMC"


def inflate_payload(path: Path) -> bytes:
    raw = path.read_bytes()
    if len(raw) < 12 + FOOTER_SIZE:
        raise SystemExit(f"file too small: {path}")
    if raw[:4] != MAGIC_HEADER:
        raise SystemExit("not a .flmc file (bad magic)")
    header_len = struct.unpack_from("<I", raw, 8)[0]
    stream_start = 12 + header_len
    stream = raw[stream_start : len(raw) - FOOTER_SIZE]
    return zlib.decompress(stream)


def send_fast(sock: socket.socket, payload: bytes, chunk: int = 64 * 1024):
    sent = 0
    n = len(payload)
    while sent < n:
        end = min(sent + chunk, n)
        sock.sendall(payload[sent:end])
        sent = end


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("file", type=Path)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7934)
    ap.add_argument("--fast", action="store_true", default=True,
                    help="send ASAP (default)")
    ap.add_argument("--chunk-kb", type=int, default=64)
    args = ap.parse_args()

    payload = inflate_payload(args.file)
    print(f"[replay] payload size = {len(payload)} bytes")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10.0)
    try:
        sock.connect((args.host, args.port))
    except OSError as e:
        print(f"[replay] connect failed: {e}", file=sys.stderr)
        return 2
    print(f"[replay] connected to {args.host}:{args.port}")

    t0 = time.monotonic()
    try:
        send_fast(sock, payload, chunk=args.chunk_kb * 1024)
    except OSError as e:
        print(f"[replay] send failed: {e}", file=sys.stderr)
        return 3
    dt = time.monotonic() - t0
    print(f"[replay] sent {len(payload)} B in {dt:.2f} s ({len(payload)/max(dt,1e-6)/1024:.0f} KB/s)")

    # Let the peer ingest before we close — Scout doesn't like abrupt RSTs.
    sock.shutdown(socket.SHUT_WR)
    try:
        while sock.recv(4096):
            pass
    except OSError:
        pass
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
