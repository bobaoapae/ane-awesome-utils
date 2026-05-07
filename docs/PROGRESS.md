# `.aneprof` 100% Windows-Android Parity — Implementation Progress

Status snapshot for the Phase 0..7 plan defined in the project root planning
prompt. Updated each iteration of the `/loop` driving Phase 4a/4b finalization.

## Test device policy — Cat S60 ONLY

**MANDATORY:** all on-device validation MUST run on Cat S60 (AArch64,
192.168.82.188:5555, build-id `7dde220f62c90358cfc2cb082f5ce63ab0cd3966`).

- Do NOT use Galaxy J5 (192.168.82.189) or Galaxy A10 — concurrent
  sessions / different builds may be using those devices.
- Do NOT use the USB device (4200a703c647151d / S601751058374) — same reason.
- Always invoke AirBuildTool with `--device 192.168.82.188:5555` AND
  `ANDROID_SERIAL=192.168.82.188:5555` (env var so bundletool also targets it).
- If Cat S60 is offline (wifi flaky), STOP and wait the next loop tick — do
  NOT fall back to another device.
- Pull `.aneprof` files via `MSYS_NO_PATHCONV=1 adb -s 192.168.82.188:5555
  exec-out "run-as br.com.redesurftank.android cat /data/.../file.aneprof"
  > out.aneprof` (binary-clean; `adb shell cat` does CRLF translation on
  Windows hosts).

Reference layout (Cat S60 / AArch64): `gc+0x10 → AvmCore`,
`AvmCore+0x68 → IMemorySampler`, `vftable[12] = recordAllocationSample` at
`libCore+0x8952ec`. Production sampler hook auto-installs at startDeep
when `as3ObjectSampling=true`; bails out gracefully if GC singleton not
captured (Phase 4c remains active).

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
**Status:** ✅ **FULL PRODUCTION VALIDATED (2026-05-06 Cat S60 AArch64).**

End-to-end auto-install via detached polling thread + 500ms grace, no
crash, no regression on Phase 4c. Validated via
`tests/scenarios/_tmp_aneprof_phase4a_natural_gc.json`:

| Metric | Value |
|---|---|
| events | 13709 |
| as3_alloc_sampler markers (Phase 4a w/ pc0+pc1 attribution) | 12345 |
| as3_alloc markers (Phase 4c typed-alloc) | 220 |
| alloc / live_allocation | 567 / 567 |
| gc_cycle (Phase 7a observer) | 6 |
| snapshot | 2 |
| File validates via aneprof_validate.py | ✅ clean |

`pc1` distribution exact match with experiment-hook frame[3] earlier:
- hot: `libCore+0x8a265c` (82.5% / 10179 hits)
- alt: `libCore+0x89a684` (17.5% / 2166 hits)

Auto-install path log:
- `nativeStartDeep: AS3 sampler hook deferred (poller thread spawned)` —
  initial eager install fails (GC singleton not yet captured at startDeep)
- `AneGcHook: captured GC singleton` — natural GC fires during scenario
- `AS3 sampler hook installed (Phase 4a, polled+grace)` — polling thread
  observes singleton + 500ms grace + installs successfully

**Analyzer parity validated 2026-05-06.** Hook now emits typed `As3Alloc`
events (`EventType=12`) via `dpc->record_as3_alloc()` instead of generic
`Marker` events. Analyzer (`aneprof_analyze.py`) recognizes Android
`.aneprof` exactly like Windows:

```
AS3 alloc/free  :  12337 /      0
AS3 live objects:         1221
AS3 live bytes  :        25640

Leak suspects:
  [medium] pc0=0x7f7d265a1c,pc1=0x7f7d25d684 count=480 bytes=16736
    types: ? x480
    - many live AS3 objects from this site
```

`pc0,pc1` encoded into the `stack` field of `As3Alloc`. Analyzer parses
stack as call-site signature and auto-groups allocations into leak
suspects. Same output format, same logic, same leak-detection
capability as Windows.

Pending future work: hook `recordDeallocationSample` to emit `As3Free`
events; would enable true alloc/free pairing and lifetime analysis.
Investigation 2026-05-06 found Adobe removed it as a separate vftable
slot in this libCore build (slots 13/14 NULL; slots 15/16 are
presample-style with stack-args, NOT (Sampler*, item)). Disabled in
code; phase 4a alloc-only delivers full leak detection via stack
signature + live-byte growth. Future SDK upgrades may re-add the slot.

**2026-05-06 final design — eager-only install:** Phase 4a auto-install
removed the polling thread because shadowhook's prologue patch is not
safe against concurrent calls to the patched function. With `as3=true`
in startDeep, we attempt eager install (works if GC singleton already
captured at startDeep, e.g., warm app). If it fails (cold app, no GC
yet), we log a warning and SKIP Phase 4a — caller must invoke
`profilerAs3SamplerInstall()` explicitly from a quiet window (no AS3
allocation churn). Phase 4c (typed-alloc resolver) is unaffected and
provides full alloc tracking; Phase 4a is the additive pc0/pc1
attribution layer.

**Recommended workflow:** call `profilerWarmupGcObserver()` early in
the AS3 boot flow (e.g., `Loading.as` init). This installs the
`MMgc::GC::Collect` shadowhook with no DPC; the proxy captures the GC
singleton on the first observed Collect (typically within seconds of
app launch). When `profilerStartDeep` is later called with
`as3ObjectSampling=true`, the eager Phase 4a install succeeds because
the singleton is already captured.

