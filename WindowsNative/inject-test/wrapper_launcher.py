"""Minimal wrapper-launcher POC for the transient .telemetry.cfg approach.

Instead of injecting a DLL, just write the cfg to the user profile,
launch the target EXE, sleep long enough for AIR to read the cfg
(~1s is plenty — init_telemetry fires during early Player::init), then
delete the cfg. Target keeps the sampler armed for the rest of its
lifetime (the cfg is read once at startup and the decisions are pinned).

This is the simplest possible "no hybrid" bridge: the cfg is never
persistent on disk, yet Adobe sees it during the exact window it needs
to wire the sampler.

Usage:
  python wrapper_launcher.py <target_exe>
"""
import os
import subprocess
import sys
import time


def main():
    target_exe = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 9999

    cfg_path = os.path.join(os.environ["USERPROFILE"], ".telemetry.cfg")
    cfg_body = (
        f"TelemetryAddress=127.0.0.1:{port}\r\n"
        "SamplerEnabled=true\r\n"
        "CPUCapture=true\r\n"
        "ScriptObjectAllocationTraces=true\r\n"
        "AllGCAllocationTraces=true\r\n"
        "GCAllocationTracesThreshold=1024\r\n"
    )

    # 1. Write cfg
    with open(cfg_path, "w", encoding="utf-8") as f:
        f.write(cfg_body)
    print(f"[wrap] wrote {cfg_path}")

    # 2. Launch target
    proc = subprocess.Popen([target_exe])
    print(f"[wrap] spawned pid={proc.pid}")

    # 3. Wait for Adobe AIR to read the cfg (init_telemetry fires within
    #    ~500ms of startup in the captive runtime on a modern box).
    time.sleep(3.0)

    # 4. Delete cfg — sampler stays armed regardless.
    try:
        os.remove(cfg_path)
        print(f"[wrap] deleted {cfg_path}")
    except OSError as e:
        print(f"[wrap] delete failed: {e}")

    # 5. Wait for target to exit.
    proc.wait()
    print(f"[wrap] target exit code = {proc.returncode}")


main()
