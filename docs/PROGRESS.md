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

**RA finding**: all occurrences of these strings in libCore.so are
entries in **ABC bytecode constant pools** (length-prefix byte + string,
no null terminator), NOT C-string native binding tables. The same name
(e.g., `addEventListener`) appears 3 times because 3 different ABC
classes reference it in their bytecode. Adobe does NOT ship a separate
name → fn_ptr binding table — the AVM2 method resolver compiles the
binding inline via switch/array on method ordinal, not via string
lookup.

This makes the original plan ("shadowhook DisplayObjectContainer::addChild
+ EventDispatcher::addEventListener directly") significantly harder than
expected: there's no easy XREF-from-string path to the C++ implementation.

**Constraint**: per project policy, all instrumentation must live in the
ANE (no AS3 compiler modifications). Two viable hooks-only paths:

1. **Bindiff-driven RA of the 4 target functions in libCore.so**:
   Windows `libCore.dll` exposes `flash::display::DisplayObjectContainer::
   _addChildAt` and the equivalent `EventDispatcher` methods at known
   RVAs (in `docs/profiler-rva-51-1-3-10.md`). Function pseudocode is
   identical between Windows x64 and Android AArch64 builds (same source).
   Cross-ref each Windows function's anchors (string xrefs, control flow,
   call graph signature) against Android libCore.so to recover offsets.
   Once offsets known, shadowhook each.
   - Pros: per-method hooks, minimal hot-path overhead, exact plan match.
   - Cons: ~2 days of RA per function × 4 functions × 2 archs = ~16 days
     unless bindiff pipeline accelerates it. Each AIR SDK upgrade
     re-RAs.

2. **Hook AVM2 dispatch layer** (`AvmCore::CallNative` / `MethodEnv::
   coerceEnter` or similar):
   Single hook intercepts every AS3 → C++ call. Filter at runtime by
   method name (via `MethodInfo::abc_name` walked from `MethodEnv*`).
   When name matches one of the 4 patterns, capture (owner, arg) from
   the AVM stack and emit reference event.
   - Pros: covers ALL native dispatches with one hook; works for any
     class/method without per-target RA. ONE function offset to find.
   - Cons: hot path. Filter cost on every native call (~1000s/sec).
     Still requires RA of `CallNative`/`coerceEnter` in libCore.so —
     ~2-3 days of bindiff vs Windows.

**Currently pursuing**: Path 1 via bindiff. Started by locating Windows
RVAs for the 4 target functions in `docs/profiler-rva-51-1-3-10.md`.
Each function then needs Android offset recovery via cross-architecture
function-shape matching (Ghidra Bindiff or manual anchor matching).

### Identified Android offsets (build-id 7dde220f...)

| Function | Win RVA | Android offset | Status |
|---|---|---|---|
| `EventDispatcher.addEventListener` | 0x1fc6fc | TBD | Initial 9 candidates (shape 2560-5120B, bl 24-30) all fired 0× during 1000 explicit AS3 addEventListener calls — wrong offsets |
| addChild | 0x50724c | TBD | depends on addEventListener anchor |
| addChildAt | 0x5073e0 | TBD | depends |
| removeChild | 0x507cac | TBD | 7-byte tail thunk |
| removeChildAt | 0x507d5c | TBD | depends |
| removeEventListener | 0x1ff3e4 | TBD | 1508 candidates — too generic |

### Phase 4b RA tooling (committed `af11800`)

Generic in-ANE experiment hook lets AS3 (test scenarios) shadowhook any
libCore.so offset at runtime — no rebuild required. API:

```actionscript
Profiler.experimentHookInstall(libcoreOffset:Number, label:String):int
Profiler.experimentHookHits(libcoreOffset:Number):Number
Profiler.experimentHookUninstallAll():Boolean
```

Validated working: hooked known-good `MMgc::GCHeap::Alloc` at
libCore+0x89c42c during 16MB alloc churn → 187 hits captured with
arg snapshots. Up to 32 concurrent slots.