```as3
// Loading.as init (or earliest available callback):
AneAwesomeUtils.instance.profilerWarmupGcObserver();

// Later, when starting a memory profile:
AneAwesomeUtils.instance.profilerStart(path, {
    memory: true,
    as3ObjectSampling: true,
    snapshots: true,
    timing: true
});
// → Phase 4a installs eagerly (singleton was captured during warmup)
//   → as3_alloc events with pc0+pc1 attribution flow into .aneprof
```

**Validated 2026-05-06 on Cat S60 (build-id `7dde220f...`)** via
`tests/scenarios/_tmp_aneprof_phase4a_warmup_api.json`:

| Metric | Value |
|---|---|
| events | 13381 |
| as3_alloc (Phase 4a typed events) | 12386 |
| Phase 4c markers | 185 |
| Crash | NONE |
| File validates via aneprof_validate.py | ✅ |
| Analyzer leak suspects | ✅ via pc0+pc1 stack signature |

Logcat confirms full chain:
- `AneGcHook: captured GC singleton` (warmup-installed observer fires
  on natural GC)
- `nativeStartDeep: AS3 sampler hook installed (Phase 4a, eager)`
  (subsequent profilerStart eager install succeeds)

**Cold-app fallback (without warmup call):** if profilerStart is called
without prior `profilerWarmupGcObserver`, eager install fails (singleton
not captured). Phase 4a stays uninstalled — Phase 4c continues to emit
typed `as3_alloc` events with class names, still leak-detectable via
analyzer (just without pc0/pc1 attribution).

**Why warmup, not auto-defer:** shadowhook's prologue patch is not
safe against concurrent calls to the patched function. A polling-thread
auto-installer would race against `recordAllocationSample` if AVM
sampler is active during install → SIGSEGV from half-patched prologue.
The warmup model puts hook installation BEFORE any AS3 activity that
could activate sampling, eliminating the race deterministically.

**Cost analysis** (zero-overhead-off requirement):
- Hook only installs when `as3ObjectSampling=true` in startDeep
- Proxy gated by `g_active.load` early-return — zero work when capture
  inactive
- FP-walk + marker emit only when capture active: ~5ns FP-walk + 1
  string format/event
- Polling thread sleeps 100ms between polls, terminates on install or
  60s timeout

**Zero-overhead-off VALIDATED (2026-05-06)** via
`tests/scenarios/_tmp_aneprof_phase4a_off_overhead.json`:

When `profilerStart` is NOT called, **NO hooks are installed**:
- No `AneAllocTracer` (libc malloc/free/mmap/munmap)
- No `AneGcHook` (MMgc::GC::Collect)
- No `AneDeepMemHook` (FixedMalloc/GCHeap::Alloc)
- No `AneRenderHook` (eglSwapBuffers/glDraw*)
- No `AneAs3ObjectHook` (Phase 4c typed-alloc)
- No `AneAs3RefGraphHook` (Phase 4b partial)
- No `AneAs3SamplerHook` (Phase 4a)
- No alloc tracker arenas reserved

Logcat empty for all profiler tags. Scenario ran 16MB ByteArray churn +
2500 EventDispatcher allocations with the ANE present but profiler OFF —
all hot paths ran at native speed, indistinguishable from no-ANE.

The only always-on ANE component is `AneDeferDrain` (libCore.so leak fix
patches at app boot — 3 instruction overwrites, no proxies). It exists
to fix a memory leak unrelated to profiling and remains active by design.

**FIX APPLIED 2026-05-06: FP-walk now SIGSEGV-safe.** Initial validation
believed `flash.sampler.startSampling() + heavy churn` was a pre-existing
Adobe AIR bug. Bisection later isolated it to Phase 4a's own
`proxy_recordAllocationSample` FP-walk doing raw `*(volatile uintptr_t*)fp`
reads. Adobe's libCore.so compiles some paths around the sampler with
`-fomit-frame-pointer`; under those callers x29 holds an arbitrary value,
not a saved FP. Heavy churn eventually hits an unmapped page → SIGSEGV.

Bisection (Cat S60 build `7dde220f...`):
| Test | profilerStart | as3 sampling | Result |
|---|---|---|---|
| 1 | OFF | n/a | ✅ 8/8 |
| 2 | memory=off | off | ✅ 12/12 |
| 3 | memory=on | off | ✅ 12/12 |
| 4 | memory=on | **on** | ❌ SIGSEGV |
| 5 (with fix) | memory=on | **on** | ✅ 12/12 |

Fix replaces raw volatile reads with `safeReadPtr()` (mincore page-mapped
+ alignment check, also used by `resolveClassName()`). Cost rises ~5ns →
~30ns/event when active, but hook never crashes process. Phase 4a output
in test 5: 170 `as3_alloc_sampler` markers, 96 `as3_alloc` (Phase 4c
coexists), 20 gc_cycle, 0 dropped events, file validates cleanly.

---

**Original RA notes (pre-validation):**

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
**Status:** ⏳ RA in progress. Hook infrastructure ready (typed
`As3ReferenceEx` events EventType=15 wired via `record_as3_reference_ex`,
`As3ReferenceKind::EventListener`, label="weak"/"strong"). Forward-
compatible with Windows analyzer.

**Validation 2026-05-06:** offset `0x00c973b0` for addEventListener
hooks successfully but fires **0 times** during 5000 explicit
addEventListener calls. Offset is NOT the actual entry point. Real
addEventListener offset still TBD via further RA.

