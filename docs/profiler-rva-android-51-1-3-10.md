# Profiler Hook Offsets for Adobe AIR 51.1.3.10 (Android `libCore.so`)

Static binary analysis of `libCore.so` (the AIR Android runtime, analogue of
`Adobe AIR.dll` on Windows) for the `.aneprof` backend hooks. All offsets are
relative to the loaded `libCore.so` base address — at runtime,
`AndroidGcHook` / `AndroidDeepMemoryHook` resolve them against the in-memory
load address obtained via `dl_iterate_phdr`.

> **Pinning by build-id.** Android `libCore.so` ships without symbol tables,
> and Adobe scrubbed all C++ RTTI (`_ZTV*`/`_ZTI*`) and most exports.
> Offsets vary across SDK builds and architectures, so each hook keeps a
> `kKnownBuilds[]` table mapping the GNU build-id → known offsets. Unknown
> build-ids fall back to a logged warning and disable the affected hook
> rather than wild-patching memory.

## Target binaries

| Architecture | Path | Build-ID (GNU) | Size |
|---|---|---|---|
| arm64-v8a   | `C:/AiRSDKs/AIRSDK_51.1.3.10/lib/android/lib/arm64-v8a/libCore.so`   | `7dde220f62c90358cfc2cb082f5ce63ab0cd3966` | 49.0 MB |
| armeabi-v7a | `C:/AiRSDKs/AIRSDK_51.1.3.10/lib/android/lib/armeabi-v7a/libCore.so` | `582a8f65b8dcb741e5eb869ccf9526137270d99e` | 14.2 MB |

Devices used for live validation:
- arm64-v8a → OnePlus 15 (PVP 30min + PVP 3x scenarios)
- armeabi-v7a → Galaxy A10 (PVP 30min smoke)

## Method

The Windows analysis (51.1.3.12 Ghidra project at
`C:/AiRSDKs/AIRSDK_51.1.3.12/binary-optimizer/`) provides ground-truth
function identities — Adobe annotated the Windows DLLs with full vtable/class
names. The Android binaries are stripped, so cross-arch correlation went:

1. **String anchors** — telemetry/scout markers (`.gc.CollectionWork`,
   `MMgcThrowOutOfMemory`, `gcheap-arena`, `GCHeap::Decommit`) survive on
   Android in `.rodata` and provide unambiguous xrefs.
2. **Structural pattern match** — once a function is identified by string,
   its body shape (size-class lookup, free-list pop, mutex acquisition) is
   matched against the analogous Windows decompile to confirm.
3. **Build-id pinning** — Adobe rebuilds with each SDK bump regenerate
   layout. The GNU build-id (32 bytes from `PT_NOTE`) is stable per build
   and used as the offset-table key.
4. **Live validation** — every offset in `kKnownBuilds[]` is exercised by
   the corresponding hook before release. PVP 30min on each device confirms
   no SIGSEGV at the patched site and the expected event flow.

Tooling: `tools/ghidra-scripts/` contains the headless Java scripts used:
- `find_avm2_natives_v3.java` — substring search + xref enumeration for AS3
  method names
- `find_xrefs_to_addr.java` — generic xref-to-address finder
- `find_bl_calls_to.java` — AArch64 BL scanner for direct callers
- `list_funcs_near.java` — function inventory in an offset window

## Offsets

### Phase 5 — `MMgc::FixedMalloc::Alloc` + `MMgc::GCHeap::Alloc`

`AndroidDeepMemoryHook` shadowhooks the per-size-class fast path inside
`FixedMalloc::Alloc` (free-list pop, no libc call) and the chunk-grow path
in `GCHeap::Alloc`. Combined coverage on PVP 3x ARM64: **6.3M alloc events
in 802MB capture, 547× more than GCHeap-only** — confirms the FixedMalloc
fast path is the dominant runtime allocator inside MMgc.

