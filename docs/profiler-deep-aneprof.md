# Deep Profiler `.aneprof`

Current profiler direction:

- Primary backend is `.aneprof`, not Scout `.flmc`.
- **Windows x86/x64 and Android arm64-v8a + armeabi-v7a are all supported** on
  AIR SDK `51.1.3.10`. The Android side reaches Windows-equivalent telemetry
  granularity end-to-end (see "Android implementation" below). iOS support is
  the alloc-tracer libc shadowhook only — no MMgc/render hooks yet.
- Existing Scout/telemetry docs, scripts and RVA research remain in the repo
  as legacy analysis material.
- The SDK DLLs should remain original; `fix_telemetry_mode_b` is not required
  by this backend.
- For day-to-day capture commands, AS3 examples and CLI usage, start with
  `docs/profiler-usage.md`.
- For libCore.so reverse-engineering details on Android, see
  `docs/profiler-rva-android-51-1-3-10.md`.

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

`profilerRequestGc` requests an MMgc collection.

- **Windows**: writes AIR's native `needsCollection` flag (see
  `docs/profiler-native-gc.md`).
- **Android**: re-invokes the captured `MMgc::GC::Collect` directly via the
  `AndroidGcHook`-stashed singleton pointer (the GC observer hook stashes
  `this` from the first runtime collection and uses it as the programmatic
  trigger entry point). No global pointer hunting needed.

Both paths replace the old E2E reliance on `System.gc()`.

Supported `profilerStart` options:

| Option | Default | Notes |
| --- | ---: | --- |
| `timing:Boolean` | `true` | Markers, method enter/exit and timing summaries. |
| `memory:Boolean` | `false` | Native memory hooks. AS3 object diagnostics require `memory=true`. |
| `render:Boolean` | `false` | Optional D3D/DXGI render frame summaries. |
| `snapshots:Boolean` | `true` | Initial/final/manual snapshots. |
| `snapshotIntervalMs:uint` | `0` | `0` means manual snapshots only. |
| `maxLiveAllocationsPerSnapshot:uint` | `4096` | Caps native `live_allocation` rows per snapshot. |
| `metadata:Object` | none | Copied into the generated header JSON. |
| `headerJson:String` | generated | Full header JSON override. |
| `as3ObjectSampling:Boolean` | `true` | Native `IMemorySampler` AS3 alloc/free/reference callbacks. |
| `as3SamplerForwarding:Boolean` | `false` | Forwards callbacks to a sampler that was already installed before the ANE proxy. |
| `as3RealEdges:Boolean` | `true` | Master switch for factual display/listener edge hooks. |
| `as3RealDisplayEdges:Boolean` | `as3RealEdges` | Factual display-list add/remove edges. |
| `as3RealEventEdges:Boolean` | `as3RealEdges` | Factual non-weak listener/timer add/remove edges. |

There is no stack sampling option. Every `as3_alloc` event has a non-empty
stack payload. If AIR exposes no AS3 frame for an allocation, the profiler
writes an explicit native fallback marker such as
`#0 <as3-stack-unavailable:no-as3-frame>` instead of inventing an AS3 method.

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

## Memory Hooks — Windows

Windows installs guarded detours for these AIR/MMgc entry points:

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

## Memory Hooks — Android

`AndroidDeepMemoryHook` shadowhooks four MMgc entry points in
`libCore.so`. Offsets are pinned by GNU build-id (Android `libCore.so`
ships stripped, so the pin is required); unknown build-ids fall back to
a logged warning and disable the hook rather than wild-patching.

| Hook | arm64-v8a (`7dde220f...`) | armeabi-v7a (`582a8f65...`) |
| --- | ---: | ---: |
| `GCHeap::Alloc` | `+0x89c42c` | `+0x5541cd` (Thumb +1) |
| `FixedMalloc::Alloc` (size-class fast path) | `+0x8a11d8` | inlined into GCHeap callers — n/a |
| `FixedMalloc::ChunkAlloc` (paired with Free) | `+0x8a15a4` | TBD |
| `MMgc::Free` (chunk-level dual of ChunkAlloc) | `+0x8a167c` | TBD |