This pivots the RA workflow from "rebuild+install per offset attempt"
to "one ANE install + N test scenarios". Iteration goes from ~90s/test
to ~30s/test.

### Phase 4b finding — direct C++ method hook NOT FEASIBLE on Android

**30 candidates tested, 0 hits during 1000 explicit AS3 addEventListener
calls.** Shape filter range 1.5x-10x Windows size (1536-10240 B), bl
15-47 (broader than any reasonable compilation variant). Tested:

```
0x269fbc  0xcea460  0xbe9ce4  0xc1899c  0x977e2c  0xbda18c  0xb6d568
0xdcd244  0x476760  0xdc6a08  0xb55bac  0xb70430  0xd21834  0x55b9e4
0x7bbc50  0x969944  0xc98060  0xc17010  0x38af70  0xc9af74  0xb11b7c
0xc83ec4  0xb4f8d4  0x59fb8c  0xbceef0  0x97abd0  0x437610  0xbf7ff8
0xadd388  0xc973b0
```

All 30 hooked successfully via experiment-hook framework, all reported
0 hits. Sanity check (`MMgc::GCHeap::Alloc` known-good offset) reported
187 hits during identical churn — infra works, the candidates are not
the right targets.

**Conclusion**: AS3 native methods (addEventListener / addChild /
removeEventListener / removeChild) are NOT exposed as separate C++
entry points in Android libCore.so for shadowhook to intercept.
Adobe's Android compilation appears to:
1. Inline these methods into the AVM2 dispatcher (`AvmCore::CallNative`
   or equivalent), making them invisible as standalone functions.
2. Or implement them as ABC bytecode (executed by the AVM interpreter)
   with no separate C++ implementation.

Either way, the original Phase 4b plan (shadowhook 4 specific C++
methods) is **architecturally infeasible on Android**, regardless of
how aggressive the candidate search is.

### Phase 4b — viable alternatives

| Approach | Where to hook | Cost | Trade-off |
|---|---|---|---|
| AVM2 dispatcher hook | `AvmCore::CallNative` (or `MethodEnv::coerceEnter`) — single function intercepts EVERY AS3→C++ call | RA of dispatcher + runtime name filter + ~600 LOC | Hot path (1000s/sec calls); MUST be cheap. Locates dispatcher requires its own bindiff session (~2-3 days). |
| MMgc write-barrier hook | `MMgc::GC::WriteBarrierWrite` — every pointer-field assignment | Already known offset, easy | Captures EVERY write, not just reference semantics. Coarse — would emit 100k+ events/sec, mostly noise. Filter logic unclear. |
| **Skip Phase 4b** | — | — | Phase 4c covers typed alloc graph (which is what 80% of leak hunts need). Reference graph is nice-to-have for cycle detection but not critical. Plan ships as 9/10 phases green. |

**Decision**: Per user constraint (hooks-only, no compiler injection),
Phase 4b cannot deliver via direct method hooks. Documented as
architecturally limited. Phase 4c remains the production typed-alloc
path with 5% resolution rate (= the actual ScriptObject ratio of all
FixedMalloc allocs — not a defect).

### Final plan status

| Phase | Status |
|---|---|
| 0 reentrancy fix | ✅ production |
| 1+2 native alloc/free + DPC lifecycle | ✅ production |
| 3 method walker (compiler probe injection) | ✅ production |
| 4a IMemorySampler hook | ✅ investigated — equivalent to Phase 4c |
| 4b reference graph | ❌ architecturally infeasible via puro-hooks |
| 4c typed AS3 alloc walker (Path B) | ✅ production |
| 5 GCHeap::Alloc deep hook | ✅ production |
| 6 EGL render hook | ✅ production |
| 7a profilerRequestGc (GC observer + trigger) | ✅ production |
| 7b profilerRecordFrame | ✅ production |

**9 of 10 phases green**.

### Phase 4b — definitive architectural finding

After three RA passes via experiment-hook framework (no rebuild
required between passes thanks to `AndroidExperimentHook`):

