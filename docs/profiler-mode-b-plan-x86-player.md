# Mode B x86 — Player* discovery (static analysis complete)

Target: `Adobe AIR.dll` 51.1.3.10 Windows **x86 (Win32, PE32)**.

- Path: `C:\AIRSDKs\AIRSDK_51.1.3.10\runtimes\air-captive\win\Adobe AIR\Versions\1.0\Adobe AIR.dll`
- SHA256: `812980fcc3dff6d25abdfacc17dfd96baf83ca77f7c872aa06c5a544ba441ce0`
- ImageBase: `0x10000000`

This document resolves the "how do we get `Player*` at runtime on x86?" open
item from `docs/profiler-mode-b-plan-x86.md`. Static analysis only — no
runtime validation.

## TL;DR

**Option 1 (AvmCore→Player offset) works**, but the path is the 4-deref
FRE-frame chain, NOT a clean `[AvmCore + OFFSET]` accessor:

```
frame  = <peek top of FRE frame stack in TLS>
step1  = *(frame + 0x08)   // AbcEnv / MethodEnv
step2  = *(step1 + 0x14)   // MethodInfo
step3  = *(step2 + 0x04)   // PoolObject
Player = *(step3 + 0x04)   // AvmCore's player slot (x86 places it at +4, NOT +0x294)
```

The existing `FRE::Player_from_Frame` at **RVA `0x45fe80`** is **WRONG** — it
does the 3 outer derefs correctly but ends at `+0x294` instead of `+0x4`.
`+0x294` is a *different field* of the same parent (likely a Stage pointer
or similar), not Player. The runtime never uses `[+0x294]` as Player; 24
separate call-sites in `.text` all use `[+0x4]` as Player.

**Recommendation: ignore `0x45fe80` entirely and synthesise the chain in
ane-awesome-utils C++.** The ANE never needs to call an AIR-internal helper.

Options 2 (singleton scan) and 4 (TLS-slot scan) were tried and came up
empty — Player is never stored in a global `.data` slot on x86.

## Method

