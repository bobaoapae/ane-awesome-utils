# Mode B on x86 — current status and what we tried

**TL;DR**: x86 Mode B is NOT YET SUPPORTED. All scaffolding is in place
(arch-switched RVAs, struct offsets, calling conventions, force-enable
body) but `tryCapturePlayer` on x86 has no path that reliably produces
a real `Player*`. The ANE refuses to start a capture on x86 — this is
intentional: no `.telemetry.cfg` fallback means zero idle overhead is
preserved. On x64 the profiler works perfectly.

## What works

- All x86 RVAs from `docs/profiler-mode-b-plan-x86.md` loaded via the
  `air_51_1_3_10_win_x86` namespace in `AirTelemetryRvas.h`.
- Prologue signature check (`55 8B EC 6A FF 68` — the SEH prolog MSVC
  emits on x86) guards `initialize()` against wrong runtime versions.
- `FRE::getActiveFrame_TLS` @ RVA `0x00458600` works (peeks the top of
  the TLS-backed frame stack; confirmed by cross-checking the
  exported FRE APIs, which all route through it). We bypass it
  entirely and read the same TLS slot ourselves (RVA `0x00e13464`)
  via the system `TlsGetValue`.
- `forceEnableTelemetry` body is fully written with x86-correct
  sizes (SocketTransport 0x20, Telemetry 0xB0, PlayerTelemetry
  0x1C8), Player offsets (+0xDB8 / +0xDBC / +0xDC0), GCHeap lock
  (+0x670) and the `__fastcall` ABI that `alloc_locked` uses on x86.

## What doesn't work: `Player_from_Frame`

Two candidates were tried, both wrong:

### Candidate 1: RVA `0x45fe80`, final offset `+0x294`

This is the function already catalogued in `profiler-mode-b-plan-x86.md`
(MED confidence). Byte-shape matches x64's `Player_from_Frame` (4-deref
chain, ending at a >`0x100` offset). Empirically, reading the three
telemetry slots off its return (`+0xDB8`, `+0xDBC`, `+0xDC0`)
produces garbage — three DIFFERENT non-zero pointers where they
should all be null pre-init. `0x45fe80` is used by exactly ONE vtable
slot in `.rdata` and has zero direct callers in `.text`, so there's
no cross-site evidence that it semantically produces Player.

### Candidate 2: chain `[+0x08][+0x14][+0x04][+0x04]`

Produced by a follow-up investigation (see
`docs/profiler-mode-b-plan-x86-player.md`). Claimed 24 call-sites in
`.text` use that exact tail followed by a call to a Player method.

Dynamic verification showed this is ALSO wrong: reading `*player`
(the vtable pointer) yields an address **outside Adobe AIR.dll's
`.rdata`** (`0x636b43dc` in one run — a completely different module).
A real Player has its vtable in the AIR.dll `.rdata` section
(`0x108c2000 .. 0x10dba000`).

The 24-site pattern match is likely a false positive: many AS3
runtime objects use the same `+0x08 / +0x14 / +0x04` navigation, but
the final `+0x04` returns something else (a PoolObject or MethodInfo
field, not Player).

## What still could work — requires dynamic debugging

Static analysis keeps finding byte-pattern matches that are not
semantically correct. We need one of these, all requiring a live CDB
session:

1. **Set a breakpoint on `Player::init_telemetry`** (RVA 0x0018e840).
   The runtime calls it once at app boot, with `ecx = Player`. Note
   the value, then compare against various offsets from AvmCore to
   find the real `AvmCore → Player` offset. Reuse in a static
   capture.

2. **Breakpoint the send() IAT** (RVA `0x008c2b08`). Set up a
   `.telemetry.cfg` that points at 127.0.0.1:7934 and observe the
   outgoing Scout bytes. At that moment, walk back up the call stack
   to find the Telemetry → SocketTransport → Player chain in flight;
   read-back the offsets.

3. **Live memory scan**: from the `diagSlotTransport` value we
   capture (which points to what the runtime set at the REAL Player's
   +0xDB8 — if any — during boot), memory-walk back until we find the
   object whose +0xDB8 slot contains this value. That object is
   Player.

All three are straightforward in a debugger; none is feasible from
static analysis alone (we don't have symbols, Adobe AIR's layout is
heavily optimised, and each false positive we get from matching byte
shapes takes an hour of dynamic verification to disprove).

## What the ANE currently does on x86

- `initialize()` succeeds (SHA / prologue match).
- `tryCapturePlayer()` runs the chain and stores its best guess.
- The vtable and slot-value diagnostics make it obvious at the AS3
  level that the capture is bogus (vtable outside AIR.dll range,
  telemetry slots already populated).
- `forceEnableTelemetry()` then refuses with `Error::AlreadyEnabled`.
- `profilerStart()` returns `false` and tears down the partially-set-up
  capture — no .flmc file is produced, no runtime patching happened.

This is the correct behaviour for the current design: we refuse to
start rather than silently fall back to a Mode A (`.telemetry.cfg`)
workflow that would defeat the zero-idle guarantee.

## How to unblock

Run the game/app once under CDB:

```
"C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe" ^
    -g -G ^
    -c "bu Adobe_AIR+0x18e840 \".printf \\\"Player=0x%p\\\\n\\\", @ecx; gc\"; g" ^
    <LoadingDebugAir.exe>
```

The breakpoint will hit once during Player init. Log `@ecx` — that's
the real Player. Then at the same session:

- `dd @ecx L200` — dump Player's header. Vtable is at +0 and should
  point into the `Adobe AIR.dll` module (check with `!address @$retreg`).
- `? @ecx - <avmcore>` — distance from AvmCore to Player. This is
  the offset we need for the chain.

Once the offset is known, update `kFramePlayerChainStep4` in
`AirTelemetryRvas.h` and x86 Mode B will start working.