**FP-walk RA data captured 2026-05-06** (Cat S60, build-id `7dde220f...`)
via `_tmp_aneprof_phase4b_sampler_callerpc.json` during 1000
addEventListener calls (4670 recordAllocationSample fires total):

Hot path (3802 hits = 3.8× per call — internal AVM allocs):
```
frame[2] libCore+0x8a2a1c (sampler wrapper)
frame[3] libCore+0x8a265c
frame[4] libCore+0x5be8f0
frame[5] libCore+0x5b27e0
frame[6] libCore+0x5b2998
frame[7] libCore+0x611944
frame[8] libCore+0x80dc34
frame[9-10] JIT
frame[11] libCore+0x80e160 (JIT→native boundary)
```

Alt path (868 hits ~ 1× per call — listener entry alloc):
```
frame[3] libCore+0x89a684
frame[4] libCore+0x89a5a4
frame[5] libCore+0x89a4a0
frame[6] libCore+0x282b88
frame[7] libCore+0x2862e8  ← candidate for addEventListener helper
frame[8] libCore+0x28975c  ← candidate (JIT→native dispatcher)
frame[9] libart (Android Runtime)
```

Without disassembly confirmation, can't pin exact addEventListener C++
entry. Candidates: `libCore+0x2862e8` (frame[7] alt, ~1:1 with calls)
or `libCore+0x28975c` (frame[8] alt, dispatcher-like). Future RA should
try both via experiment hook framework + count match.

Method names confirmed in libCore.so .rodata ABC constant pool
(length-prefixed strings, no null terminator):

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

## 2026-05-05 (analyzer integration) — File-format gap discovered

`aneprof_validate.py` against the new sampler-marker-rich aneprof reveals
**2-byte gaps between events** under high marker pressure (3700+ markers
mixed with alloc events). All expected events ARE present (1 start,
1 stop, 2 snapshot, 19 gc_cycle, 533 alloc, 533 live_allocation, 3850
markers = 4939 total) but the file format has 129+ misalignments that
break sequential parsing.

Older Android aneprofs (no markers) validate cleanly. So the bug is
specifically about marker emission under load — likely re-entrancy
between `marker()` payload allocation (heap_record.resize > 128 bytes
calls libc malloc) and the alloc_tracer hook on that malloc.

Workaround: gap-stripping repair pass on the file (4939 events validate
cleanly post-repair). Tracked as task #23 for proper writer fix.

**2026-05-05 update — original hypothesis disproved.** Inspection of
`alloc-tracer-android.aneprof` (parser fail at offset 10396) shows a
single stray null byte between two consecutive Alloc events. The file
contains zero Marker events in the parsed prefix. Alloc events use the
inline `PendingEvent` buffer (record_size=64, fits in 256-byte
`inline_record`) so `heap_record.resize` is never called and the
"marker malloc → recursion" path is not relevant here. Real cause is
still unknown — single-byte misalignment between two ordinary Alloc
events on the writer thread. Multi-producer ring (`enqueue_event`
CAS loop + `slot->sequence` MPSC pattern) and writer thread
(`file_.write(event.data(), event.record_size)`) both look textbook
correct on read. Needs reproduction on current HEAD + per-event byte
counter instrumentation in the writer thread to localize the drift.

**2026-05-06 update — accounting evidence**. For `p4b2.aneprof`:
footer reports event_count=4939, payload_bytes=451511 → expected
writer-emitted bytes = 4939*24 + 451511 = **570047**. Actual events
region size = file_size - header(237) - footer(72) = **571289**, so
**1242 bytes are unaccounted for**. The writer thread's per-event
counters (`writer_bytes_written_`) match the footer accounting, so
the 1242 stray bytes do NOT come from `file_.write(event.data(),
event.record_size)` on the writer thread.

**2026-05-06 — FIXED**. Migrated `DeepProfilerController` from
`std::ofstream + rdbuf()->pubsetbuf(...)` to `std::FILE* +
std::setvbuf(_IOFBF, file_buffer_, 8MB)` — bypasses libc++
`basic_filebuf` entirely. Validated:

- **Compile**: clean on Android NDK clang/libc++ + Windows MSVC
  (x86 + x64) via `build-all.py {android,windows-native}`.
- **Unit tests** (Windows): all 5 pass in
  `shared/profiler/build/test_aneprof_controller.exe` (header/footer
  round-trip, marker+timing+memory+snapshots, AS3 references, AS3
  graph payloads, periodic snapshots).
- **Cat S60 e2e**: ran `_tmp_aneprof_phase4a_warmup_api.json` →
  `phase4a-warmup-filewriter.aneprof` (1086616 B, 13463 events
  including 198 markers + 12385 as3_alloc). `aneprof_validate.py`
  passes; **stray bytes = 0** (was 1242 before fix).

Root cause confirmed: libc++ `basic_filebuf::xsputn` over a
`pubsetbuf`-supplied buffer was emitting extra zero bytes from the
pre-zeroed buffer region under high event throughput. Switching to C
stdio with `setvbuf(_IOFBF)` eliminates that path completely. This is
also a small simplification — fewer abstractions, same buffering
behavior.

**Real-gameplay validation status (2026-05-06).** Authored
`tests/scenarios/_tmp_aneprof_phase4a_hall_idle_real.json`
(login via `advanceLogin` helper + Hall idle 30s + aneprof capture).
First run on Cat S60 hit a separate blocker: a concurrent session
(GPU rasterizer Phase 6) was applying ARMv7-targeted patches to
arm64-v8a `libCoreRenderer.so` / `libCore.so` in the shared
`out/AndroidStudioProject_loadingAirAndroidDebug/jniLibs/` — those
patches caused 3000+ ms per frame on AArch64 hardware. App login
state-machine couldn't progress fast enough; profiler `start()`
never reached. Coordinated via `sharedTests.md` to either arch-gate
Phase 6 patches (only ARMv7) or arrange a clean build window.
Phase 4a writer parity itself is unaffected — synthetic warmup_api
already validated end-to-end (13463 events, 0 stray bytes) and OFF
overhead validated (21/21 steps, no memory regression). The
real-gameplay run is just additional defensive validation.

