# Mode B — AS3 function sampling limitation

**Status**: Mode B captures everything except AS3 function-level samples.
Scout's Top Activities / ActionScript panel stays empty on Mode B captures.

## What works in Mode B

- Frame timing, CPU usage, memory (managed/bitmap/total/script)
- Render events (`.rend.*`), dirty-region painting
- Span activities (GC mark/sweep/reap, actions)
- DisplayList rendering when `displayObjectCapture=true`
- Stage3D events when `stage3DCapture=true`
- All custom markers (`flash.profiler.Telemetry.sendMetric` / `sendSpanMetric`)

Scout renders these correctly. The timeline, frame-time bars, memory chart,
and DisplayList hierarchy all populate as expected.

## What does NOT work in Mode B

No `.sampler.sample`, `.sampler.methodNameMap`, or `Function/...` records
reach Scout. Consequence:

- **Top Activities** (function-level cost breakdown): empty
- **ActionScript panel**: empty with "Compile with -advanced-telemetry" hint
- **Method-level CPU attribution**: unavailable

## Why — the root cause

`AvmCore::ctor` (RVA 0x00209351 on x86) instantiates a
`MemoryTelemetrySampler` object only when `[Player+0xDC0]` (PlayerTelemetry)
is non-null at AvmCore construction time. This gate is evaluated exactly
once, during player startup, and cannot be re-evaluated later:

```
// Inside AvmCore::ctor at RVA ~0x2095xx
eax = [AvmCore+0x5E8];           // = Player
edi = [Player+0xDC0];             // = PlayerTelemetry (null if no .telemetry.cfg)
if (edi == 0) goto skip_sampler;  // ← Mode B always hits this
// else: allocate + ctor sampler, store at AvmCore+0x5F4
```

In Mode B the flow is:
1. AIR startup → AvmCore::ctor runs with PlayerTelemetry == null → sampler
   skipped, AvmCore+0x5F4 remains null forever.
2. AS3 VM initialises, JIT-compiles methods.
3. User triggers profilerStart. ANE populates Player+0xDC0 with a fresh
   PlayerTelemetry, constructs SocketTransport/Telemetry chain — too late
   for the AvmCore gate.

## What we tried (best-effort sampler install)

`WindowsAirRuntime::forceEnableTelemetry` replays the sampler sub-block
that AvmCore::ctor would have executed:

1. Alloc 0x1D810 B buffer (the MemoryTelemetrySampler footprint).
2. Call `MemoryTelemetrySampler::ctor` (RVA 0x0038b02c) with ecx=buffer,
   stack arg = player.
3. Store buffer at AvmCore+0x5F4 (captured from tryCapturePlayer's
   `diag_chain_step3_` — AvmCore is `step3` of our FRE→Player chain).
4. Inline `AvmCore::attachSampler`: +0x40 = sampler, +0x38 = 0, +0x3C = 1.
5. Set PlayerTelemetry+0x38 = 1 (the "sampler armed" flag).
6. Call `Player::populateNameMaps` (RVA 0x002151a9) — idempotent, creates
   method-name lookup tables at Player+0xDD8 / +0xDDC.
7. Propagate those maps to AvmCore+0xBC / +0xC0.
8. Call `AvmCore::registerSamplerMeta` (RVA 0x000a8754) — wires up rate
   counters at AvmCore+0xEC/F0/F4.

**Result**: the sampler *pump* — the per-frame function that drains queued
samples and emits `.sampler.sample` records (RVA ~0x38d6af) — starts firing
(CDB observed 1 hit in a short Mode B run, vs 0 hits before). But the
sample *queue* stays empty: zero samples are captured.

## Why the queue stays empty

After installing the sampler replay (best-effort in
`forceEnableTelemetry` — construct MTS/TS, attach to AvmCore+0x40/0x5F4,
propagate name maps, wire rate counters), CDB confirms:

- The sampler-pump function at RVA 0x38d6af **does fire** (0→1 hits per
  short run), proving the sampler subsystem now sees a live sampler
  object.
- The pump drains a queue at `[sampler+0x228]+0x204` — and that count
  is **always zero**, so the pump emits no `.sampler.sample` records.

So enqueue is the missing step. AVM2 sampling appears to be implemented
via code paths that the AS3 method dispatch takes **only when the
sampler was live at AvmCore construction time**. We verified:

- Setting AvmCore+0x3C = 1 (active byte) after the fact is not
  sufficient — dispatch still doesn't enqueue.
- Calling the complete post-sampler init sub-block (attach,
  populateNameMaps, registerSamplerMeta) is not sufficient either.
- The AS3 methods the test app exercises were JIT-compiled **before**
  Mode B enabled the sampler; they never enqueue.

Our working theory is that JIT-compiled AS3 methods bind to the
sampler's slot early (either inline-materialising its pointer or
capturing a "sampler absent" decision at compile time). AVM2 has no
re-JIT primitive exposed through the native interfaces we can reach
from an ANE, so once a method is compiled without sampler hooks it
stays that way for the lifetime of the process.

A definitive verification would need either (a) Adobe source access to
confirm the dispatch's exact sampler-check pattern, or (b) a test
scenario that loads an entirely new SWF *after* Mode B enables
telemetry — the new SWF's methods should pick up sampler hooks if the
JIT recompiles them, and would then emit samples even in Mode B.
Neither is in scope for the current profiler target.

## Recommended user-facing behaviour

**Default**: Mode B, zero idle overhead, no AS3 function samples.
Covers 95% of profiling needs (timeline, CPU, memory, DisplayList,
markers).

**Opt-in for AS3 function sampling**: document how to ship a
`.telemetry.cfg` at `%USERPROFILE%\.telemetry.cfg` (or
`C:\Users\<USER>\telemetry.cfg`) with `SamplerEnabled=true`. AIR reads it
at startup, AvmCore::ctor sees PlayerTelemetry, and the sampler wires up
correctly with full JIT hooks. This costs startup-time enablement (not
zero-overhead) but delivers the full AS3 view.

A hybrid API (e.g. `profilerEnableSamplingOnNextLaunch(true)`) could
write the cfg file and require a restart — acceptable trade-off when
the user explicitly wants function-level detail.

## File inventory

- `shared/profiler/include/AirTelemetryRvas.h` — RVAs and offsets for
  sampler replay (`kRvaMemoryTelemetrySamplerCtor`,
  `kRvaPlayerPopulateNameMaps`, `kRvaAvmCoreRegisterSamplerMeta`,
  AvmCore / Player / PlayerTelemetry sampler offsets).
- `WindowsNative/src/profiler/WindowsAirRuntime.cpp` — sampler install
  block at end of x86 `forceEnableTelemetry`. Guarded so failure is
  non-fatal (transport + telemetry + ptel trio still work).
- This document.
