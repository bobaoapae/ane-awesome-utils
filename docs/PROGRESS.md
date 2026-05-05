# `.aneprof` 100% Windows-Android Parity — Implementation Progress

Status snapshot for the Phase 0..7 plan defined in the project root planning
prompt. Updated each iteration of the `/loop` driving Phase 4a/4b finalization.

Validation devices:
- **AArch64**: Cat S60, Android 6.0.1 / API 23, libCore.so build-id `7dde220f62c90358cfc2cb082f5ce63ab0cd3966`
- **ARMv7**:  Galaxy A10, Android 9, libCore.so build-id `582a8f65b8dcb741e5eb869ccf9526137270d99e`

## Phases

### Phase 0 — `alloc_tracer` reentrancy fix (`#25`)
**Status:** ✅ Production. Validated in `_tmp_pvp_10x_galaxy_a10_soak.json` and
PVP soak runs. RAII `t_in_tracer` guard + arena-allocated open-addressing hash
table replace `std::unordered_map<addr, AllocRecord>` on the hot path.

### Phase 1+2 — Native alloc/free events + DPC lifecycle
**Status:** ✅ Production. `alloc_tracer.cpp` shadow-hooks libc malloc family
on libCore.so callers; `AndroidProfilerBridge.cpp` owns the
`DeepProfilerController` lifecycle and JNI surface.

### Phase 3 — AS3 method walker (`method_id` on `Alloc` events)
**Status:** ✅ Production via **AS3 compiler probe injection** (NOT TLS walker
of the original plan). The AS3 compiler is built with `--profile-probes`; it
emits `awesomeUtils_profilerProbeEnter(method_id)` / `Exit(method_id)` calls
at every method boundary. DPC maintains a per-thread method stack; alloc
events inherit `current_method_id()` automatically. `MethodTable` payload
registered once at app init.

The TLS walker in the original plan was never needed — the compiler-injection
path is more robust (no avmplus internal dependency) and equivalent in
outcome.

### Phase 4a — `IMemorySampler` hook (Path A — typed AS3 alloc/free)
**Status:** ✅ **Investigated; equivalent to Phase 4c — no productive gain.**
Sampler localized at `AvmCore+0x68 → vftable[12] = recordAllocationSample`
(150k hits per 128MB churn, 1:1 with allocs, FIXED-this confirms it's a
real virtual on the sampler singleton).

But: the sampler internally just stores `(item, size)` in a ring buffer
and defers type resolution to `getSamples()` time. There is no separate
"typed-capture" slot that emits AS3 class names directly. Type resolution
requires walking the `item` layout — exactly what Phase 4c already does
via `MMgc::FixedMalloc::Alloc` defer queue.