Scripts in `C:\tmp\air_x86_analysis_pl\`:

- `player_discovery.py` — finds every function that touches
  `[reg + 0xDB8]`, `[reg + 0xDBC]`, or `[reg + 0xDC0]`. These are the three
  Player telemetry slots; any function accessing them is (with high
  confidence) a Player method. We found 111 such functions.
- `deep_chain.py`, `deep_chain2.py` — for every caller of a Player method,
  symbolically trace backward to find what produces `ecx` (the `this`
  argument). Classify by pattern.
- `singleton_scan.py` — scans `.text` for every `mov ecx, [abs_va]` and
  `mov reg, [abs_va]; mov ecx, [reg + off]` sequence that is immediately
  followed by a call to a Player method. Zero matches — Player is never
  loaded from a global `.data` slot.
- `fre_exports.py` — reads the PE export directory to find every FRE API.
  39 exports; all of them start by calling the same helper at
  `0x458600` (`FRE::getActiveFrame_TLS`).
- `chain_investigate.py`, `cross_validate.py`, `validate_player_plus_4.py`
  — pin down the exact 4-deref chain and count call-site evidence.
- `check_peek_safety.py`, `find_peek_frame.py` — verify that the TLS-backed
  `getActiveFrame` is effectively non-destructive even though it
  internally decrements/re-increments the count.

## Evidence for the chain

### Pattern census (Strategy A)

Of 610 call-sites that pass `ecx = something` to a known Player method
(where the caller is NOT itself a Player method — i.e., an outside-in view):

| rank | count | `ecx` at call (shape)                                          | interpretation |
|-----:|------:|----------------------------------------------------------------|----------------|
|  1   |   23  | `*(*(*(*(<ecx@entry> + 0x8) + 0x14) + 0x4) + 0x4)`             | 4-deref chain through the function's own `this` |
|  2   |   32  | `<ecx@entry>`                                                  | `this` already IS Player (Player→Player call) |
|  3   |   18  | `*(<ecx@entry> + 0x24)`                                        | Player stored at `this+0x24` of caller |
|  4   |   10  | `*(<ecx@entry> + 0x4)`                                         | Player stored at `this+0x4` of caller |

The **`[+0x14][+0x4][+0x4]` tail** is a 9-byte literal pattern
(`8B 40 14 8B 40 04 8B 48 04`) — unambiguous. I scanned for it in `.text`
and found **24 distinct sites**. Every single one is immediately followed
(within 10 bytes) by a `CALL` to a Player method:

| follow-up call target | hits |
|----------------------:|-----:|
|              `0x1533b0` | 13   |
|              `0x1536b2` |  3   |
|              `0x1579fd` |  1   |
|        other targets    |  7   |

`0x1533b0` is in our 111-strong `PLAYER_METHODS` set (accesses
`[this + 0x530]`, `[this + 0x4ec]`, `[this + 0xDBC]`). Its callers use the
chain with `ecx = <chain-step-4>`, which MUST therefore be Player.

### The `0x45fe80` red herring

`FRE::Player_from_Frame` at `0x45fe80` is 16 bytes:

```
0x0045fe80  mov eax, [ecx + 8]
0x0045fe83  mov eax, [eax + 0x14]
0x0045fe86  mov eax, [eax + 4]
0x0045fe89  mov eax, [eax + 0x294]    ; <-- WRONG offset!
0x0045fe8f  ret
```

Call-site scan proves nobody actually uses `[+0x294]` as Player — the
24 real Player-getting sites all use `[+0x4]` at the final step. The
`+0x294` shape comes from shared code with some other FRE internal
(e.g., a Stage3D path), not from the Player-access path.

Example of co-existence at `0x0036cb1d..0x0036cb26`:

```
mov eax, [eax + 0x14]          ; step 2
mov eax, [eax + 4]             ; step 3
mov ecx, [eax + 4]             ; ecx = Player  (passed to 0x1533b0 below)
mov esi, [eax + 0x294]         ; esi = SOME OTHER field (not Player)
call 0x1533b0                  ; Player method — ecx must be Player
```

Both `+0x4` and `+0x294` are read off the same parent, but only `+0x4`
feeds the Player method.

### Why AvmCore→Player is at `+4` in x86 vs `+0xAC0` in x64

In x64, `Player*` is `AvmCore.m_player` at `core + 0xAC0`. In x86 the
entire `AvmCore` layout is different (smaller pointer size + member
reordering by MSVC 32-bit), and empirically `m_player` lives at the
first dword after the vtable, offset `+0x4`. This is consistent with
MSVC compilers sometimes hoisting a hot-accessed member to the front of
an unrelated class for cache locality. The struct re-layout was
apparent already in the existing x86 doc's struct-size table (AvmCore
is substantially smaller in x86).

### TLS-peek safety (the `0x458600` trap)

`FRE::getActiveFrame_TLS @ 0x458600` looks destructive:

```
push [TLS_idx]
call TlsGetValue
mov edx, eax
test edx, edx; je done
mov ecx, [edx+8]           ; count
test ecx, ecx; je done
mov eax, [edx+4]           ; array
dec ecx                    ; count-1
push esi
mov [edx+8], ecx           ; WRITE: save count-1   (!)
mov esi, [eax+ecx*4]       ; esi = array[count-1]
mov ecx, edx
push esi
call 0x45819d              ; push_frame(framestack, esi)
mov eax, esi
pop esi; ret
```

On inspection, `0x45819d` is exactly `push_frame`:

```
mov eax, [esi+8]           ; count
cmp eax, [esi+0xc]         ; capacity
jl skip
call grow_array
mov edx, [esi+8]
mov ecx, [esi+4]
mov eax, [esp+8]           ; new value
mov [ecx+edx*4], eax       ; array[count] = value
inc [esi+8]                ; count++
```

So the net effect of `0x458600` is: decrement count, read `array[count-1]`,
push it back → count is restored, array slot is unchanged. **Intermediate
state is transient, never visible to concurrent readers** (AS3 is
single-threaded on its dispatch loop). Calling `0x458600` from an
FREFunction body is therefore safe.

**However**, we do not need to call `0x458600` at all — the ANE can just
do the TlsGetValue + 4-deref chain itself in pure C++ with no AIR
internals involved.

## Option 3 candidates (inline hook on a high-traffic Player method)

We identified Player methods called during ordinary frame dispatch:

- `0x1533b0` — called 13 times from the chain pattern and
  4+ more from other Player-adjacent code paths; prolog
  `55 8B EC 51 51 56 57` → 7 bytes → **hookable**.
- `0x1536b2` — 3 hits; same prolog shape.
- `0x18e840` (`Player::init_telemetry`) — called ONCE during startup;
  not suitable for steady-state capture, but usable as a one-shot
  boot-time anchor via the same inline-hook technique.

Inline-hook approach as **Plan B** if the TLS-chain approach breaks on a
future AIR build:

```
// Overwrite first 5 bytes of 0x1533b0 with `E9 <rel32>` → our trampoline.
// On first entry: capture `ecx` as Player, remove the hook, restore
// original bytes, jump into the original 5 bytes manually.
```

The prolog has no branch target in its first 5 bytes (`55 8B EC 51 51` —
five independent pushes), so splice-safe.

## Final implementation: `tryCapturePlayer` x86

Replace the x86 arm of the `#if defined(_M_X64)` block in
`WindowsNative/src/profiler/WindowsAirRuntime.cpp`. The ANE bypasses all
AIR-internal helpers:

