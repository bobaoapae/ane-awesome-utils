# Native GC request for aneprof

The profiler E2E harness must not depend on `System.gc()` or
`System.pauseForGCIfCollectionImminent()`: AIR warns that those AS3 calls are
advisory and they are not reliable enough for leak diagnostics.

The native path mirrors AIR's internal `.player.gc` telemetry command:

- x64 analysis: the `.player.gc` handler reads `PlayerTelemetry+0x58` to get
  `Player*`, reads `Player+0x48` to get `MMgc::GC*`, then writes
  `GC+0x168 = 1`.
- x86 analysis: binary pattern analysis identified the equivalent
  `needsCollection` flag at `GC+0x150`. The x86 FRE frame chain already
  captures `AvmCore`, so the ANE reads `AvmCore+0x04` to get `MMgc::GC*`.

`AneAwesomeUtils.profilerRequestGc()` exposes this as a Windows-only native
request. It validates the candidate GC object and the writable flag byte before
writing, then leaves collection scheduling to AIR/MMgc's normal tick. It does
not call `GC::Collect` directly.

The E2E leak scenarios now call this native request after releasing non-leak
test objects and before the final snapshot:

1. snapshot `pre-release`
2. release disposable pools
3. `profilerRequestGc()`
4. wait briefly for AIR's normal collection tick
5. snapshot `post-native-gc-pre-stop`
6. `profilerStop()`

Status fields exposed by `profilerGetStatus()`:

- `nativeGcAvailable`
- `nativeGcRequestCount`
- `nativeGcLastFailure`
- `nativeGcPending`
- `nativeGcPtr`