**Chunk-Alloc/Free pairing (arm64-v8a).** The `+0x8a15a4` /
`+0x8a167c` pair tracks page-aligned chunks rather than the
user-pointers Phase 5's `proxy_GCHeapAlloc` and `proxy_FixedAlloc`
return. Without the chunk-Alloc proxy in place, Free events at
`+0x8a167c` arrive with chunk_base ptrs that have no matching shard
entry (off by header offset). With it, the chunk_base is recorded in
the same shard map the Free queries, so frees match cleanly.
Validated 1:1 (59:59 hits) on 64MB ByteArray churn during RA via
`tests/scenarios/_tmp_aneprof_task11_free_ra.json`.

**Chunk-walk sweep on chunk reclaim.** Adobe inlined the per-class
`FixedAlloc::Free` on this AArch64 build (`grep ldxr|stxr` over
`+0x890000..+0x8b0000` returns zero matches — no atomic free-list
operations remain as standalone functions to hook). To compensate,
`proxy_MMgcFree` calls `dpc->record_free_chunk_sweep(chunk_base,
chunk_size)` after the original. The sweep walks all 32 allocation
shards, collects ptrs in `[chunk_base, chunk_base + chunk_size)`,
and emits FreeIfTracked events for each — implicitly reclaiming the
finer-tier user-pointers Phase 5's alloc proxies tracked within that
chunk. The shard scans are O(N_total) per chunk Free (~10000 entries
walked), bounded by per-shard mutex, asynchronous through the writer
queue.

**libc malloc family.** Phase 1+2 alloc_tracer shadowhooks
`libc.so:malloc/calloc/realloc/free/mmap/munmap` via PLT/GOT patches
on `libCore.so` callers. Reentrancy is guarded by RAII `t_in_tracer`
moved to proxy entry with a size-filter-first ordering (NDK r28
`__emutls_get_address` calls libc malloc on first thread_local touch
— required to avoid recursion on small allocations).

The chunk-tier MMgc::Free hook + chunk-walk sweep + libc free hook
together cover every dealloc path the AS3 runtime takes, matching
Windows `live_allocations` parity.

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

`profilerStart(..., { render: true })` enables optional render
instrumentation. The hook is intentionally disabled by default and has no
active runtime hook until the first render-enabled capture. `profilerStop()`
pauses render capture, clears the active controller and stops writing
events immediately.

### Render Hooks — Windows

The physical vtable restore is deferred to extension shutdown to avoid
racing AIR's renderer while it still owns live D3D/DXGI objects.

On AIR SDK `51.1.3.10`, the hook currently installs the stable Present paths:

- D3D9/9Ex `CreateDevice`, `CreateDeviceEx`, device `Present`, `PresentEx`,
  swap-chain `Present`, `GetSwapChain`, `CreateAdditionalSwapChain`
- D3D9 texture creation/update, render-target changes, clears and draw calls
- DXGI `Present` / `Present1`
- D3D11 texture creation, `UpdateSubresource`, draw calls, render-target
  changes and clears, discovered from a dummy D3D11 device/context

### Render Hooks — Android

`AndroidRenderHook` shadowhooks the Android system GLES libraries via two
distinct binding strategies, both gated by `profilerStart(..., {
render: true })`:

**v1 — `shadowhook_hook_sym_name(libGLESv2.so, ...)`.** Patches the
exported prologue of:

- `libEGL.so:eglSwapBuffers` — frame boundary signal (one
  `render_frame` event per swap)
- `libGLESv2.so:glDrawArrays` / `glDrawElements` — draw_calls counter
  + primitive_count via the helper
  `primitives_from_mode_count(mode, count)` (covers all GLES2 modes:
  POINTS, LINES, LINE_STRIP, LINE_LOOP, TRIANGLES, TRIANGLE_STRIP,
  TRIANGLE_FAN). These bind correctly through the lib-exported
  trampoline because Adobe's draw call sites resolve them at the
  `libGLESv2.so` entry.

**v2 — `eglGetProcAddress(sym)` + `shadowhook_hook_func_addr`.**
Resolves the symbol through `eglGetProcAddress` first, then patches
the resulting address. This is required for:

- `glClear` — clear_count
- `glTexImage2D` — texture_create_count + texture_create_bytes
  (level 0 only; pyramids are bookkeeping not allocations)
- `glTexSubImage2D` — texture_update_count + texture_upload_bytes
- `glBindTexture` — set_texture_count
- `glBindFramebuffer` — render_target_change_count

The byte-size helper `gles_bpp(format, type)` covers the common
GLES2 cases (RGB/RGBA/LUMINANCE/LUMINANCE_ALPHA/ALPHA × UNSIGNED_BYTE
/ FLOAT / HALF_FLOAT_OES, plus packed types `5_6_5/4_4_4_4/5_5_5_1`).
Returns 0 for compressed (ETC2/ASTC) and unrecognized combinations —
those textures are counted but not byte-sized.

