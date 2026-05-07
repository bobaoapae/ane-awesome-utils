# Profiler Usage Guide

This guide covers the day-to-day `.aneprof` workflow: AS3 calls, capture flags,
status fields, CLI tools and validation commands.

The deep implementation details remain in
[profiler-deep-aneprof.md](profiler-deep-aneprof.md).

## Scope

The `.aneprof` backend targets AIR SDK `51.1.3.10`. As of 2026-05-06 it
supports:

- **Windows x86** — full hooks (MMgc + render + AS3 sampler + reference graph)
- **Windows x64** — full hooks (MMgc + render + AS3 sampler + reference graph)
- **Android arm64-v8a** — full hooks (MMgc chunk-tier with chunk-walk sweep,
  GLES2 v1+v2 render, AS3 sampler with pc0/pc1 attribution, GC observer +
  programmatic trigger via `requestCollect`)
- **Android armeabi-v7a** — Phase 5 partial (GCHeap-only; FixedMalloc/Free
  inlined into callers per Adobe's ARMv7 toolchain), other phases full
- **iOS** — Phase 1+2 alloc_tracer libc shadowhook only (no MMgc/render hooks)
- **macOS** — Phase 1+2 alloc_tracer libc shadowhook only

It does not require Scout, `.telemetry.cfg` or advanced telemetry. The old
`.flmc` tools remain in the repository for legacy captures, but
memory/performance work should use `.aneprof`.

For Android-specific hook offsets and reverse-engineering details see
[profiler-rva-android-51-1-3-10.md](profiler-rva-android-51-1-3-10.md).

## AS3 API

Initialize the ANE once, then start and stop captures around the workload:

```actionscript
import AneAwesomeUtils;
import flash.filesystem.File;

var ane:AneAwesomeUtils = AneAwesomeUtils.instance;
if (!ane.initialize()) {
    trace("ANE init failed");
    return;
}

var dump:File = File.applicationStorageDirectory.resolvePath(
    "aneprof-dumps/run-0.aneprof"
);

var started:Boolean = ane.profilerStart(dump.nativePath, {
    timing: true,
    memory: true,
    render: false,
    snapshots: true,
    snapshotIntervalMs: 1000,
    maxLiveAllocationsPerSnapshot: 20000,
    as3ObjectSampling: true,
    as3SamplerForwarding: false,
    as3RealEdges: true,
    metadata: {
        scenario: "formigueiro_loop",
        run: 0
    }
});

if (!started) {
    trace("profilerStart failed");
    return;
}

ane.profilerMarker("login.start");
// drive UI/gameplay here
ane.profilerMarker("login.end");

ane.profilerSnapshot("before-gc");
ane.profilerRequestGc();
ane.profilerSnapshot("after-gc");

var status:Object = ane.profilerGetStatus();
trace("dropped=" + status.dropped +
      " as3=" + status.as3ObjectAllocCalls +
      " overflowPeak=" + status.writerOverflowPeak);

ane.profilerStop();
```

Available methods:

| Method | Use |
| --- | --- |
| `profilerStart(path, options)` | Opens a new `.aneprof` capture. Returns `false` if unsupported, already recording, output path is invalid or disk free space is too low. |
| `profilerStop()` | Stops hooks and closes the file. Always call this before reading the dump. |
| `profilerSnapshot(label)` | Writes native/AS3 live counters and a bounded list of live native allocations. |
| `profilerMarker(name, value)` | Writes a marker. Use paired `.start` / `.end` names for spans. |
| `profilerRecordFrame(index, durationMs, allocationCount, allocationBytes, label)` | Writes an explicit AS3 frame/interval event. Useful for synthetic tests or a custom frame bridge. |
| `profilerRequestGc()` | Requests AIR native GC and writes a `gc_cycle` event with before/after counters. |
| `profilerGetStatus()` | Returns live hook, writer, render and AS3 sampler diagnostics. |

## `profilerStart` Options

| Option | Default | Notes |
| --- | ---: | --- |
| `timing` | `true` | Enables markers, method enter/exit events and timing summaries. |
| `memory` | `false` | Enables native memory hooks. AS3 object diagnostics require this to be `true`. |
| `render` | `false` | Enables optional D3D/DXGI render frame aggregation. Disabled by default to avoid render hook work in memory-only captures. |
| `snapshots` | `true` | Writes initial/final snapshots and allows manual snapshots. |
| `snapshotIntervalMs` | `0` | `0` means manual snapshots only. Use `1000` or higher for long gameplay loops. |
| `maxLiveAllocationsPerSnapshot` | `4096` | Caps native `live_allocation` records per snapshot. AS3 live summaries are reconstructed from alloc/free events and are not limited by this cap. |
| `metadata` | none | Copied into header JSON. Use it for scenario, run number, account, renderer, build SHA and other run context. |
| `headerJson` | generated | Full header override. Prefer `metadata` unless a test needs exact header control. |
| `as3ObjectSampling` | `true` | Installs the native `IMemorySampler` hook for AS3 alloc/free/reference events when `memory=true`. |
| `as3SamplerForwarding` | `false` | If another sampler already exists, forward callbacks to it after recording `.aneprof` events. Leave `false` for normal leak runs to reduce overhead. Set `true` only when you intentionally need `flash.sampler`/Scout's sampler to keep receiving callbacks. |
| `as3RealEdges` | `true` | Master switch for factual AS3 edge hooks. |
| `as3RealDisplayEdges` | `as3RealEdges` | Records factual display-list add/remove edges as `display_child`. |
| `as3RealEventEdges` | `as3RealEdges` | Records factual non-weak listener add/remove edges as `event_listener` or `timer_callback`. |
| `frameEvents` | test bridge only | Used by the E2E AS3 test app; ignored by the native `profilerStart` call. |

There is intentionally no stack sampling flag. Every `as3_alloc` event gets a
non-empty stack payload. When AIR exposes an AS3 frame, that stack is AS3. When
the allocation happens without a visible AS3 frame, the event is marked with a
native fallback like `#0 <as3-stack-unavailable:no-as3-frame>` instead of a
fake AS3 method.

## Capture Recipes

Memory leak capture:

```actionscript
ane.profilerStart(path, {
    timing: true,
    memory: true,
    render: false,
    snapshots: true,
    snapshotIntervalMs: 1000,
    maxLiveAllocationsPerSnapshot: 20000,
    as3ObjectSampling: true,
    as3RealEdges: true,
    metadata: { scenario: "memory-loop", run: runIndex }
});
```

Render/performance capture:

```actionscript
ane.profilerStart(path, {
    timing: true,
    memory: false,
    render: true,
    snapshots: true,
    metadata: { scenario: "render-stress", renderer: "d3d11" }
});
```

Combined memory and render capture:

```actionscript
ane.profilerStart(path, {
    timing: true,
    memory: true,
    render: true,
    snapshots: true,
    snapshotIntervalMs: 1000,
    as3ObjectSampling: true,
    as3RealEdges: true,
    metadata: { scenario: "long-session", renderer: "d3d11" }
});
```

This records the most data, but it is also the heaviest mode. Use it to
correlate a suspected leak with a frame/render symptom, not as the default for
every run.

## Status Checklist

Call `profilerGetStatus()` shortly after `profilerStart()` and again before
`profilerStop()`.

Core fields:

| Field | Meaning |
| --- | --- |
| `state` | `0=Idle`, `1=Starting`, `2=Recording`, `3=Stopping`, `4=Error`. |
| `events`, `payloadBytes` | Events accepted by the writer and payload bytes written. |
| `dropped` | Events that could not be queued or written. This should stay `0` in acceptance runs. |
| `writerQueueDepth` | Ring queue depth plus overflow depth. |
| `writerQueueCapacity` | Fixed ring capacity. |
| `writerOverflowDepth` | Current in-memory overflow depth when the writer cannot keep up. |
| `writerOverflowPeak` | Peak overflow depth for the capture. Non-zero means the writer was saturated, but events were kept in memory instead of spinning the game thread. |
| `writerOverflowEvents` | Total events routed through overflow. |

Memory/AS3 fields:

| Field | Meaning |
| --- | --- |
| `memoryLeakDiagnosticsReady` | Native alloc/free/realloc hooks are active. |
| `as3LeakDiagnosticsReady` | AS3 object sampler hook is active. |
| `as3ObjectAllocCalls`, `as3ObjectFreeCalls` | AS3 alloc/free callbacks observed. |
| `as3ObjectStackCacheHits`, `as3ObjectStackCacheMisses` | Exact stack cache counters. Hits reduce repeated stack construction without sampling. |
| `as3ObjectStackUnavailableCalls` | AS3 alloc callbacks where AIR exposed no AS3 frame. The event still has a non-empty native fallback stack. |
| `as3ObjectStackNativeFallbackCalls` | Native fallback stacks written. Should match `as3ObjectStackUnavailableCalls`. |
| `as3ObjectHookChainedSampler` | The ANE installed in front of an existing AIR sampler. |
| `as3ObjectHookForwardedCalls`, `as3ObjectHookForwardFailures` | Forwarding counters when `as3SamplerForwarding=true`. |
| `as3RealEdgeHookInstalls`, `as3RealEdgeHookFailures` | Optional factual edge hook install counters. |
| `as3RealDisplayChildEdges`, `as3RealDisplayChildRemoves` | Factual display-list edge mutations. |
| `as3RealEventListenerEdges`, `as3RealEventListenerRemoves` | Factual listener/timer edge mutations. |

Render fields:

| Field | Meaning |
| --- | --- |
| `renderDiagnosticsReady` | Render hook installed and at least one slot was patched. |
| `renderHookPresentCalls`, `renderHookFrames` | Present calls and emitted `render_frame` events. These should grow during a render-enabled capture. |
| `renderHookDrawCalls`, `renderHookPrimitiveCount` | Hooked D3D draw/blit activity. Can stay zero if AIR uses an unhooked draw path while Present is still captured. |
| `renderHookTextureCreates`, `renderHookTextureUpdates` | Hooked texture/surface operations. |
| `renderHookTextureUploadBytes`, `renderHookTextureCreateBytes` | Estimated texture upload/create bytes when the backend exposes dimensions/formats. |

## CLI Tools

Structural validation:

```powershell
python tools\profiler-cli\aneprof_validate.py path\to\capture.aneprof
```

This checks the header/footer, walks every event and prints counts by event
type. Exit codes:

| Code | Meaning |
| ---: | --- |
| `0` | Structurally valid. |
| `2` | Parse/format error. |
| `4` | Footer counters do not match walked events. |

Single-file analysis:

```powershell
python tools\profiler-cli\aneprof_analyze.py path\to\capture.aneprof --top 30 --stack-frames 8
```

Write machine-readable and HTML reports:

```powershell
python tools\profiler-cli\aneprof_analyze.py path\to\capture.aneprof `
    --json out\analysis.json `
    --html out\analysis.html `
    --top 50 `
    --stack-frames 12
```

Diff a baseline run against a later run:

```powershell
python tools\profiler-cli\aneprof_analyze.py baseline.aneprof target.aneprof `
    --diff `
    --json out\diff.json `
    --html out\diff.html `
    --top 50
```

CI-style leak gate:

```powershell
python tools\profiler-cli\aneprof_analyze.py path\to\capture.aneprof `
    --require-free-events `
    --fail-on-leak `
    --min-live-bytes 1048576 `
    --min-live-count 100
```

`aneprof_analyze.py` exit codes:

| Code | Meaning |
| ---: | --- |
| `0` | Analysis completed without warnings and leak gate did not fail. |
| `2` | Usage or parse error. |
| `4` | Analysis completed with warnings. |
| `6` | `--require-free-events` failed because allocs exist but no free/realloc events exist. |
| `8` | `--fail-on-leak` threshold was reached. |

Important JSON/report fields:

| Field | Use |
| --- | --- |
| `top_as3_live_types`, `top_as3_live_stacks`, `top_as3_allocation_sites` | Main leak triage by type, stack and allocation site. |
| `retainer_paths` | Shortest known path from a root to live AS3 objects. |
| `dominator_summary` | Retained size. Exact on small graphs, marked partial on large approximations. |
| `payload_by_owner` | BitmapData/ByteArray/native payload bytes attached to AS3 owners or unowned pseudo-payloads. |
| `lifetime_summary` | Age, snapshot/GC survival and never-freed objects. |
| `allocation_rate` | Allocation bursts by second, marker and frame. |
| `frame_summary` | Explicit AS3 frame/interval events from `profilerRecordFrame`. |
| `gc_summary`, `post_native_gc_as3` | Objects/bytes still live after `profilerRequestGc()`. |
| `render_frame_summary`, `render_frames` | Native Present intervals, present time, CPU-between-present estimate and render counters. |
| `performance_suspects` | Marker/frame/render/allocation-site suspects scored for triage. |

## Build And Test Commands

Build/package the ANE after native changes:

```powershell
python build-all.py windows-native
python build-all.py package
```

Run shared profiler unit tests:

```powershell
cmd /c shared\profiler\build.bat
```

Run synthetic AIR E2E:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File tests\profiler-e2e\run_test.ps1 -Arch x64 -D3D d3d11 -KeepOutputs
pwsh -NoProfile -ExecutionPolicy Bypass -File tests\profiler-e2e\run_test.ps1 -Arch x64 -D3D d3d9 -KeepOutputs
pwsh -NoProfile -ExecutionPolicy Bypass -File tests\profiler-e2e\run_test.ps1 -Arch x86 -KeepOutputs
```

In a client repository that uses AirBuildTool, run the scenario from the client
repo so relative scenario paths and ports match that project:

```powershell
dotnet run --project tools\AirBuildTool -- --test tests\scenarios\formigueiro_loop.json --target windows --verbose
```

Keep each UI/battle scenario on its configured port and do not reuse a port
while another automated test is logged into the same account/server session.