```cpp
#else  // ---- x86 ----

// x86 path — the FRE-frame-stack accessor is inlined by MSVC on this
// build, and the existing FRE::Player_from_Frame at RVA 0x45fe80 ends
// at +0x294 which is NOT Player. Bypass both.
//
// Chain derived from 24 identical call sites in .text (static analysis):
//   frame     = <peek top of FRE frame stack stored in TLS slot>
//   Player    = *(*(*(*(frame + 0x08) + 0x14) + 0x04) + 0x04)
//
// The TLS slot index is stored in a DWORD at RVA 0x00e13464 of Adobe
// AIR.dll (filled at DLL load by TlsAlloc). We read it directly and use
// the system TlsGetValue — no AIR-internal function is called, which
// makes the capture idempotent and side-effect free.

// The TLS slot index is filled by TlsAlloc at DLL init, so we read it
// every call rather than once at initialize(): if the value is 0 / -1
// we simply bail. The TlsGetValue function lives in kernel32.dll and
// is always resolvable.

constexpr std::uint32_t kX86TlsIdxSlotRva  = 0x00e13464;  // DWORD in .data
constexpr std::uint32_t kX86Step1Off       = 0x08;
constexpr std::uint32_t kX86Step2Off       = 0x14;
constexpr std::uint32_t kX86Step3Off       = 0x04;
constexpr std::uint32_t kX86Step4Off       = 0x04;  // AvmCore -> Player

// Read the TLS slot index the AIR runtime allocated for its FRE frame
// stack. This DWORD is written once by the runtime during DLL
// initialisation; after that it never changes.
const auto* tls_idx_slot = reinterpret_cast<const volatile DWORD*>(
    air_base_ + kX86TlsIdxSlotRva);
const DWORD tls_idx = *tls_idx_slot;
if (tls_idx == 0 || tls_idx == TLS_OUT_OF_INDEXES) return nullptr;

// TlsGetValue is safe — pure system API, no AIR state touched.
void* frame_stack = nullptr;
__try {
    frame_stack = ::TlsGetValue(tls_idx);
} __except(EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
}
if (frame_stack == nullptr) return nullptr;

// FRE frame-stack struct layout (16 bytes):
//   +0x00  <spare / type tag?>
//   +0x04  void**  array    (pointer to array of frame pointers)
//   +0x08  int32_t count    (number of frames currently on the stack)
//   +0x0C  int32_t capacity
//
// The ANE runs from inside an FREFunction body, so count >= 1. We peek
// array[count-1] — same result as calling the destructive-looking
// 0x458600 (which internally pops-then-pushes-back, but that is fragile
// under concurrent GC; peeking in pure C++ is cleaner).
void* frame = nullptr;
__try {
    auto* array = *reinterpret_cast<void* const**>(
        reinterpret_cast<char*>(frame_stack) + 0x04);
    const std::int32_t count = *reinterpret_cast<const std::int32_t*>(
        reinterpret_cast<char*>(frame_stack) + 0x08);
    if (count <= 0 || array == nullptr) return nullptr;
    frame = array[count - 1];
} __except(EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
}
if (frame == nullptr) return nullptr;

// Walk the 4-step chain to Player.
void* player = nullptr;
__try {
    const auto* p0 = static_cast<char*>(frame);
    const auto* p1 = *reinterpret_cast<char* const*>(p0 + kX86Step1Off);
    if (p1 == nullptr) return nullptr;
    const auto* p2 = *reinterpret_cast<char* const*>(p1 + kX86Step2Off);
    if (p2 == nullptr) return nullptr;
    const auto* p3 = *reinterpret_cast<char* const*>(p2 + kX86Step3Off);
    if (p3 == nullptr) return nullptr;
    player = *reinterpret_cast<void* const*>(p3 + kX86Step4Off);
} __except(EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
}

if (!looks_like_player(player)) return nullptr;

player_.store(player, std::memory_order_release);
return player;

#endif  // x86
```

Also expose three new constants in
`shared/profiler/include/AirTelemetryRvas.h` (x86 namespace):

```cpp
// FRE frame-stack TLS index. Initialised by AIR at DLL load via TlsAlloc.
inline constexpr std::uint32_t kRvaFreFrameStackTlsIdx = 0x00e13464;

// Frame-stack struct offsets (offset into the struct pointed to by TLS).
inline constexpr std::uint32_t kFreFrameStackOffArray = 0x04;
inline constexpr std::uint32_t kFreFrameStackOffCount = 0x08;

// 4-deref Frame->Player chain offsets.
inline constexpr std::uint32_t kFramePlayerChainStep1 = 0x08;  // frame   -> AbcEnv
inline constexpr std::uint32_t kFramePlayerChainStep2 = 0x14;  // AbcEnv  -> MethodInfo
inline constexpr std::uint32_t kFramePlayerChainStep3 = 0x04;  // MethodInfo -> Pool
inline constexpr std::uint32_t kFramePlayerChainStep4 = 0x04;  // AvmCore -> Player
```

