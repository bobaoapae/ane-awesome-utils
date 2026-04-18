# E2E Test Validation — 2026-04-18

**Status: PASS (Modo B completo)**

## What it validates

A fully automated test that, without any `.telemetry.cfg` on disk:

1. Builds the test SWF with `amxmlc`.
2. Generates a self-signed cert and packages a captive-runtime AIR bundle
   via `adt -target bundle -tsa none`. The bundle includes our ANE and
   an `Adobe AIR.dll` 51.1.3.10 x64 whose SHA matches our Mode B RVA map.
3. Launches the resulting `TestProfilerApp.exe`.
4. Inside the app (AS3):
   - Calls `AneAwesomeUtils.instance.profilerStart(path, {features...})`
   - Runs for 5 s with allocating-per-frame work to generate telemetry
   - Takes a marker every second
   - Calls `profilerStop()`
   - Writes a JSON result file
5. The harness verifies:
   - Exit code 0
   - `.flmc` file exists and is non-trivial
   - `flmc_validate.py` parses header + footer + inflates stream
   - Inflated stream is valid AMF3 (1525 objects parsed, 0 errors)

## Result (representative run)

```
hook output  (raw_capture.bin):       12713 B  (uncompressed)
.flmc file  size:                      4770 B  (zlib level 6)
ratio:                                 0.366
records:                                  17
dropped:                                   0
state transitions:                    Idle -> Recording (5s) -> Idle
modeBAvailable:                            1   (SHA sanity-check passed)
modeBActive:                          1 -> 0   (forced on, then off)
flash-profiler analyze_dump:           1525 AMF3 values, 0 parse errors
Scout wire types present:             .player, .tlm, .span, .value,
                                      .mem, .gc, .as, .swf
```

## Sequence that worked

1. ANE init (runs only once, no side effects yet — zero idle overhead).
2. AS3 `profilerStart({ features... })`:
   1. `WindowsAirRuntime::initialize()` — verifies runtime signature (12-byte
      prologue of `Player::init_telemetry`), caches 12 function pointers
      and GCHeap global.
   2. `tryCapturePlayer()` — reads `FRE::getActiveFrame_TLS`, walks the
      `frame→toplevel→pool→core[+0xac0]` chain to get `Player*`.
   3. `CaptureController::start()` — opens `.flmc`, writes header.
   4. `WindowsRuntimeHook::install()` — VirtualProtect + swap of the
      `ws2_32!send` slot in Adobe AIR.dll's IAT.
   5. `WindowsLoopbackListener::start(7934)` — accepts on 127.0.0.1
      so the runtime's `connect()` succeeds.
   6. `WindowsAirRuntime::forceEnableTelemetry(host, port, features)`:
      default_init TelemetryConfig → AirString::assign host → set flags →
      alloc_small SocketTransport → ctor → alloc_small Telemetry →
      ctor + bindTransport → CAS on GCHeap + alloc_locked PlayerTelemetry
      → ctor → write pointers to Player+0x1650/0x1658/0x1660 →
      TelemetryConfig::dtor.
3. Runtime emits `.player.*` handshake + `.player.enterframe` etc.;
   our IAT hook pushes bytes into the SPSC ring; writer thread compresses
   into `.flmc`.
4. AS3 `profilerStop()`:
   1. `forceDisableTelemetry()` — calls the three destructors in reverse,
      zeros the Player fields.
   2. `WindowsLoopbackListener::stop()` — closes loopback peer + listen.
   3. `CaptureController::stop()` — drains ring, flushes deflate, writes footer.

## Files

- `app/TestProfilerApp.as` — AS3 test driver
- `app/TestProfilerApp-app.xml` — AIR descriptor
- `app/build.bat` — amxmlc + self-sign + adt bundle
- `run_test.ps1` — harness (build if needed, launch, verify, diff)
- `output/TestProfilerApp/TestProfilerApp.exe` — packaged bundle
- `output/app_stdout.log` / `output/app_stderr.log` — last run's AS3 trace

## Re-running

```
pwsh -NoProfile -ExecutionPolicy Bypass -File run_test.ps1 [-Rebuild] [-KeepOutputs]
```

`-Rebuild` wipes the `output/` folder and rebuilds SWF + ANE + bundle from
scratch. `-KeepOutputs` leaves the captured `.flmc` and `test_result.json`
on disk instead of cleaning them up.
