# Mode B on x86 — RESOLVED

**Status**: x86 Mode B is functional. Dynamic CDB investigation located the
correct `AvmCore → Player` offset (+0x5E8). All telemetry/transport/ptel
allocations, hook installation and capture controller paths from the x64
implementation carry over unchanged.

## The fix

`kFramePlayerChainStep4` on x86 is **+0x5E8** (not +0x04 as the static
scan suggested). The complete x86 chain from a live FRE frame:

```
frame  = <peek top of FRE framestack stored in TLS>
step1  = *(frame + 0x08)       // AbcEnv/MethodEnv
step2  = *(step1 + 0x14)       // MethodInfo
step3  = *(step2 + 0x04)       // "AvmCore-ish" container
Player = *(step3 + 0x5E8)      // Player pointer
```

Verified dynamically under CDB: at three independent
`TestProfilerApp.exe` launches (different ASLR), the offset +0x5E8
consistently produced the same value that `Player::init_telemetry`
received as `ecx`. Two adjacent back-references (+0x2BDC, +0x301C) also
point to Player, suggesting the AvmCore-like struct at `step3` embeds a
back-pointer chain for fast traversal by other runtime subsystems.

The structural correspondence to the x64 chain is:

| arch | step1 | step2 | step3 | step4  |
|------|-------|-------|-------|--------|
| x64  | 0x10  | 0x28  | 0x08  | 0xAC0  |
| x86  | 0x08  | 0x14  | 0x04  | 0x5E8  |

Offsets halve for pointer-sized fields, which matches the first three
steps. `step4` does not halve to `0x560` because roughly half the fields
between `step3` and the Player slot are `uint32_t` counters that are the
same size on both architectures.

## How the offset was discovered

Static analysis exhausted: six candidate offsets were tried, all failed
dynamic verification (vtable mismatch, non-null telemetry slots, etc.).

Dynamic approach that worked:

1. Set a breakpoint on `init_telemetry` (RVA `0x0018e840`) — it receives
   `ecx = Player` on entry.
2. Let the runtime hit the breakpoint once at app startup, save `@ecx`
   as `$t10` (the ground-truth Player).
3. Continue running until our ANE's `tryCapturePlayer` hits (RVA
   `0x135c0` in `AneAwesomeUtilsWindows.dll`, inside `profilerStart`
   FREFunction).
4. At that moment, walk the chain manually:
   - Read TLS-backed frame stack from `TEB+0xE10+idx*4` where `idx` is
     the DWORD at `AIR+0xe13464`.
   - Framestack+0x04 is the array, framestack+0x08 is the count.
   - Walk `array[count-1] → +0x08 → +0x14 → +0x04`.
5. Brute-force scan `step3[0..0x4000]` for `$t10`. One consistent
   offset: `0x5E8`.
6. Confirm with two more ASLR-randomised launches. Same offset every
   time.

The `fs:[0x2C]` read path is wrong on 32-bit Windows — that's the
*static*-TLS pointer. Dynamic TLS (`TlsAlloc`/`TlsGetValue`) lives at
`TEB+0xE10..TEB+0xF0C` for slots 0..63. Several hours were lost to this
detail; the fix validated once `TEB+0xE10+idx*4` was used.

## Implementation notes

- `WindowsAirRuntime::tryCapturePlayer` uses `TlsGetValue(idx)` which is
  the correct dynamic-TLS accessor, so the C++ code was already right
  — only the `kFramePlayerChainStep4` constant needed updating.
- `looks_like_player()` still validates that `*player` (the vtable)
  falls inside Adobe AIR.dll's image, which rules out random heap
  objects that happen to have readable memory at the Player-telemetry
  offsets.
- `forceEnableTelemetry` continues to work unchanged (the three
  allocation sizes / Player offsets / GCHeap lock offset / calling
  conventions from the original plan were already correct).

## Test results

Running `pwsh run_test.ps1 -Arch x86 -Rebuild`:

| Scenario | Frames | Records | BytesIn | BytesOut | Result |
|----------|--------|---------|---------|----------|--------|
| A        | 120    | 29      | 27 995  | 12 480   | OK     |
| B        | 80     | 20      | 19 517  | 8 958    | OK     |
| C        | 240    | 57      | 56 350  | 24 296   | OK     |
| D (kill) | killed | n/a     | 26 736  | –        | OK-partial (footer absent, inflate clean) |

All four scenarios PASS on x86 Mode B. Multi-capture (Scenario B after
A's stop) works. Kill-resilience (Scenario D) works.