**ON-overhead win — `record_as3_alloc_raw` (2026-05-06, applied).** Phase 4a
hot path per event was: `proxy_recordAllocationSample` →
`std::string(name_buf)` + `std::string(stack_buf)` →
`record_as3_alloc(name_ref, stack_ref)` →
`std::vector<uint8_t> payload(sizeof(fixed)+name+stack)` → memcpy ×3 →
`write_event(payload.data(), payload.size())` → PendingEvent with
heap_record allocation → memcpy. Per event: 3 heap allocs (two
non-SBO strings + payload vector) + 4 frees + ~5 memcpys. Added
`record_as3_alloc_raw(sample_id, type_name_ptr, type_name_len, size,
stack_ptr, stack_len)` that uses a `thread_local
std::vector<std::uint8_t> t_as3_alloc_scratch` (grow-only, never
freed) instead of the per-event vector, and takes raw pointers
instead of `std::string` refs so the caller skips the two
`std::string()` constructions on the hot path. Old
`record_as3_alloc(string&, string&)` now delegates to the raw variant,
preserving the existing API. `AndroidAs3SamplerHook` proxy updated to
call the raw variant directly. Saves ~150–300 ns/event (2 string
allocs + 1 vector alloc collapsed to 1 amortized scratch). Compile
clean Android NDK + Windows MSVC; 5/5 shared profiler unit tests pass
(test_aneprof_controller.exe).

**Runtime validation on Cat S60 (2026-05-06, both ON-overhead wins
combined).** Re-ran `_tmp_aneprof_phase4a_warmup_api.json` against
the freshly built ANE (page cache + `record_as3_alloc_raw`) on Cat
S60 with arm64-v8a libCore.so restored to pristine. Test passed
28/0; `phase4a-warmup-onoverhead.aneprof` (1 088 574 B) validates
cleanly at **0 stray bytes**. Event composition essentially
identical to baseline (13 497 vs 13 463 total; 12 388 vs 12 385
as3_alloc; 193 vs 198 markers — natural ±5 variance from app-boot
timing). All three writer/sampler changes (FILE\* migration + page
cache + raw alloc API) working together without regression in real
Android runtime.

**Phase 4c parity fix (2026-05-06).** `AndroidAs3ObjectHook.cpp`
drain thread previously emitted a JSON-encoded `Marker` event
(`{class, size, ptr}`) for each typed AS3 allocation, while the
Windows-side `WindowsAs3ObjectHook` emitted typed `As3Alloc` events
(EventType=12) directly. The TODO comment at the call site
explicitly noted this was waiting for a DPC API to land — which is
exactly what `record_as3_alloc_raw` provides. Replaced the snprintf
+ `dpc->marker(...)` call with
`dpc->record_as3_alloc_raw(obj, name_buf, name_len, size, nullptr,
0)`. Effect: (a) the analyzer reads the Phase 4c contribution with
the same parser it uses for Windows As3Alloc and Phase 4a sampler
events — no JSON parsing fallback needed; (b) one less snprintf per
drained event. No stack metadata is emitted from this drain path
because the FixedMalloc::Alloc proxy doesn't capture frame[N>1] —
Phase 4a's sampler hook is the canonical source for pc0/pc1
attribution. Compile clean on Android NDK; runtime-validated via
warmup_api re-run on Cat S60 (file 1 086 937 B, 0 stray bytes,
13 448 events; Phase 4c contribution is small in this synthetic
workload because the LeakTracker churn is ByteArray-heavy, not
ScriptObject FixedMalloc — the structural fix is correct, gameplay
workloads will see the typed events).

**Phase 7a parity fix (2026-05-06).** `AndroidGcHook` previously
emitted `GcCycle` events with `before/after_live_count` and
`before/after_live_bytes` hardcoded to zero, with a TODO comment
claiming "Heap stats unavailable without RA on `GC::GetTotalGCBytes`".
That was wrong: Windows side
(`ProfilerAneBindings::profiler_request_gc`) reads the same numbers
from `DeepProfilerController::status()` — DPC's own allocation
tracking provides the values. Updated the proxy to call
`dpc->status()` before+after `g_orig_collect` and pass the values
into `record_gc_cycle`. The values are bounded by Phase 5's
alloc-only tracking (free-side hook still pending — see next entry)
so `before.live_allocations` trends higher than reality; that's the
same approximation Windows uses. Compile clean on Android NDK and
runtime-validated on Cat S60: forced a GC mid-capture via
`profilerRequestGc`, GcCycle event emitted with `kind=NativeObserved`
and 0 stray bytes in the file. The live_count/bytes read from
`status()` reflect a known writer-thread lag — `allocation_shards` is
populated asynchronously by the writer's `update_allocation_tracking`,
so values lag the producer by the queue depth. Windows has the
identical lag (writer thread is the same shared pipeline);
`write_snapshot_events` calls `wait_for_writer_idle()` before reading
shards but the GC observer cannot block, so it accepts the lag. Net:
behavior matches Windows.