**Why eglGetProcAddress and not dlsym for the v2 hooks?** Adobe AIR
caches GLES function pointers at boot via `eglGetProcAddress`, which
on vendor-specific implementations (Adreno, Mali, Xclipse) returns
the vendor implementation entry point, not the libGLESv2.so trampoline
that `dlsym(RTLD_DEFAULT, sym)` resolves. Empirical validation on
Cat S60 (Adreno) showed dlsym-resolved hooks installed cleanly but
fired zero times during 196 frames of dynamic Hall rendering —
Adobe's call sites bypassed them. Switching to eglGetProcAddress
lands the hook on the same address Adobe's call sites cache, and
all v2 metrics begin populating immediately.

The v1 hooks (`glDraw*`, `eglSwapBuffers`) keep using
`shadowhook_hook_sym_name(libGLESv2.so, ...)` because they bind
correctly there; only the texture/clear/bind family needs the
eglGetProcAddress path. Vendor-agnostic for both modes — works on
Adreno, Mali, Xclipse without binding to per-vendor `.so` files.

**Per-frame aggregation.** Each hook bumps a `thread_local` counter
(`t_frame_clear_count`, `t_frame_texture_update_count`, etc.). On
`eglSwapBuffers` the proxy snapshots all counters into a
`render_frame` event payload, then resets them for the next frame.
This matches the Windows D3D9 per-Present aggregation.

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

### Generic capture validation

```powershell
python tools\profiler-cli\aneprof_validate.py path\to\capture.aneprof
python tools\profiler-cli\aneprof_analyze.py path\to\capture.aneprof --require-free-events
```

### Windows E2E

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

### Android E2E

The reference end-to-end Android capture is
[`tests/scenarios/aneprof_full_parity_validate.json`](../../ddtank-client/tests/scenarios/aneprof_full_parity_validate.json)
in the consumer repo. It boots the app on Cat S60 (AArch64,
build-id `7dde220f...`), logs in via the `advanceLogin` helper,
reaches the Hall, runs a 64MB pregc churn to ensure the GC singleton
is captured before `profilerStart`, then captures 30s of real-gameplay
Hall idle with the full hook stack active.

Pull the dump from the device with `adb exec-out` (binary-clean — `adb
shell cat` does CRLF translation on Windows hosts):

```bash
MSYS_NO_PATHCONV=1 adb -s 192.168.82.188:5555 exec-out \
  "run-as br.com.redesurftank.android cat \
   'br.com.redesurftank.android/Local Store/profiler-output/aneprof-full-parity.aneprof'" \
  > aneprof-full-parity.aneprof
```

Expected metrics from a clean 30s capture (Cat S60 build
`7dde220f...`, all hooks active, server SurfTank/1001):

```
alloc/free/realloc      :  ~10000 / ~400 / 0       (chunk-tier balanced, unknown_frees=0)
AS3 alloc/free          :  ~25000 / 0              (Phase 4a sampler firing w/ pc0/pc1)
gc_cycle                :  ~100 events             (live_count/bytes populated)
render_frame            :  ~200 frames
  draw_calls            :  ~6500 (~33/frame)       (Phase 6 v1)
  primitive_count       :  ~150000
  clear_count           :  ~200 (1/frame)          (Phase 6 v2 — eglGetProcAddress)
  set_texture_count     :  ~6500
  texture_update_count  :  ~400
  texture_create_count  :  >=2
  texture_upload_bytes  :  >100 MB
analyzer diagnostic     : "probable live allocations remain at stop"
```

Zero-overhead-when-off invariant validation runs without calling
`profilerStart` and asserts logcat is silent for all profiler tags
(`AneRenderHook`, `AneAs3SamplerHook`, `AneGcHook`, `AneDeepMemHook`,
`AneAs3ObjectHook`, `AneAs3RefGraphHook`, `AneAllocTracer`):

```bash
ANDROID_SERIAL=192.168.82.188:5555 \
  dotnet run --project tools/AirBuildTool -- \
    --test tests/scenarios/_tmp_aneprof_phase4a_off_overhead.json \
    --target android --test-module loadingAirAndroid/loadingAirAndroidDebug \
    --device 192.168.82.188:5555 --port 7965
```
