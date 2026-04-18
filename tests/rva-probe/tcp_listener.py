"""
Minimal TCP listener for the RVA validation probe.

Accepts a single TCP connection on 127.0.0.1:PORT, writes everything it
receives to OUT_PATH as raw bytes, and keeps the connection open until the
peer closes it or a small idle timeout fires.

Intentionally simple: we don't parse AMF3 here. Parsing happens later by
replaying the dump through flash-profiler-core's decoder.
"""

from __future__ import annotations

import argparse
import socket
import sys
import time
from pathlib import Path


def run(port: int, out_path: Path, idle_timeout_s: float) -> int:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    srv.settimeout(60.0)  # wait up to 60s for first connection

    print(f"[listener] bound to 127.0.0.1:{port}, waiting for AIR runtime...", flush=True)
    try:
        conn, peer = srv.accept()
    except socket.timeout:
        print("[listener] ERROR: no connection in 60s — runtime never tried Scout", file=sys.stderr)
        return 2

    print(f"[listener] got connection from {peer}, writing to {out_path}", flush=True)
    total = 0
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "wb") as fh:
        conn.settimeout(idle_timeout_s)
        try:
            while True:
                data = conn.recv(65536)
                if not data:
                    print(f"[listener] peer closed (read {total} bytes total)", flush=True)
                    break
                fh.write(data)
                fh.flush()
                total += len(data)
                if total >= 1024 and total % (64 * 1024) < 65536:
                    print(f"[listener] {total} bytes received", flush=True)
        except socket.timeout:
            print(f"[listener] idle for {idle_timeout_s}s, assuming done ({total} bytes)", flush=True)
        finally:
            conn.close()
            srv.close()
    print(f"[listener] wrote {total} bytes to {out_path}", flush=True)
    return 0 if total > 0 else 3


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=17934)
    ap.add_argument("--out", default="scout_bytes.bin")
    ap.add_argument("--idle-timeout", type=float, default=5.0,
                    help="seconds of silence before assuming session is done")
    args = ap.parse_args()
    sys.exit(run(args.port, Path(args.out), args.idle_timeout))