| Build-ID | `GCHeap::Alloc` | `FixedMalloc::Alloc` | Notes |
|---|---|---|---|
| `7dde220f...` (arm64-v8a) | `+0x89c42c` | `+0x8a11d8` | Both hooks active |
| `582a8f65...` (armeabi-v7a) | `+0x5541cd` (Thumb +1) | n/a | FixedMalloc inlined into GCHeap callers; only GCHeap hook active |

ARMv7 toolchain inlined `FixedMalloc::Alloc` into the GCHeap callers — the
size-class binner uses `__aeabi_uidivmod` for `/104` (entry stride) where
ARM64 uses `madd`. Static RA can't isolate a clean entry. Coverage on
ARMv7 is 10–30× lower than ARM64 in PVP captures — accepted tradeoff
(documented in PROGRESS.md Iter N+19).

### Phase 7a — `MMgc::GC::Collect` (observer + programmatic trigger)

`AndroidGcHook` shadowhooks `GC::Collect` to emit `GcCycleEvent`s on each
runtime collection. The hook also stashes `this` (x0/r0) on first fire as
the recovered GC singleton — `requestCollect()` re-invokes the original
with the captured pointer, satisfying the programmatic trigger requirement
without finding a global pointer.

| Build-ID | `GC::Collect` | Notes |
|---|---|---|
| `7dde220f...` (arm64-v8a) | `+0x896824` | Body emits `.gc.CollectionWork` Scout marker |
| `582a8f65...` (armeabi-v7a) | `+0x5501e1` (Thumb +1) | Same logical structure, ARMv7 alignment differs at `this` flags offset |

Static call-site analysis (`find_bl_calls_to.java`) found **zero direct BL
callers** on ARM64 — Adobe dispatches `Collect` via fn-pointer/vtable.
Combined with stripped MMgc RTTI, chasing the global GC pointer through
.bss/.data is impractical (multi-week, low confidence). The runtime
singleton capture is the canonical solution.

### Phase 6 — EGL/GLES render hooks

Not in `libCore.so`. `AndroidRenderHook` shadowhooks the Android system
libraries — `libEGL.so:eglSwapBuffers` and `libGLESv2.so:glDrawArrays` /
`glDrawElements`. Vendor-agnostic (works on Adreno, Mali, Xclipse).
No build-id pinning needed; symbol resolution via `dlsym`.

### Phase 3 — AS3 method walker (TLS-probe based)

`AndroidAirRuntime` does not use static offsets. Instead it probes the
thread-local CallStack at install time to discover the offset where AVM2
records the active `MethodInfo*`. Cross-arch and cross-SDK robust by
construction. See `AndroidAirRuntime.cpp` for the probe protocol.

### Phase 0 — `alloc_tracer` libc shadowhooks

PLT/GOT patches on `libc.so:malloc/calloc/realloc/free/mmap/munmap`. No
`libCore.so` offsets needed. Reentrancy fix (Iter N+15) moved
`t_in_tracer` guard to proxy entry with a size-filter-first ordering to
avoid TLS access on small allocations (NDK r28 `__emutls_get_address`
calls libc malloc on first thread_local touch — recursion).

## Permanent NO-GOs (RA blocked)

These phases are not implementable on Android with current binaries:

### Phase 4a — `IMemorySampler` typed AS3 alloc

Adobe stripped the entire `IMemorySampler` interface from Android
`libCore.so`. No vtable, no string anchors (`presample`,
`recordAllocationSample`, `recordDeallocationSample` not present), no
indirect xrefs. Confirmed via headless string scan + symbol enumeration in
Iter N+18.

**Plan B** (direct `MMgc::GC::Alloc`+Traits walk) is also blocked: Adobe
stripped all `_ZN5MMgc*` typeinfo so the Traits pointer at the call site
can't be resolved to a class name without parallel avmplus source
matching (multi-week, low confidence). Skipped.

### Phase 4b — AS3 reference graph