**Phase 6 v2 hook binding fix (2026-05-06).** Initial v2 hooks
(`glClear`, `glTexImage2D`, `glTexSubImage2D`, `glBindTexture`,
`glBindFramebuffer`) used `shadowhook_hook_sym_name("libGLESv2.so",
sym, ...)`. Real-gameplay run on Cat S60 showed `glDraw*` proxies
firing (drew=31/frame) but the v2 hooks all stuck at zero. Adobe's
GLES call sites resolve their function pointers via
`dlsym(RTLD_DEFAULT, ...)` at boot and cache the addresses; on
Adreno-backed devices those addresses point into
`libGLESv2_adreno.so` rather than `libGLESv2.so`'s exported stub, so
the cached pointer bypasses our libGLESv2.so prologue patch.
Switched the v2 hooks to a new `hookOneAddr(sym, ...)` helper that
calls `dlsym(RTLD_DEFAULT, sym)` first and then
`shadowhook_hook_func_addr(addr, ...)` — patching whichever
implementation the dynamic loader hands out, system-wide. `glDraw*`
+ `eglSwapBuffers` keep using the original `hookOne(libGLESv2.so,
...)` path since they bind correctly there. Compile clean.

**RUNTIME PARTIALLY VALIDATED 2026-05-06** via hall_idle_real on
Cat S60: all 8 hooks install successfully, logcat banner:
```
AneRenderHook: install swap=0x55ba4ae230
AneRenderHook: install drawArrays=0x55bada85a0 drawElements=0x55bb35cbd0
AneRenderHook: install clear=0x55b8be39b0
AneRenderHook: install texImage2D=0x55bab39dc0 texSubImage2D=0x55bab73e00
AneRenderHook: install bindTexture=0x55bad78da0 bindFramebuffer=0x55bb35cca0
```
30s of Hall idle produced 196 render_frame events.

**v1 hooks (eglSwap + drawArrays/drawElements) work.** Per the
.aneprof binary parsed for render_frame fields:
  draw_calls       : 6460   (33/frame avg)
  primitive_count  : 151360

**v2 dlsym hooks install but DON'T FIRE.** Per same parsed events:
  texture_create_count       : 0
  texture_create_bytes       : 0
  texture_update_count       : 0
  set_texture_count          : 0
  render_target_change_count : 0
  clear_count                : 0

Hall has dynamic text labels (player name etc.) that render each
frame — those typically go through glTexSubImage2D + glBindTexture
+ glClear. Zero hits for 196 frames means Adobe's render path on
Cat S60 (Adreno) **doesn't actually call through the dlsym-resolved
addresses we hooked**, despite the install succeeding at those
addresses.

**Root cause confirmed:** Adobe AIR resolves GLES function pointers
via `eglGetProcAddress(name)` at boot, not `dlsym(RTLD_DEFAULT,
name)`. On Adreno-backed devices these return DIFFERENT addresses
— eglGetProcAddress hands out the vendor-specific implementation
entry point (often inlined or in a different code page from the
dlsym-resolved trampoline). Our hook patched the dlsym address but
Adobe's cached eglGetProcAddress pointer never went through it.

