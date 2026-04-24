# Deep Profiler `.aneprof`

Current profiler direction:

- Primary backend is `.aneprof`, not Scout `.flmc`.
- Windows x86 and x64 are supported on AIR SDK `51.1.3.10`.
- Existing Scout/telemetry docs, scripts and RVA research remain in the repo as legacy analysis material.
- The SDK DLLs should remain original; `fix_telemetry_mode_b` is not required by this backend.

## Public API

```actionscript
profilerStart(path:String, options:Object = null):Boolean
profilerStop():Boolean
profilerSnapshot(label:String = null):Boolean
profilerMarker(name:String, value:* = null):Boolean
profilerRequestGc():Boolean
profilerGetStatus():Object
```

`profilerStart` writes a native binary event stream to `path`. Recommended
extension is `.aneprof`.

`profilerRequestGc` is Windows-only and requests MMgc collection through AIR's
native `needsCollection` flag. It replaces the old E2E reliance on `System.gc()`;
see `docs/profiler-native-gc.md`.

Supported options:

- `timing:Boolean` default `true`
- `memory:Boolean` default `false`
- `snapshots:Boolean` default `true`
- `snapshotIntervalMs:uint` default `0`
- `maxLiveAllocationsPerSnapshot:uint` default `4096`
- `metadata:Object` copied into the file header JSON
- `headerJson:String` full header JSON override

## Event Model

The file starts with `ANEPROF\0`, a versioned header and UTF-8 JSON metadata.
It then stores fixed event headers plus typed payloads:

- start/stop
- marker
- snapshot
- method enter/exit
- alloc/free/realloc
- live allocation entries
- method table
- AS3 alloc/free entries with runtime type name and AS3 stack
- AS3 reference/dependent edges from the runtime memory sampler
- optional AS3 typed references, roots and native payload ownership
- optional frame summaries and requested/observed GC cycle summaries

The footer uses `ANEPEND\0` and records event counts, payload bytes, dropped
events, final live allocations and final live bytes.

Event IDs are append-only. Current captures still use format version `1`; older
files that stop at `as3_reference` remain valid, and readers must treat the
new graph/payload/frame/GC events as optional enrichment.

Append-only event IDs currently reserved by the `.aneprof` backend:

| ID | Event | Required | Purpose |
| ---: | --- | --- | --- |
| 1-14 | existing base events | yes | start/stop, markers, snapshots, native allocations, AS3 alloc/free/reference |
| 15 | `as3_reference_ex` | no | reference edge with edge kind, label and inferred flag |
| 16 | `as3_root` | no | known or inferred AS3 root such as stage, static, timer, dispatcher or native |
| 17 | `as3_payload` | no | native/logical payload bytes owned by an AS3 object |
| 18 | `frame` | no | frame or interval duration plus allocation count/bytes |
| 19 | `gc_cycle` | no | requested or observed GC cycle counters before/after collection |

## Probe Surface

The native ANE exposes internal FRE functions for generated code:

- `awesomeUtils_profilerProbeEnter(methodId:uint)`
- `awesomeUtils_profilerProbeExit(methodId:uint)`
- `awesomeUtils_profilerRegisterMethodTable(bytes:ByteArray)`

A custom compiler pass should add `--profile-probes`, emit a stable method
table once, and inject balanced enter/exit calls around AS3 methods while
preserving returns, throws, finally blocks, constructors and closures.

## Memory Hooks

Windows currently installs guarded detours for these AIR/MMgc entry points:

| Hook | x64 RVA | x86 RVA |
| --- | ---: | ---: |
| `MMgcAllocSmall` | `0x001a0a64` | `0x0014f323` |
| `MMgcAllocLocked` | `0x001ab200` | `0x001573de` |
| `MMgcFree` body | `0x001ab379` | `0x00157526` |

It also patches the `Adobe AIR.dll` IAT entries for:

| Import | x64 IAT RVA | x86 IAT RVA |
| --- | ---: | ---: |
| `Kernel32!HeapAlloc` | `0x00b04b70` | `0x008c2584` |
| `Kernel32!HeapFree` | `0x00b04b68` | `0x008c2588` |
| `Kernel32!HeapReAlloc` | `0x00b049c0` | `0x008c22f0` |

