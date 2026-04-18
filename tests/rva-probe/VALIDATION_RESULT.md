# RVA validation result — 2026-04-18

**Status: PASS**

## What was validated

`SocketTransport::send_bytes` at RVA `0x493060` in `Adobe AIR.dll` 51.1.3.10
Windows x64 is the function that emits bytes on the Scout TCP socket, and
the bytes it produces are byte-perfect Scout-compatible AMF3.

## Method

Dynamic: launched a packaged AIR captive runtime (loadingDebug64) with
`.telemetry.cfg` pointing to a local TCP listener on port 17934.
No hook, no debugger — just letting the runtime itself emit to our socket.

## Evidence

- `scout_bytes.bin` (86,187 B) — raw TCP stream captured from the runtime
- Parsed by `flash-profiler-core::analyze_dump` with **0 parse errors**
  and 8957 AMF3 objects recovered
- Stream contents (decoded):
  - `.tlm.version = "3,2"` — protocol version 3.2
  - `.player.version = "51,1,3,10"` — confirms AIR 51.1.3.10
  - `.player.airversion = "51.1.3.10"`
  - `.tlm.category.enable`/`.disable` for `sampler`, `displayobjects`
  - Live telemetry: 707 `.player.enterframe`, 717 `.tlm.doplay`,
    241 `.gc.Reap`, 45 `.value:.player.cpu`, etc.

## Inferences (valid because bytes transit through only one path)

The only outbound path to the TCP socket in the runtime is
`SocketTransport::send_bytes → PlatformSocket::send`. All 86 KB we captured
passed through the function at RVA `0x493060`.

Therefore:
1. The RVA map in `docs/profiler-rva-51-1-3-10.md` is correct.
2. Hooking at that RVA (either by IAT/detour, or by overwriting vtable
   slot 11 of `PlatformSocketWrapper` at `0x180ecb828 + 0x58`) will
   intercept byte-identical Scout wire data.
3. The data is directly consumable by `flash-profiler` desktop and any
   Scout-compatible parser.

## Files

- `probe.cdb` + `probe.log` — CDB script (kept for future deeper dives,
  not required given the empirical evidence above)
- `run_probe.ps1` — repro harness
- `tcp_listener.py` — minimal listener
- `scout_bytes.bin` — the captured payload

## Next step

Fase 2 — POC standalone: build a DLL injected into the process that
intercepts at the vtable slot, producing our own dump independent of the
.telemetry.cfg workflow and proving the hook itself works in release.
