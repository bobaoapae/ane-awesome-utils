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
- `render:Boolean` default `false`
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
- optional render frame summaries from native Present hooks

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
| 20 | `as3_reference_remove` | no | factual removal of a previously emitted typed AS3 edge |
| 21 | `render_frame` | no | native render interval summary around a D3D/DXGI Present |

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
- `as3_reference_id_aliases` plus live-owner/live-dependent reference counters;
  the analyzer canonicalizes small x86/x64 object/header pointer offsets so
  `addDependentObject` callbacks can become AS3-AS3 edges when both objects are
  still live
- `as3_reference_real_typed_edges` and `top_as3_reference_kinds` for typed
  edges that came from factual `as3_reference_ex` events
- `active_as3_reference_ex_edges` and `as3_reference_remove_edges` so real
  add/remove hooks do not leave stale retainer edges in the final graph
- `as3_reference_inferred_typed_edges` and
  `top_as3_reference_inferred_kinds` for conservative typed-edge suggestions
  (`timer_callback`, `event_listener`, `array`, `dictionary`,
  `display_child`) when the runtime only emitted a base `as3_reference`;
  these suggestions are not used as factual retainer path edge kinds
- `payload_by_owner` for BitmapData/ByteArray/native payload bytes attached to
  AS3 owners, plus inferred unowned pseudo-payloads such as `.mem.bitmap.data`
- `lifetime_summary`, `allocation_rate`, `frame_summary`, `gc_summary` and
  `performance_suspects`
- `render_frames` and `render_frame_summary` for native frame intervals,
  Present time, CPU time between Presents, draw/blit counters, texture
  upload/create counters and slow/present-bound/upload-heavy frame suspects

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

The runtime may report dependent pointers using a nearby object body/allocation
address instead of the exact `recordNewObjectAllocation` object pointer. The
native hook and analyzer both canonicalize a small fixed set of verified x86/x64
offsets before storing or consuming reference edges. This keeps old captures
readable while improving retainer paths for new and re-analyzed `.aneprof`
files.

The runtime `IMemorySampler.addDependentObject` callback proves that an owner
depends on another object, but it does not identify the source of that edge.
For that reason the native hook does not write inferred `as3_reference_ex`
events from this callback. The analyzer may still report type-based suggestions
for triage, but retainer paths keep `edge_kind=unknown` unless a real
`as3_reference_ex` event exists.

Real typed edges come from hooks at the operation that creates or removes the
relationship. Windows x64 and x86 AIR `51.1.3.10` currently hooks
`DisplayObjectContainer.addChild/addChildAt/removeChild/removeChildAt` for
`display_child`, and `EventDispatcher.addEventListener/removeEventListener` for
`event_listener`. Non-weak event listeners are recorded as factual edges;
weak listeners are skipped because they should not retain the listener. Listener
identity includes event type and capture/bubble phase, so removing one
registration does not hide another registration that still retains the same
listener. If the dispatcher is sampled as a timer-like type, the event-listener
edge is recorded as `timer_callback`. Display-list reparenting emits a removal
for the previously observed parent when the add hook sees the child move. The
matching removal operations emit `as3_reference_remove`, and the analyzer
replays those mutations before building retainer paths and dominator summaries.

Hooks for static fields, array/dictionary slot writes, closure capture slots and
native payload owner links still require separate verified write-path hooks.
Until those exist, the analyzer keeps such edge kinds as conservative
suggestions instead of factual retainer-path labels.

Frame summaries can be supplied explicitly through `profilerRecordFrame()`.
The AS3 test bridge uses this to write one `frame` event per `ENTER_FRAME` when
`frameEvents: true` is passed to `profilerStart` options. The frame payload
contains the measured frame duration; the analyzer fills `allocation_rate.by_frame`
by scanning allocation events whose timestamps fall inside that frame interval.

## Render Performance Hooks

`profilerStart(..., { render: true })` enables optional Windows render
instrumentation. This is intentionally disabled by default and has no active
runtime hook until the first render-enabled capture. `profilerStop()` pauses
render capture, clears the active controller and stops writing events
immediately. The physical vtable restore is deferred to extension shutdown to
avoid racing AIR's renderer while it still owns live D3D/DXGI objects.

On AIR SDK `51.1.3.10`, the hook currently installs the stable Present paths:

- D3D9/9Ex `CreateDevice`, `CreateDeviceEx`, device `Present`, `PresentEx`,
  swap-chain `Present`, `GetSwapChain`, `CreateAdditionalSwapChain`
- D3D9 texture creation/update, render-target changes, clears and draw calls
- DXGI `Present` / `Present1`
- D3D11 texture creation, `UpdateSubresource`, draw calls, render-target
  changes and clears, discovered from a dummy D3D11 device/context

The profiler writes one `render_frame` event per observed Present instead of
one event per draw call. The event aggregates counters since the previous
Present:

- frame interval and Present call duration
- estimated CPU time between Present calls
- draw/blit calls and primitive estimates when the backend goes through hooked
  D3D9/D3D11 methods
- texture/surface create and upload byte estimates when dimensions/formats are
  known
- render-target changes and clears

Known limits:

- The CPU time field is `interval - present`, so it is a frame-interval signal,
  not an exact AS3/display-list CPU attribution.