## CLI Diagnostics

`tools/profiler-cli/aneprof_analyze.py` reconstructs native and AS3 live state
from raw events. The JSON output includes:

- `snapshots` and `snapshot_diffs` for native allocation counters
- `as3_snapshot_summaries` and `as3_snapshot_diffs` for AS3 type/site/stack
  growth at each snapshot point
- `post_native_gc_as3` for objects still live after `profilerRequestGc()`
- `top_as3_memory_by_type`, `top_as3_allocation_sites`,
  `top_as3_live_stacks`, reference-owner tables and leak suspects
- `retainer_paths` with the shortest known path from a root to live AS3 objects
- `dominator_summary`, exact for small graphs and marked partial for large
  fan-in approximations
- `payload_by_owner` for BitmapData/ByteArray/native payload bytes attached to
  AS3 owners, plus inferred unowned pseudo-payloads such as `.mem.bitmap.data`
- `lifetime_summary`, `allocation_rate`, `frame_summary`, `gc_summary` and
  `performance_suspects`

The inline detours are guarded by byte signatures from `Adobe AIR.dll`
`51.1.3.10`, and the IAT patches verify the expected slot RVAs. Broad IAT
hooks only emit events for pointers already tracked, or for new HeapAlloc
returns not seen by the narrower MMgc hooks, so AIR-internal frees do not flood
the report as unknown frees.

The Windows AS3 object hook also attaches a runtime `IMemorySampler`
implementation. It records AS3 object allocations/frees with the runtime type
name and the current AS3 stack without requiring compiler-injected probes. Its
`addDependentObject` callback is stored as `as3_reference` events. In current
AIR `51.1.3.10` captures, these are often runtime dependent refs owned by live
AS3 objects rather than a full AS3-to-AS3 retainer graph, so the analyzer reports
both:

- live AS3-to-AS3 reference edges when both sides are sampled AS3 objects;
- live-owner dependent refs grouped by AS3 type and allocation site.

## Validation

Use:

```powershell
python tools\profiler-cli\aneprof_validate.py path\to\capture.aneprof
python tools\profiler-cli\aneprof_analyze.py path\to\capture.aneprof --require-free-events
```

The E2E harness runs scenarios A/B/C/E/T/M/S/L for both architectures:

```powershell
powershell -ExecutionPolicy Bypass -File tests\profiler-e2e\run_test.ps1 -Arch x64
powershell -ExecutionPolicy Bypass -File tests\profiler-e2e\run_test.ps1 -Arch x86
```

Current standard coverage is A/B/C/E/T/M/S/L. Scenario D is an optional
kill-test and only runs with `-WithKillTest`.

Scenario C validates timing + memory with real runtime allocation/free pairing.
Scenario E simulates a hidden listener leak and validates AS3 type, stack,
suspect and runtime dependent-ref reporting. Scenario T covers timer/closure
retention, M covers explicit closure capture, S covers static display cache
retention with BitmapData/ByteArray payloads, and L adds deterministic retained
allocation records so the analyzer can assert that the leak path reports a
larger final live set than the baseline.

Validation commands used for the deep graph/payload analyzer change:

```powershell
python -m unittest discover tests\profiler-cli -v
cmd /c shared\profiler\build.bat
python build-all.py windows-native
python build-all.py package
powershell -ExecutionPolicy Bypass -File tests\profiler-e2e\run_test.ps1 -Arch x64 -SkipBuild -KeepOutputs
powershell -ExecutionPolicy Bypass -File tests\profiler-e2e\run_test.ps1 -Arch x86 -SkipBuild -KeepOutputs
```

After packaging, verify the Windows DLL signatures with:

```powershell
Get-AuthenticodeSignature AneBuild\windows-32\AneAwesomeUtilsWindows.dll,AneBuild\windows-64\AneAwesomeUtilsWindows.dll,AneBuild\windows-32\AwesomeAneUtils.dll,AneBuild\windows-64\AwesomeAneUtils.dll
```