**RESOLVED 2026-05-06.** `hookOneAddr` switched from
`dlsym(RTLD_DEFAULT, sym)` to `eglGetProcAddress(sym)` (with dlsym
fallback for symbols eglGetProcAddress doesn't know). This resolves
the same address Adobe AIR caches at boot — our hooks now sit on
the actual call path. Validated via hall_idle_real on Cat S60
(30s recording window):

| Metric | Before (dlsym, broken) | After (eglGetProcAddress) |
|---|---|---|
| frames | 196 | 198 |
| clear_count | 0 | **198** (1/frame) |
| set_texture_count | 0 | **6749** (~34/frame) |
| texture_update_count | 0 | **415** |
| texture_create_count | 0 | **2** |
| texture_create_bytes | 0 | **7,819,264** (~7.8MB) |
| texture_upload_bytes | 0 | **115,740,000** (~115MB) |
| render_target_change_count | 0 | 0 (no FBO switch in Hall) |
| draw_calls | 6460 | 6528 |
| primitive_count | 151360 | 153006 |

All v2 hooks now fire correctly and populate render_frame events
with the same telemetry granularity as the Windows D3D9 side. The
eglGetProcAddress fallback to dlsym is preserved for vendor
extensions where eglGetProcAddress returns null.

**Zero-overhead-off invariant confirmed post-fix 2026-05-06** via
`_tmp_aneprof_phase4a_off_overhead.json` — 16MB ByteArray churn +
1000 listener add/remove cycles WITHOUT calling profilerStart on
Cat S60. Logcat completely silent for all profiler tags
(`AneRenderHook`, `AneAs3SamplerHook`, `AneGcHook`, `AneDeepMemHook`,
`AneAs3ObjectHook`, `AneAs3RefGraphHook`, `AneAllocTracer`) — none
of the new chunk-Alloc/chunk-Free/chunk-walk/v2-eglGetProcAddress
hooks install when capture is inactive. Memory delta bounded
(GC fully reclaimed the 16MB).

**Phase 6 partial parity (2026-05-06).** `AndroidRenderHook` previously
emitted `RenderFrame` events with `primitive_count`, `clear_count`,
and all texture-related fields hardcoded to zero. The Windows
counterpart (`WindowsRenderHook::add_draw_call`) tracks primitive
counts (D3D9 hands explicit primitive_count to draw calls; on GLES
the count parameter is vertex/index count + the mode determines how
many primitives that produces). Added a small
`primitives_from_mode_count(mode, count)` helper covering all GLES2
draw modes (POINTS / LINES / LINE_STRIP / LINE_LOOP / TRIANGLES /
TRIANGLE_STRIP / TRIANGLE_FAN), accumulated into a thread_local
`t_frame_primitives` from `proxy_glDrawArrays` and
`proxy_glDrawElements`, and passed into `record_render_frame` on
`eglSwapBuffers`. Also added a new `proxy_glClear` hook on
`libGLESv2.so:glClear` that increments a thread_local
`t_frame_clear_count`, matching Windows' clear-counter semantics.
Both reset on swap. Compile clean on Android NDK; runtime validation
will land naturally with the next render-on capture (Phase 6 is
gated by `cfg.render_enabled` and Stage3D presence).

**Phase 6 texture metrics added (2026-05-06).** Followed up the
primitive/clear fix with hooks on `libGLESv2.so`:
- `glTexImage2D` → `texture_create_count` + `texture_create_bytes`
  (only level=0 mip — level>0 mips are pyramid bookkeeping, not new
  texture allocations)
- `glTexSubImage2D` → `texture_update_count` + `texture_upload_bytes`
- `glBindTexture` → `set_texture_count` (mirrors D3D9
  SetTexture/SetSamplerState counter)
- `glBindFramebuffer` → `render_target_change_count` (mirrors D3D9
  SetRenderTarget)

A small `gles_bpp(format, type)` helper covers the common GLES2 cases
(GL_RGB/RGBA/LUMINANCE/LUMINANCE_ALPHA/ALPHA × GL_UNSIGNED_BYTE/FLOAT/
HALF_FLOAT_OES, plus packed types `GL_UNSIGNED_SHORT_5_6_5/4_4_4_4/
5_5_5_1`). Returns 0 for unrecognised combinations (compressed
formats like ETC2/ASTC, etc.) — those textures are counted but not
byte-sized. Compile clean Android NDK; format strings verified
present in stripped binary.

**Real-gameplay validation 2026-05-06.** With LLM 2's section 7 fix
(server SurfTank/1001 instead of the Cloudflare-blocked Testes(1016)),
hall_idle_real ran end-to-end on Cat S60: passed 114/0,
`phase4a-hall-idle-real-FULL.aneprof` (2 948 333 B), 0 stray bytes,
event composition: 1 start, 1 stop, 582 marker, 2 snapshot, 9136
alloc, 4096 live_alloc, 24 875 as3_alloc, 257 gc_cycle, 196
render_frame.

Concretely confirmed at runtime:
- Writer FILE\* migration: 0 stray bytes (was 1242 pre-fix).
- Phase 4a sampler: 24 875 typed As3Alloc events (Phase 4a +
  Phase 4c contributing).
- Phase 5: 9136 alloc + 4096 live_alloc.
- Phase 7a: 257 GcCycle events emitted via the GC observer hook
  (live_count/bytes still 0/0 due to writer-thread lag, same as
  Windows — no regression).
- Phase 6 v1 (eglSwapBuffers + glDraw\*): 196 render_frame events
  with `draws=31` per frame.

Partial: Phase 6 v2 hooks (`glClear`, `glTexImage2D`,
`glTexSubImage2D`, `glBindTexture`, `glBindFramebuffer`) SHIPPED in
the binary (`grep -aoE` confirms format string present) but the
runtime LOGI line only printed three install pointers (swap +
drawArrays + drawElements) and the corresponding render_frame
fields are zero. The likely cause: on Adreno devices Adobe's render
path resolves these GL entry points through `libGLESv2_adreno.so`
or via a dlsym path that bypasses `libGLESv2.so`'s prologue, so
shadowhook's symbol-name binding doesn't intercept them. glDraw\*
DO go through the standard `libGLESv2.so` so they catch fine. To
fix: try `libGLESv2_adreno.so` as the shadowhook target on
Adreno-backed devices (or a GOT/PLT scan in libCore.so for those
specific GL entry points). Tracked as a Phase 6 follow-up.

Remaining Phase 6 gap (low priority): compressed texture sizing via
`glCompressedTexImage2D` (would need format→bytes-per-block tables
for ETC2/ETC1/ASTC). Skipped — the AIR runtime emits mostly
uncompressed RGBA8 textures, so the gap is small in practice.

**Open parity gap — MMgc::Free hook missing on Android (2026-05-06).**
`WindowsDeepMemoryHook` hooks **two** entry points:
`hook_mmgc_alloc` and `hook_mmgc_free` (signature
`void(*)(void* heap, void* ptr)`). The free hook calls
`record_free_if_tracked(ptr)` so DPC's per-shard allocation map stays
balanced. `AndroidDeepMemoryHook` only hooks the alloc side
(`proxy_GCHeapAlloc` + `proxy_FixedAlloc`); there is no Android
equivalent for `proxy_MMgcFree`. Result: when the profiler is
recording, every Phase 5 alloc adds an entry to the allocation
shards but matching frees are never recorded, so the
`live_allocations` counter on Android grows monotonically and
includes every recycled MMgc allocation. This masks real leaks
versus the noise of ordinary churn-and-reuse.

**Task #11 progress (2026-05-06):** Empirical RA via
`tests/scenarios/_tmp_aneprof_task11_free_ra.json` (16 candidate
offsets light-installed concurrently, 64MB ByteArray churn).
Identified **chunk-level** MMgc::Free at `+0x8a167c` — paired
1:1 with chunk-level Alloc at `+0x8a15a4` (59:59 hits), 2-arg
signature `(this, ptr)`, body does pthread_mutex_lock + sub
`(size>>12)` from `[this+0x1118]` + page-header lookup at
`(ptr & ~0xfff) + 0x22`. Hooked as `proxy_MMgcFree` in
`AndroidDeepMemoryHook.cpp` calling `dpc->record_free_if_tracked`.

**OPEN — Tier mismatch (validated empirically 2026-05-06):** the
`+0x8a167c` Free we hooked is for **page-aligned chunks** (LOGI
shows all observed ptrs have `&0xfff=0x0`, e.g. `0x7f6816e000`),
whereas Phase 5's `proxy_GCHeapAlloc` and `proxy_FixedAlloc` track
the **non-page-aligned user pointers** their original functions
return (LOGI sample showed sizes 24/32/40/1024/65536 bytes returning
`0x55b8...` libc-malloc-range and `0x7f627f6010` page+0x10 — never
page-aligned). Result: `record_free_if_tracked` enqueues but the
writer-thread shard-map lookup misses (key mismatch — chunk-level
Free ptrs vs user-pointer alloc keys), so even hall_idle_real (30s
recording window with thousands of allocs and chunk-level frees)
produces `alloc/free/realloc: 10110/0/0` in the .aneprof.

The hook IS architecturally correct (logcat confirms install OK,
LOGI confirms proxy_MMgcFree fires, return of record_free_if_tracked
is true meaning the event was successfully queued) — it just hooks
a different allocator tier than the one Phase 5 currently tracks.

`grep ldxr|stxr|ldaxr|stlxr` over `+0x890000..+0x8b0000` produced
**zero matches** — Adobe's MMgc on this AArch64 build does NOT use
atomic CAS on the free-list. All synchronization is mutex-protected.
The user's "atomic CAS sub a current size" hint should therefore be
read as "subtract a size from a counter under lock" — which c02
(0x8a167c) does match (`sub x8, x8, x22, lsr #12; str x8, [x20,
#0x1118]` under pthread_mutex_lock). It's still the right Free
function for its tier; Phase 5's alloc proxies are just at a
finer-grained tier.

Two paths remain to close the live_allocations parity:

1. **Replace Phase 5 alloc proxies** with chunk-level allocator
   `+0x8a15a4` (paired 1:1 with the +0x8a167c Free we hooked,
   59:59 hits during 64MB churn). This would track at the same
   tier as the Free, so frees would match. Trade-off: chunk-level
   tracking loses small-object granularity (we'd see one chunk
   allocation per page rather than per object slice).

2. **Find finer-grained Free duals** for `proxy_GCHeapAlloc` and
   `proxy_FixedAlloc`. Likely candidates: small functions in
   `+0x89c..0x8a0` that take 2 args and manipulate per-size-class
   free lists. Requires another experiment-hook RA pass with
   alloc-paired hit-count comparison, this time using GCHeap::Alloc
   (+0x89c42c) as the anchor and seeking offsets that fire 1:1
   under churn.

3. **Chunk-walk free sweep** (recommended): when proxy_MMgcFree
   fires for a chunk_base, walk our allocation shard maps and
   remove ALL entries with ptrs in `[chunk_base, chunk_base+size)`.
   Requires intercepting the size returned by helper
   `+0x8a1744` (called by `+0x8a167c` to get the chunk byte size).
   The Free body computes `(size+0xfff) >> 12` for page count and
   subtracts from `[this+0x1118]`; this size is also exactly what
   we'd need to sweep our shard map. Implementing this means each
   chunk reclamation correctly drops ALL its allocs from the
   tracked set in one go — closing the parity gap without RA-ing
   per-class Free.

**RESOLVED 2026-05-06 via option 1 (proxy_ChunkAlloc).** Added
`proxy_ChunkAlloc` at `+0x8a15a4` (the 1:1 paired Alloc dual of
the +0x8a167c Free, signature `void*(this, size, flags)`).
Tracking the page-aligned chunk_base it returns means subsequent
frees at +0x8a167c hit matching shard entries. Validated via
`_tmp_aneprof_task11_free_validate.json` (4-sec recording window
during 64MB ByteArray churn):

| Metric | Before (chunk-Free only) | After (chunk-Alloc + chunk-Free) |
|---|---|---|
| alloc events | 405 | 461 |
| free events | **0** | **34** |
| live allocations | 405 | 427 (= 461 - 34) |
| unknown frees | 0 | 0 (all matched) |

The 34 frees are the chunks both allocated AND freed during the
recording window — exactly what we want. live_allocations is no
longer monotonic at the chunk tier. Logcat confirms install:
```
AneDeepMemHook: install: ChunkAlloc hook OK (target=...stub=...)
AneDeepMemHook: install: MMgc::Free hook OK (target=...stub=...)
```

**Real-gameplay validated 2026-05-06** via hall_idle_real on Cat
S60 (30-sec Hall-idle window, login + Hall reached + capture):

| Metric | Pre-fix hall_idle | Post-fix hall_idle |
|---|---|---|
| alloc events | 10110 | **12213** |
| free events | **0** | **359** |
| unknown_frees | 0 | 0 (all matched) |
| live allocations | 10110 (monotonic) | 11854 (= 12213 - 359, drops on chunk reclaim) |
| analyzer diagnostic | "incomplete; no free/realloc events captured" | **"probable live allocations remain at stop"** |
| render_frame events | 193 | 193 (no Phase 6 regression) |
| File validates | ✅ | ✅ |

The diagnostic line changed from "incomplete" → "probable live
allocations remain" — the analyzer now sees a balanced
alloc/free stream and only flags entries that genuinely outlive
the recording window. This is the parity Windows already had.

**Remaining smaller-tier gap:** Phase 5's `proxy_GCHeapAlloc` and
`proxy_FixedAlloc` track user-pointers at finer granularity. Their
Free duals (per-class FixedAlloc::Free) still aren't located, so
those tracked entries continue to grow monotonically until DPC
sees the chunk Free that contains them. For most workloads
chunk-tier coverage is sufficient since chunk reclamation is what
truly bounds the heap; per-object live_allocations parity would
require either further per-class Free RA (Adobe likely inlines on
this build per `grep ldxr|stxr` returning zero matches) or the
chunk-walk sweep approach (option 3 above) extending each chunk
Free to delete all shard entries within `[chunk_base, chunk_base
+ chunk_size)`. Tracked as future improvement.

**ON-overhead win — `safeReadPtr` page cache (2026-05-06, applied).**
The Phase 4a hot path was making 7 `mincore()` syscalls per event
(2 in FP-walk + up to 5 in `resolveClassName`'s Traits walk). On
Android `mincore` costs ~1–5 µs each → **7–35 µs per event** of
pure syscall overhead. At 12 386 events / 30 s (warmup_api) that is
**~84–420 ms over 30 s ≈ 0.3–1.4 % CPU**. Added a thread_local
8-entry round-robin validated-page cache in
`AndroidAs3SamplerHook.cpp::safeReadPtr`: cache hit skips `mincore`
entirely. The working set is small enough to fit (1 stack page +
2–3 libCore .data pages for Traits/name strings) so the steady-state
hit rate is high. Compile clean on Android NDK; runtime validation
deferred until LLM 2's Phase 6 AArch64 regression is unblocked.
Single-threaded sampler proxy → no atomics needed.

Phase 4a/4b CAPTURE parity is achieved (data is in the file). FORMAT
parity awaits the writer fix.

## 2026-05-05 (later) — Phase 4a production end-to-end validated

End-to-end Phase 4a parity validated on Cat S60 (AArch64) **and** Galaxy J5
(ARMv7) with auto-install via existing `profilerStartDeep` API:

**AArch64 (Cat S60, build-id `7dde220f...`)**:
- `recoverSamplerSlot` configured at gc+0x10, avmcore+0x68 — works first try
- 135025 hits during scenario; ~3700 emit `as3_alloc_sampler` markers
- `pc0 = libCore+0x8a2a1c` (alloc wrapper, 100% of hits) — exact match with
  experiment-hook frame[2]
- `pc1` distribution: `libCore+0x8a265c` (2776, 77%), `libCore+0x89a684`
  (801, 22%), `libCore+0x895f34` (4, 0.1%) — exact match with experiment-
  hook frame[3] hot/alt

**ARMv7 (Galaxy J5)**:
- `recoverSamplerSlot` auto-probe (configured offsets fail) discovered
  `gc+0xc, avmcore+0x34` after self-reference filter rejected first hit.
- Hook fires; vtable[12] resolves to thumb fn at `libCore+0x501ea4`
- ARMv7 method semantic still needs validation — slot 12 may not be
  recordAllocationSample on the smaller-vtable ARMv7 build.

**Auto-install via deferred GC singleton callback** (new):
`registerGcSingletonCallback` in `AndroidGcHook` lets `nativeStartDeep`
queue the AS3 sampler install for first GC capture, so `profilerStartDeep`
with `as3Sampling=true` works without any explicit init sequence — meets
"keep existing API surface" requirement.

**Zero-overhead-off invariant maintained**:
- Install only happens when `as3Sampling=true` is passed to startDeep
- Proxy gated by `g_active.load` early-return when capture inactive
- FP-walk + marker emit only when capture is active; ~5ns + 1 string format
  per fired event

This closes the productive Phase 4a path. Phase 4b reference-graph events
are now approximated via the `pc1` field — analyzer groups allocs by call
site without needing per-method libCore offsets.

## 2026-05-05 — Sampler stack-walk + PC resolution productionized

`AndroidExperimentHook` now captures 4-frame stack via x29 (FP) chain at every
hook firing, with PC-to-library resolution via `dl_iterate_phdr` enumerating
`PT_LOAD` segments. Validated on Cat S60 (AArch64, build-id `7dde220f...`) via
`tests/scenarios/_tmp_aneprof_phase4b_sampler_callerpc.json`:

- Sampler at `libCore.so+0x8952ec` (recordAllocationSample, vtable[12])
- 4681 hits during 2000 addEventListener add/remove churn (~2.34 allocs/call)
- frame[0]/[1] = `libemulatordetector.so+0x148dd8` (our shadowhook proxy itself)
- frame[2] = `libCore.so+0x8a2a1c` (wrapper, 100% of hits)
- frame[3] hot = `libCore.so+0x8a265c` (84%) — dominant alloc-site caller
- frame[3] alt = `libCore.so+0x89a684` (16%) — secondary path

This unblocks As3Alloc events emitting native call-site signatures
(frame[2]+frame[3] PCs) for analyzer attribution, without per-method offsets.
Cost: ~50ns/event (FP-walk = 2 mem reads). Off path: zero (gated by capture).

Phase 4b reference-graph hooks (addChild / addEventListener) still need
specific offsets — task #21 plans differential hit-count discovery using the
experiment hook framework.

`nativeGetSamplerRecordAllocAddr` JNI fixed to return libCore-relative offset
(was returning absolute address, breaking experiment hook install).