- Draw/upload counters are backend dependent. The x64 and x86 E2E scenarios
  currently observe real Presents and emit render frames, but AIR does not
  route that synthetic scene through the hooked D3D9/D3D11 draw/upload slots,
  so draw/upload counters are zero in that smoke test.
- If a runtime uses an unhooked renderer path, `renderDiagnosticsReady` can be
  true while `renderHookPresentCalls` or `renderHookFrames` stays zero; this is
  exposed in `profilerGetStatus()` and the `.aneprof` remains valid.

AIR exposes a single active `IMemorySampler` slot. If the SWF/runtime already
owns that slot, for example through `flash.sampler.startSampling()`,
`setSamplerCallback()`, or Scout/advanced telemetry creating Adobe's
`MemoryTelemetrySampler`, the ANE now installs a chained sampler instead of
aborting AS3 diagnostics. The normal AIR `attachSampler` helper only fills an
empty slot, so when a sampler is already present the ANE resolves the slot used
by `getSampler()` and writes its proxy there directly. The chained sampler
records `.aneprof` AS3 alloc/free/reference events and forwards each callback
to the sampler that was already installed; `profilerStop()` restores the
previous sampler pointer.

Check `profilerGetStatus()` during a capture:

- `as3LeakDiagnosticsReady=true` means the AS3 object sampler is active.
- `as3ObjectHookChainedSampler=true` means the ANE is proxying another
  sampler, so `as3ObjectHookForwardedCalls` should grow as callbacks arrive.
- `as3ObjectHookDirectSlotInstalls>0` means `attachSampler` would not replace
  the occupied AIR slot and the direct slot fallback was used.
- `as3SamplerSlotPtrHex` is the resolved AIR sampler slot while the hook is
  installed.
- `as3SamplerPreviousVtableModule` and `as3SamplerPreviousVtableHead` identify
  the sampler that occupied the AIR slot before the ANE installed its proxy.
- `as3RealEdgeHookInstalls` should be `6` when the display-list and
  event-listener hook set installed successfully.
- `as3RealEdgeHookFailures` and `as3RealEdgeLastFailureStage` identify optional
  real-edge hook install failures. AS3 allocation diagnostics may still run if
  only one of these optional hooks fails.
- `as3RealDisplayChildEdges`, `as3RealDisplayChildRemoves`,
  `as3RealEventListenerEdges` and `as3RealEventListenerRemoves` count factual
  typed add/remove edges captured during the run.
- `memoryLeakDiagnosticsReady=true` with `as3LeakDiagnosticsReady=false` now
  indicates a lower-level attach/prologue failure rather than normal sampler
  contention.
- `renderDiagnosticsReady=true` means optional render hooks installed.
- `renderHookPresentCalls` and `renderHookFrames` should grow during a
  render-enabled capture when a hooked Present path is active.
- `renderHookDrawCalls`, `renderHookPrimitiveCount`,
  `renderHookTextureUploadBytes` and `renderHookTextureCreateBytes` grow only
  when the active AIR renderer uses a hooked D3D9/D3D11 operation that exposes
  those details. They are expected to stay zero for the current E2E renderer
  even though Present timing is captured.

## Validation

Use:

```powershell
python tools\profiler-cli\aneprof_validate.py path\to\capture.aneprof
python tools\profiler-cli\aneprof_analyze.py path\to\capture.aneprof --require-free-events
```

The E2E harness runs scenarios A/B/P/C/R/E/T/M/S/L for both architectures:

```powershell
powershell -ExecutionPolicy Bypass -File tests\profiler-e2e\run_test.ps1 -Arch x64
powershell -ExecutionPolicy Bypass -File tests\profiler-e2e\run_test.ps1 -Arch x64 -D3D d3d9
powershell -ExecutionPolicy Bypass -File tests\profiler-e2e\run_test.ps1 -Arch x86
```

Current standard coverage is A/B/P/C/R/E/T/M/S/L. Scenario D is an optional
kill-test and only runs with `-WithKillTest`.

Scenario C validates timing + memory with real runtime allocation/free pairing.
Scenario P validates render-enabled captures by requiring native Present calls,
`render_frame` events and `render_frame_summary` output. The harness injects
`<windows><maxD3D>14</maxD3D></windows>` by default, matching the client D3D11
path. Pass `-D3D d3d9` to generate `<maxD3D>9</maxD3D>` and validate the D3D9
path.
Scenario R validates the real AS3 typed edge hooks by requiring display-list and
event-listener add/remove counters plus active `as3_reference_ex` /
`as3_reference_remove` analyzer output.
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
powershell -ExecutionPolicy Bypass -File tests\profiler-e2e\run_test.ps1 -Arch x64 -KeepOutputs
powershell -ExecutionPolicy Bypass -File tests\profiler-e2e\run_test.ps1 -Arch x86 -KeepOutputs
```

After packaging, verify the Windows DLL signatures with:

```powershell
Get-AuthenticodeSignature AneBuild\windows-32\AneAwesomeUtilsWindows.dll,AneBuild\windows-64\AneAwesomeUtilsWindows.dll,AneBuild\windows-32\AwesomeAneUtils.dll,AneBuild\windows-64\AwesomeAneUtils.dll
```