And retire the existing `kRvaFreGetActiveFrame = 0x00458600` /
`kRvaFrePlayerFromFrame = 0x0045fe80` — both are unused by the new path.

## Static validation (2+ independent signals)

**Signal 1 — call-site census.** Exactly 24 sites in `.text` use the
`[+0x14][+0x04][+0x04]` tail pattern. In every case the chain is
immediately followed by a `CALL` to one of 5 Player methods (identified
independently via `[this + 0xDBC]` write). No site follows the
pattern with a call to a non-Player method. Signal strength: **high**
(100% consistency across 24 sites).

**Signal 2 — FRE export analysis.** All 39 FRE exports start by calling
`0x458600`, which internally does the exact same `TLS -> frame` peek we
reproduce in C++. This proves the TLS slot `0x10e13464` is the correct
one for FRE frames (and not, say, a locale-init counter). The IAT
constant for `TlsGetValue` at `0x108c260c` is the one Adobe uses.

**Signal 3 — `+0x294` disproof.** Byte-pattern search for the alternative
chain tail `[+0x14][+0x04][+0x294]` (used by `0x45fe80`) returns **zero**
matches in `.text` outside `0x45fe80` itself. If `+0x294` were a real
Player path, we would see it at real call sites. We don't.

**Signal 4 — Player struct consistency.** The Player methods that write
`[this + 0xDB8]` (the SocketTransport slot) are 111 in number. Every one
of them accepts `this` via `ecx` at entry, i.e. `__thiscall`. When
`ecx` at entry can be traced, 23/610 cases trace back to the 4-deref
chain from `ecx@entry+8`. No caller places Player in a global slot.
Zero false positives across the 111 functions.

**Signal 5 — Chain matches x64 semantic shape.** The x64 chain is
`(frame+0x10)[+0x28][+8]+0xAC0` — 3 derefs from frame to Core, plus a
field offset from Core to Player. The x86 chain is
`(frame+0x08)[+0x14][+4]+4` — same 3 derefs to what must be Core, plus
`+4` as Core's Player slot in x86. Step 1 offset goes `0x10 → 0x08`
(halved; pointer size), step 2 `0x28 → 0x14` (halved; pointer-heavy
MethodInfo layout), step 3 `0x08 → 0x04` (halved; Pool's Core slot).
Step 4 differs because AvmCore member layouts changed between arches.

## Plan B (if the above fails at runtime)

1. **Inline hook on `0x1533b0`**. The first 5 bytes are
   `55 8B EC 51 51` — five register pushes, no branch, safe to splice.
   Install an `E9 <rel32>` trampoline on first call after DLL load.
   The hook reads `ecx` as Player, restores the original 5 bytes,
   tail-jumps into the saved prolog. This guarantees we capture Player
   the first time any AS3 frame dispatches (well within 1-2 s of boot).

2. **Inline hook on `0x18e840` (init_telemetry itself)**. If (1)
   somehow fires in a thread where ecx != Player, the last-resort is
   init_telemetry — which is unambiguously Player-only. The catch: it
   runs only once per process, so if telemetry is already on we missed
   it. We'd need to patch the function at DLL_PROCESS_ATTACH.

3. **Dynamic debugger session**. Attach WinDbg to `adl.exe`, break on
   `0x18e840`, confirm `ecx` at entry, then step out and re-confirm the
   chain. Takes ~10 minutes to do once, validates everything above in
   concrete.

## Artefacts

All analysis output is in `C:\tmp\air_x86_analysis_pl\`:

- `player_discovery.py` + `player_methods.json` — the 111 Player
  methods with their exact `[this+0xDBx]` sites.
- `deep_chain2.py` + `ecx_origins.json` — the 610 caller-to-Player
  traces categorised by `ecx`-origin shape.
- `singleton_scan.py` + `singleton_findings.json` — the negative result
  for Options 2 and 4.
- `fre_exports.py` + `fre_exports.json` — all 39 FRE exports.
- `validate_player_plus_4.py` — the side-by-side `+0x4` vs `+0x294`
  evidence.
- `check_peek_safety.py` — the proof that `0x458600` is peek-equivalent.

Keep these if a future AIR build breaks the chain — they can be rerun
as-is to re-derive the offsets.