Phase 4a infrastructure (AndroidSamplerHook, AndroidAs3SamplerHook) is
committed for future use (e.g., dealloc tracking via the parallel
`recordDeallocationSample` slot that Phase 4c can't see), but does NOT
supersede Phase 4c for typed alloc capture.

Discovered (Cat S60, build-id `7dde220f...`):
- Adobe ships a **working** `IMemorySampler` in Android libCore.so.
  `flash.sampler.getSamples()` returns 4861 `NewObjectSample` +
  `DeleteObjectSample` after `startSampling()` + churn. The
  `presample`/`recordAllocationSample` debug strings are absent (Adobe
  compiles with NDEBUG-like macro), but the code is there.
- Sampler localization (dynamic, no symbols required):
  ```
  gc_singleton (captured by AndroidGcHook on first MMgc::GC::Collect)
    + 0x10  →  AvmCore*
    + 0x68  →  IMemorySampler*
    *(0)    →  vftable in libCore .data
    vftable + 12*8  →  recordAllocationSample
  ```
- Slot 12 confirmed via 16-slot diagnostic scan with arg-shape filter:
  - 150622 hits per 128MB churn (1:1 ratio with allocs)
  - `distinct_a0 == 1` across 100 calls (= fixed-this = real virtual on
    sampler singleton)
  - a2 = 0x3e8 = 1000 bytes (matches AS3 alloc size)

**Remaining gap**: items passed to `recordAllocationSample` are NOT
ScriptObjects — VTable walk at +0 AND +16 both fail (0 resolved out of
219960 hits). The TYPED path (which produces `flash.sampler::NewObjectSample`)
is in a **different sampler method**, likely `recordAllocationInfo(item,
Traits*, size, ...)` or similar that takes a `Traits*` parameter explicitly.

**Next concrete step**: re-run 16-slot diagnostic with arg-shape filter
"a3/a4 in libCore .data range" (Traits vtable signature) to locate the slot
that takes Traits* as a parameter.

### Phase 4b — AS3 reference graph
**Status:** ⏳ RA in progress. Method names confirmed in libCore.so .rodata
ABC constant pool (length-prefixed strings, no null terminator):

| Name | Address | Class table |
|---|---|---|
| `addChildAt` | 0xea9049 | DisplayObjectContainer (`.display:DisplayObjectContainer.addChildAt.removeChild...`) |
| `removeChild` | 0xea9054 | DisplayObjectContainer (same table) |
| `removeChildAt` | 0xea9060 | DisplayObjectContainer (same table) |
| `addEventListener` | 0xe8fc02 | EventDispatcher (`IEventDispatcher.addEventListener.removeEventListener...`) |
| `removeEventListener` | 0xe8fc13 | EventDispatcher (same table) |

But these strings are **AVM2 ABC bytecode constant pool entries** (length-
prefix byte + string), NOT native-function-pointer tables. They tell the
AVM how to resolve AS3 calls but don't directly map to C++ function
addresses.

**Next concrete RA step**: locate the **native binding table** that maps
each ABC method name to a C++ function pointer. In avmplus open source,
this is `flashNativeIDs` array at `flash::display::DisplayObjectContainer`
class registration site. Adobe ships this stripped of symbols, but the
table layout (a struct array of `{name_idx, fn_ptr}` typically) should
be discoverable by scanning the .data section adjacent to the class
constructor.

Alternative path: hook at AVM2 dispatch layer (`AvmCore::CallNative` or
similar) — slower but doesn't require finding individual binding-table
entries. Trade-off: every native call gets intercepted.

Reference Windows implementation lives in
`WindowsNative/src/profiler/WindowsAs3ObjectHook.cpp:139` (`g_as3_refs`).
Estimated total: ~900 LOC + RA of 4 methods × 2 architectures.

### Phase 4c — Path B (typed AS3 alloc via VTable→Traits walk)
**Status:** ✅ Production. Hooks `MMgc::FixedMalloc::Alloc` (Phase 5
proxy), defer-queue with 50µs delay, walks
`ScriptObject+16 → VTable+48 → Traits+224 → Stringp.buffer`. ~5%
resolution rate (which matches the actual ratio of FixedMalloc allocs that
ARE ScriptObjects — not a defect, that IS the typed-AS3 population).

Validated layouts:
- **AArch64** (Cat S60): `vtable=+16, traits=+48, name=+224`
- **ARMv7** (Galaxy A10): `vtable=+8, traits=+32, name=+72`

### Phase 5 — `MMgc::GCHeap::Alloc` deep hook
**Status:** ✅ Production. `AndroidDeepMemoryHook.cpp` shadow-hooks
`+0x89c42c` on AArch64 (build-id `7dde220f...`). Captures the canonical
runtime-internal alloc path that's invisible to libc malloc hooks.

### Phase 6 — EGL render hook
**Status:** ✅ Production. `AndroidRenderHook.cpp` shadow-hooks
`eglSwapBuffers` and `glDraw*`. Emits `RenderFrame` events with timing +
draw count.

### Phase 7a — `profilerRequestGc` (`MMgc::GC::Collect` observer + trigger)
**Status:** ✅ Production. `AndroidGcHook.cpp` shadow-hooks at
`+0x896824` on AArch64 (`+0x5501e1` on ARMv7). Captures GC singleton on
first fire; subsequent `requestCollect()` calls fire programmatic GC.

### Phase 7b — `profilerRecordFrame`
**Status:** ✅ Production. `nativeRecordFrame` JNI in
`AndroidProfilerBridge.cpp` emits `Frame` events with label, duration,
alloc deltas.

## Blocking sequence

1. Phase 4a typed-capture identification (estimated 1-2h iterations)
   - Re-run diagnostic with Traits-shape filter
   - Identify the `recordAllocationInfo` (or equivalent) slot
   - Update `AndroidAs3SamplerHook` to hook that slot, walk Traits→name
   - Validate resolved count > 1000 per 128MB churn (vs current 0)
2. Phase 4a integration: replace Phase 4c queue with sampler-driven typed
   emit (Phase 4c retained as fallback for unsampled allocs)
3. Phase 4b reference graph (separate work, ~900 LOC)
4. ARMv7 sampler offsets for parity

## Files

Production:
- `AndroidNative/app/src/main/cpp/profiler/AndroidGcHook.{cpp,hpp}`
- `AndroidNative/app/src/main/cpp/profiler/AndroidAs3ObjectHook.{cpp,hpp}` (Phase 4c)
- `AndroidNative/app/src/main/cpp/profiler/AndroidDeepMemoryHook.{cpp,hpp}` (Phase 5)
- `AndroidNative/app/src/main/cpp/profiler/AndroidRenderHook.{cpp,hpp}` (Phase 6)
- `AndroidNative/app/src/main/cpp/profiler/AndroidProfilerBridge.cpp` (JNI)

Phase 4a in-progress:
- `AndroidNative/app/src/main/cpp/profiler/AndroidSamplerHook.{cpp,hpp}` (diagnostic 96-slot scanner)
- `AndroidNative/app/src/main/cpp/profiler/AndroidAs3SamplerHook.{cpp,hpp}` (productive on slot 12)

Phase 4b: not yet started.

## Recent commits (`ane-awesome-utils` master)

- `a5f1d66` — Phase 4a slot 12 identified
- `0106678` — Phase 4a RA framework
- `62c1687` — Phase 4c diagnostic-discovery removed for ship build
- `634c043` — Phase 4c AArch64 layout validated