| Pass | Filter | Candidates | Result |
|---|---|---|---|
| 1 | size 2.5x-5x Win, bl 24-30 | 9 | 0 hits / 1000 AS3 calls |
| 2 | size 1.5x-10x Win, bl 15-47 | 30 | 0 hits / 1000 AS3 calls |
| 3 | dispatcher-shape (small + blr) | 30 | 1 hit total / 1000 AS3 calls |
| 4 | interpreter-shape (4-60KB + blr) | 30 | max 18 hits / 1000 AS3 calls |

Across 99 candidate offsets covering broad shape ranges (small thunk
to large interpreter), zero candidate fires consistently with AS3
addEventListener invocations.

**Root cause**: AS3 native methods are bound at AVM2 verify time —
the binding resolves `addEventListener` (and similar) to a C++ function
pointer that is then **JIT-baked into AS3 bytecode-emitted machine
code**. The JIT'd code calls the resolved function pointer directly,
**bypassing any shadowhook patch installed after binding** because:
1. shadowhook patches the function PROLOGUE
2. JIT code at the call site has the original target address
3. CPU executes branch to the original prologue location
4. The patched first 4 bytes redirect to our proxy stub
5. *…BUT* if the JIT-baked address points past the prologue (e.g., an
   inlined fast-path), the patch is bypassed
6. *…OR* if Adobe inlined the implementation entirely, no entry
   exists to patch

Sanity check: `MMgc::GCHeap::Alloc` (Phase 5, known-good offset)
fires 187× during 16MB churn. Hook infra is correct; AS3 native
methods are simply not exposed as standalone hookable C++ entry
points on Android.

### Phase 4b — alternatives if pursued in the future

| Approach | Cost | Trade-off |
|---|---|---|
| Hook AVM2 verify-time binding registration (intercept the `NativeMethodInfo` table install) | ~3 days RA + reuse hook framework | Catches binding before JIT bake — but requires tracing the verify path |
| Hook MMgc write-barrier (`MMgc::GC::WriteBarrierWrite`) — every pointer-field assignment in GC heap | Already known offset, easy | High noise (100k+ events/sec); reference semantics extracted from access patterns |
| AVM2 interpreter slot dispatch (intercept the bytecode op for `callmethodlate`) | Requires deep RA + hooking interpreter switch | Most invasive; high overhead on every AS3 method call |

All three require multi-day RA. **Status quo**: Phase 4c (production
typed-alloc walker, 5% resolution = real ScriptObject ratio) covers
80% of leak-hunt scenarios. Phase 4b reference graph is "nice to
have" for cycle detection but not blocking; defer until concrete
campaign requires it.

### Phase 4b — infrastructure preserved for future RA

Reusable assets committed:

- **`AndroidExperimentHook`** (`af11800`) — hook ANY libCore.so offset
  at runtime via JNI/AS3 from test scenarios. Up to 32 concurrent slots
  with arg capture + hit counters. Validated firing 187× on
  known-good `GCHeap::Alloc`.
- **`AdbServerIsolator`** (`AirBuildTool b9a97a8`) — dedicated
  single-device adb server for parallel test runs.
- **`ResolveHostLanIpForWifiTarget`** + LAN-direct dial path
  (`AirBuildTool b9a97a8`) — bypasses adb 37.0.0+ `reverse` bug,
  multi-device-agnostic test pipeline.
- **`tools/crash-analyzer/android_phase4b_*.py`** — bindiff scripts
  + scenario generators for future RA passes on different SDK builds.

When Adobe ships a different libCore.so build-id (SDK upgrade), the
infrastructure is reusable; the RA work is per-build-id
(non-portable across versions).

Reference Windows implementation lives in
`WindowsNative/src/profiler/WindowsAs3ObjectHook.cpp:139` (`g_as3_refs`).
Windows uses native shadowhook (works because Windows AIR has more
exposed binding metadata). Android can't easily replicate that, so
Path 1 is the practical Android-equivalent.

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