Same RTTI strip blocks `DisplayObjectContainer::addChild` /
`EventDispatcher::addEventListener` resolution. The AS3 method-name strings
that DO survive in `.rodata` are part of the embedded SWF ABC string pool
(packed Pascal-prefixed pool at e.g. ARMv7 `+0xc89b00`), not a runtime
native dispatch table. Adobe's AVM2 dispatches AS3 methods via 32-bit
indices in compiled Trait tables — no name-based runtime lookup exists to
hook.

Skipped.

## Verification

Cross-arch device matrix:

| Phase | OnePlus 15 (arm64) | Galaxy A10 (armv7) |
|---|---|---|
| 0 — alloc_tracer reentrancy | ✅ PVP 30min, 0 SIGSEGV, >1M allocs | ✅ PVP 30min, 0 SIGSEGV |
| 3 — AS3 method walker | ✅ method_id non-zero in capture | ✅ method_id non-zero in capture |
| 5 — GCHeap deep hook | ✅ events flowing | ✅ events flowing |
| 5 ext — FixedMalloc fast-path | ✅ 547× coverage in PVP 3x | ⚠ inlined, GCHeap-only |
| 6 — RenderHook | ✅ ~3600 RenderFrame events / 60s | ✅ |
| 7a observer — GC::Collect | ✅ cycles emitted | ✅ cycles emitted |
| 7a programmatic — requestGc | ✅ singleton captured + re-invoked | ✅ same |
| 7b — recordFrame | ✅ Frame events | ✅ Frame events |

Tooling for SDK-bump verification:
1. Pull `libCore.so` from new `AIRSDK_*/lib/android/lib/{arm64-v8a,armeabi-v7a}/`
2. Compute build-id via `readelf -n libCore.so | grep "Build ID"`
3. Run Ghidra headless with anchor scripts (`find_avm2_natives_v3.java`)
4. Confirm string anchors `.gc.CollectionWork`, `MMgcThrowOutOfMemory`,
   `gcheap-arena` still present
5. Locate new offsets by structural match against the previous build's
   decompile
6. Append entry to `kKnownBuilds[]` in each hook
7. Live PVP 30min on both devices to validate

## Pulling captures off-device — binary safety

When pulling `.aneprof` files via `adb`, **always use `adb exec-out`**, NOT
`adb shell cat`. The shell variant routes through a TTY which performs
LF→CRLF translation, corrupting binary records (1-byte gap inserted after
every 0x0a byte in the stream). A 683 KB capture had 1152 such corruptions,
making the validator reject the file as malformed.

```bash
# CORRECT — binary-safe
adb -s <serial> exec-out 'run-as br.com.redesurftank.android cat "<path>"' > local.aneprof

# WRONG — TTY translation corrupts binary
adb -s <serial> shell 'run-as br.com.redesurftank.android cat "<path>"' > local.aneprof
```

The validator output `unknown event type 1792 at offset N` (1792 = 0x0700,
i.e. `EventType::Alloc << 8`) is the canonical symptom of CRLF-corrupted
pulls. Re-pulling with `exec-out` resolves it.

## References

- `AndroidNative/app/src/main/cpp/profiler/AndroidGcHook.{cpp,hpp}`
- `AndroidNative/app/src/main/cpp/profiler/AndroidDeepMemoryHook.{cpp,hpp}`
- `AndroidNative/app/src/main/cpp/profiler/AndroidRenderHook.{cpp,hpp}`
- `AndroidNative/app/src/main/cpp/profiler/AndroidAirRuntime.{cpp,hpp}`
- `AndroidNative/app/src/main/cpp/alloc_tracer.cpp`
- `tools/ghidra-scripts/*.java` — headless RA tooling
- `docs/profiler-rva-51-1-3-10.md` — Windows counterpart (full parity reference)
- `tools/crash-analyzer/PROGRESS.md` (in `ddtank-client` repo) — iteration history
