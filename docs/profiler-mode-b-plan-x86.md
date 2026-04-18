# Profiler Mode B — Adobe AIR.dll 51.1.3.10 Windows x86 (Win32) backport

Target: `Adobe AIR.dll` 51.1.3.10 Windows **x86 (Win32, PE32)**.

- Path: `C:\AIRSDKs\AIRSDK_51.1.3.10\runtimes\air-captive\win\Adobe AIR\Versions\1.0\Adobe AIR.dll`
- Size: 15,478,488 bytes
- SHA256: `812980fcc3dff6d25abdfacc17dfd96baf83ca77f7c872aa06c5a544ba441ce0`
- Machine: `0x14c` (x86)
- PE Magic: `0x10b` (PE32)
- ImageBase: **`0x10000000`**
- Section layout:
  - `.text`  RVA `0x1000`    size `0x8c09aa`  file `0x400`
  - `.rdata` RVA `0x8c2000`  size `0x4f7822`  file `0x8c0e00`
  - `.data`  RVA `0xdba000`  size `0x128174`  file `0xdb8800`
  - `.tls`   RVA `0xee3000`  size `0x9`
  - `.rsrc`  RVA `0xee4000`
  - `.reloc` RVA `0xf19000`

This document mirrors `docs/profiler-mode-b-plan.md` (x64) and resolves the x86
analogs of every symbol in `docs/profiler-rva-51-1-3-10.md` and the Mode B
plan. All offsets are written assuming the image base above — add `0x10000000`
to RVAs for the VA seen inside code/data.

Static analysis only. No runtime validation performed.

## 1. Method

Analysis path used (in order):

1. **Direct string-xref probing** on `.text` via Python+Capstone for:
   - All 9 `TelemetryConfig` key names (the runtime stores them in an
     initialised table at RVA `0xdd1710`, **not** via inline `push imm32`).
     That table has 9 consecutive pointers: `TelemetryAddress`,
     `TelemetryPassword`, `SamplerEnabled`, `Stage3DCapture`,
     `DisplayObjectCapture`, `CPUCapture`,
     `ScriptObjectAllocationTraces`, `AllGCAllocationTraces`,
     `GCAllocationTracesThreshold` (plus more following ones for other
     subsystems).
   - `".telemetry.cfg"`, `".player.version"`, `".player.airversion"`,
     `"51,1,3,10"`, `"51.1.3.10"`, `"yes"`, `"true"`, `"on"`, `"standby"`.
2. **Call-graph chasing** from entrypoints (`init_telemetry` → `default_init` →
   `read_file` → `parse_kv` → `set_kv` → `AirString::assign` /
   `parse_bool_pair` / `parse_addr_port`).
3. **WS2_32.send IAT xrefs** to identify the low-level Winsock wrappers and
   confirm the SocketTransport send path.
4. **Vtable dumping** (`SocketTransport::vftable`,
   `PlatformSocketWrapper::vftable`, `PlayerTelemetry::vftable`).

Ghidra headless was started in parallel on the same DLL
(`/c/Users/Joao/AppData/Local/Temp/air_x86_analysis/`) for later
cross-validation; auto-analysis succeeded (`HeadlessAnalyzer: Analysis
succeeded`). The ExportFunctions.java script was still running when this
document was written; it is not required to validate the findings below.

SHA256 of the DLL was verified before analysis began
(`812980fcc3dff6d25abdfacc17dfd96baf83ca77f7c872aa06c5a544ba441ce0`).

## 2. RVA table (51.1.3.10 Win32 x86)

All RVAs are relative to image base `0x10000000`. Confidence column:
- **HIGH** — disassembled prologue matches semantic signature, and at least one
  second signal validates the identification (call graph, string xref, vtable
  slot, or immediate constant `0x1EFE`/`0x400` that is unique to the function).
- **MED** — unique pattern match but only one line of evidence.
- **LOW** — heuristic candidate only; should be confirmed via Ghidra decompile
  before shipping.

| # | Function                                       | RVA x86 (this build) | Size approx | ABI        | Confidence | Validation |
|---|------------------------------------------------|---------------------:|------------:|------------|-----------|-----------|
| 1 | `Player::init_telemetry`                        | **`0x0018e840`**     | ~432 B      | `__thiscall` | HIGH | single caller of `TelemetryConfig::default_init` (0x38b2a7); calls `read_file`, `has_address`, `alloc_small(0x20)`, `SocketTransport::ctor`, then the full init body |
| 2 | `TelemetryConfig::default_init`                 | **`0x0038b2a7`**     | 46 B        | `__thiscall` | HIGH | unique function containing `mov dword [ecx+0x18], 0x1EFE` **and** `mov dword [ecx+8], 0x400`; zeroes cfg fields 0/4/0x0C/0x10/0x14/0x1C/0x20/0x24 |
| 3 | `TelemetryConfig::dtor`                         | **`0x0038b729`**     | ~98 B       | `__thiscall` | HIGH | SEH prolog; calls `AirString::clear` on cfg+0xC and cfg+0x1C (4× with unwind pairs) |
| 4 | `TelemetryConfig::has_address`                  | **`0x0038e2dd`**     | 8 B         | `__thiscall` | HIGH | `cmp [ecx+0x10], 0; setg al; ret` — mirrors x64 exactly |
| 5 | `TelemetryConfig::parse_addr_port`              | **`0x0038e2e5`**     | ~230 B      | `__cdecl`    | HIGH | splits on `':'` (0x3a); called from set_kv at TelemetryAddress branch |
| 6 | `TelemetryConfig::parse_bool`                   | **`0x0038e365`**     | ~102 B      | `__cdecl`    | HIGH | compares `"true"` (VA 0x10b04a5c), `"yes"` (VA 0x10b2feec), `"on"` (VA 0x10bcb5c4) |
| 7 | `TelemetryConfig::parse_bool_pair`              | **`0x0038e3c8`**     | ~62 B       | `__cdecl`    | HIGH | compares `"standby"` (VA 0x10c2afec) and tail-calls parse_bool (0x38e365) |
| 8 | `TelemetryConfig::read_file`                    | **`0x0038e474`**     | ~300 B      | `__thiscall` | HIGH | calls `ExpandEnvironmentStringsA`, references `.telemetry.cfg` (VA 0x10c2b010), calls file-line reader with parse_kv thunk at `0x38e780` |
| 9 | `TelemetryConfig::parse_kv` (real)              | **`0x0038e0e3`**     | ~490 B      | `__cdecl`    | HIGH | first xref site of TelemetryAddress (via table+0); dispatches all 9 keys via `strcmp` loop and calls set_kv (0x38ce1c) |
| 10 | `TelemetryConfig::parse_kv` thunk (callback)   | `0x0038e780`         | 22 B        | `__cdecl`    | HIGH | 4-line thunk: `push [esp+0x10]; mov ecx,[esp+8]; push [esp+0x10]; push [esp+0x10]; call parse_kv; ret` — used as per-line callback inside read_file |
| 11 | `TelemetryConfig::set_kv`                       | **`0x0038ce1c`**     | ~1016 B     | `__thiscall` | HIGH | called by parse_kv at each key-match; reads all 9 keys from table at `0xdd1710`, writes to cfg+0xC..0x24 via inlined strcmp |
| 12 | `AirString::assign`                             | **`0x0024ea9a`**     | ~150 B      | `__thiscall` | HIGH | called from set_kv for TelemetryAddress/TelemetryPassword branches; signature `(this, char* data, size_t len_or_-1)` |
| 13 | `AirString::clear`                              | **`0x0024e1ee`**     | ~40 B       | `__thiscall` | HIGH | called 4× from TelemetryConfig::dtor on AirString members; checks empty-sentinel `0x10c10b08` |
| 14 | `MMgc::alloc_small`                             | **`0x0014f323`**     | ~??? B      | `__cdecl`    | HIGH | called from Player::init_telemetry with `push 1; push 0x20; call`, then `push 1; push 0xb0; call`; signature `(size_t size, int flags)` |
| 15 | `MMgc::alloc_locked`                            | **`0x001573de`**     | ~??? B      | `__fastcall` (ecx=heap, edx=size) + 1 stack arg | HIGH | called from Player::init_telemetry with `mov edx, 0x1c8; push 1; mov ecx, esi; call`; `0x7F0` size-cap bucket logic confirmed |
| 16 | `SocketTransport::ctor`                         | **`0x00390501`**     | ~160 B      | `__thiscall` (this=ecx; self, player, host, port on stack) | HIGH | installs vftable `0x10c2b470` at this+0; stores port at this+0xC, wrapper at this+0x10, player at this+0x18; allocates 0x230-byte PlatformSocketWrapper |
| 17 | `Telemetry::ctor`                               | **`0x00388be7`**     | ~290 B      | `__thiscall` | HIGH | called by `Player::init_telemetry` after `alloc_small(0xB0)`; first 16 B of concrete body set fields before vftable install |
| 18 | `Telemetry::bindTransport` thunk                | **`0x00389f78`**     | 8 B         | `__cdecl`    | HIGH | `push 1; call 0x389b9e; ret` — same two-step thunk pattern x64 uses |
| 19 | `Telemetry::bindTransport` real body            | `0x00389b9e`         | ~316 B      | `__thiscall` | HIGH | called by thunk; uses `this+0x98` and vtable slot +8 (matches x64 semantics) |
| 20 | `PlayerTelemetry::ctor`                         | **`0x0038ffc9`**     | ~1400+ B    | `__thiscall` | HIGH | installs vftable `0x10c2a788` at this+0; copies cfg+0..+8 into ptel+0x20..+0x30 (struct layout compressed vs x64) |
| 21 | `SocketTransport::send_bytes` (real body)       | **`0x00393f80`**     | ~80 B       | `__thiscall` | HIGH | gets wrapper from this+0x10, pushes data/len onto stack, loads wrapper vtable slot 9 (+0x24), calls through it; then uses IAT stub `[0x108c2c44]` for MSVC slot-cfg guard and invokes the send. Signature: `(this, const char* data, int size)` |
| 22 | `SocketTransport::close_1` (body)               | `0x0039385c`         | ~76 B       | `__thiscall` | HIGH | target of close_1 vtable adjustor-thunk `0x393830`; does not carry data args |
| 23 | `SocketTransport::close_2` (body)               | `0x003938a8`         | ~52 B       | `__thiscall` | HIGH | alternative close variant called by the close-with-flag dispatcher `0x393840` |
| 24 | `PlatformSocketWrapper::ctor`                   | **`0x0038ff87`**     | ~62 B       | `__thiscall` | HIGH | installs vftable `0x10c2b49c` at this+0; stores parent at this+0x220 and extra at this+0x224 |
| 25 | `FRE::Player_from_Frame` (`(frame+8)[+0x14][+4]+0x294`) | **`0x0045fe80`** | 16 B        | `__thiscall` | HIGH (by shape) / MED (by semantics) | unique 4-deref chain with last offset `0x294` (>=0x100); matches the x64 chain `(+0x10)[+0x28][+8]+0xac0` with every step halved except the final Core→Player offset (Core has mixed members so scaling isn't exactly /2). Zero direct callers found — function pointer is stored in a class vtable at RVA `0xbccac0` (one of the FRE-adjacent classes). |
| 26 | `FRE::getActiveFrame_TLS`                       | **not resolvable statically — see §5** | — | — | LOW | In x64 this was 8 bytes: `mov rax, gs:[slot]; ret`. In x86 the DLL does **not** use the direct `fs:[0x2c]+TLS_idx*4` pattern as a standalone exported function; all 54 such accessors found are static-init counters for locale objects. The AS3 frame-stack accessor appears to have been inlined by the MSVC optimiser in this build, or is called via `kernel32!TlsGetValue` (IAT `0x108c260c` — 227 callsites). Resolution requires decompiler-level analysis (deferred to Ghidra). |
| 27 | PlatformSocketWrapper::vftable slot for send_bytes | **not present in x86** | — | — | HIGH (negative finding) | In x64 it was PlatformSocketWrapper::vftable slot 11 (+0x58), a 5-byte MSVC adjustor thunk. In x86 the same slot structure exists but slot 11 (+0x2c) in PlatformSocketWrapper::vftable is the `close_1` thunk, not send_bytes. The x86 build routes `send_bytes` through **SocketTransport::vftable slot 4 (+0x10) = 0x393f80** directly — no wrapper-side thunk. **Hook point for Mode B on x86 is therefore different from x64** — patch `SocketTransport::vftable+0x10` rather than `PlatformSocketWrapper::vftable+0x58`. See §6. |
| 28 | `SocketTransport::vftable`                      | RVA `0x00c2b470`     | 20 slots × 4 B | — | HIGH | installed by SocketTransport::ctor at this+0 |
| 29 | `PlatformSocketWrapper::vftable`                | RVA `0x00c2b49c`     | 20 slots × 4 B | — | HIGH | installed by PlatformSocketWrapper::ctor at this+0 |
| 30 | `PlayerTelemetry::vftable`                      | RVA `0x00c2a788`     | ≥12 slots × 4 B | — | HIGH | installed at PlayerTelemetry ctor line 1 (`mov [ebx], 0x10c2a788`) |
| 31 | ws2_32!send IAT slot                            | VA `0x108c2b08` (RVA `0x8c2b08`) | 4 B | — | HIGH | ordinal 19 (= `send`); 4 CALL sites in `.text` |
| 32 | Telemetry keys lookup table                     | VA `0x10dd1710` (RVA `0xdd1710`) | 36 B (9×4) | — | HIGH | 9-entry array of char* pointing at each parsed key name |

### Where `set_kv` looked like it was at `0x38c277` but wasn't

The MSVC linker placed a large FRE-integrated helper function at
`0x38c277..0x38d5d3` (3420 B) with an `__SEH_prolog4(0x14, …)` prologue. It
is **not** `TelemetryConfig::set_kv` — it is a FRE-thread dispatcher that
itself happens to compare the same 9 strings (inlined). The real
`TelemetryConfig::set_kv` starts immediately after this function at
`0x38ce1c` with a clean `__thiscall` prologue (`push ecx; push esi; push edi;
mov edi, [esp+0x10]; mov esi, ecx`). No `int3` padding separates the two
functions — MSVC skipped alignment because the preceding function ends with
`ret 8` at `0x38ce1b`.

## 3. TelemetryConfig struct layout (x86, 40 bytes)

Derived from the writes in `TelemetryConfig::default_init` (0x38b2a7), the
offset tested in `has_address` (`cmp [ecx+0x10], 0`), and the field layout
consumed by `PlayerTelemetry::ctor` (copies cfg+0..+8 byte-by-byte into
ptel+0x20..+0x30):

```c
// x86: AirString is 0x0C bytes (3 × int32), TelemetryConfig is 0x28 bytes.
struct AirString {                // size 0x0C
    char*    data;                // +0x00
    int32_t  len;                 // +0x04    <-- has_address checks cfg+0x10 (= host.len)
    int32_t  cap;                 // +0x08
};

struct TelemetryConfig {          // size 0x28 (aligned to 4)
    uint8_t  Stage3DCapture;                 // +0x00
    uint8_t  Stage3DCapture_standby;         // +0x01
    uint8_t  DisplayObjectCapture;           // +0x02
    uint8_t  SamplerEnabled;                 // +0x03
    uint8_t  SamplerEnabled_standby;         // +0x04
    uint8_t  CPUCapture;                     // +0x05
    uint8_t  ScriptObjectAllocationTraces;   // +0x06
    uint8_t  AllGCAllocationTraces;          // +0x07
    uint32_t GCAllocationTracesThreshold;    // +0x08  default 0x400 (1024)
    AirString host;                          // +0x0C  host.data @ +0x0C, host.len @ +0x10, host.cap @ +0x14
    uint32_t port;                           // +0x18  default 0x1EFE (7934)
    AirString password;                      // +0x1C  password.data @ +0x1C, password.len @ +0x20, password.cap @ +0x24
};
static_assert(sizeof(TelemetryConfig) == 0x28);
```

Field writes in `default_init` (exact order, each with `eax=0`):

```
mov [ecx+0x0C], eax   ; host.data = NULL
mov [ecx+0x10], eax   ; host.len = 0
mov [ecx+0x14], eax   ; host.cap = 0
mov [ecx+0x1C], eax   ; password.data = NULL
mov [ecx+0x20], eax   ; password.len = 0
mov [ecx+0x24], eax   ; password.cap = 0
mov [ecx+0x00], eax   ; bool block 0 (8 flags cleared as 2× 4-byte writes)
mov [ecx+0x04], eax   ; bool block 1
mov dword [ecx+0x18], 0x1EFE   ; port = 7934
mov dword [ecx+0x08], 0x400    ; threshold = 1024
```

There is **no padding** — every field is either `uint8_t`, `uint32_t` or an
AirString of 3 × 4-byte fields, all aligned on 4-byte boundaries.

PlayerTelemetry::ctor copy map (bytes copied from cfg to the PlayerTelemetry
object):

```
ptel+0x20 <- *(u8*)(cfg+0x00)   (Stage3DCapture)
ptel+0x22 <- *(u8*)(cfg+0x02)   (DisplayObjectCapture)
ptel+0x24 <- *(u8*)(cfg+0x03)   (SamplerEnabled)
ptel+0x27 <- *(u8*)(cfg+0x05)   (CPUCapture)
ptel+0x28 <- *(u8*)(cfg+0x06)   (ScriptObjectAllocationTraces)
ptel+0x2b <- *(u8*)(cfg+0x07)   (AllGCAllocationTraces)
ptel+0x30 <- *(u32*)(cfg+0x08)  (GCAllocationTracesThreshold)
```

The `_standby` flags (cfg+0x01 and cfg+0x04) are not copied into
PlayerTelemetry — same as in x64. The host/port fields are consumed by
`SocketTransport::ctor` directly before `PlayerTelemetry::ctor` is called.

## 4. Player struct offsets

From the disassembly of `Player::init_telemetry` (`0x18e840`):

| Field | x64 offset | x86 offset | Notes |
|-------|-----------:|-----------:|-------|
| `SocketTransport*`            | `+0x1650` | **`+0xDB8`** | `mov [edi + 0xDB8], eax` just after `alloc_small(0x20)` |
| `Telemetry*`                  | `+0x1658` | **`+0xDBC`** | `mov [edi + 0xDBC], eax` just after `alloc_small(0xB0)` and `Telemetry::ctor` |
| `PlayerTelemetry*`            | `+0x1660` | **`+0xDC0`** | `mov [edi + 0xDC0], eax` just after `PlayerTelemetry::ctor` |
| `SocketTransport->field_for_pt` | `+0x38` | **`+0x1C`** | `mov [eax + 0x1C], ecx` where eax = transport, ecx = PlayerTelemetry* |

Allocator sizes (passed to `alloc_small` / `alloc_locked`):

| Object                  | x64 size | x86 size | Notes |
|-------------------------|---------:|---------:|-------|
| `SocketTransport`       | `0x40`   | **`0x20`** | all 8-byte fields halved to 4-byte |
| `Telemetry`             | `0x110`  | **`0xB0`** | vtable-heavy; many pointers |
| `PlayerTelemetry`       | `0x220`  | **`0x1C8`** | size passed to `alloc_locked`; mostly pointers |
| `PlatformSocketWrapper` | `0x298`  | **`0x230`** | pushed explicitly inside `SocketTransport::ctor` |

The GCHeap lock CAS loop in `init_telemetry` uses `[ebx + 0]` where
`ebx = GCHeap + 0x670`, i.e. the per-heap spin-lock is at offset **`+0x670`**
from the GCHeap singleton pointer in x86 (x64 was `+0xB98`). The GCHeap
singleton is loaded from `[0x10e08570]` (a global pointer in .data).

After acquiring the lock, the runtime writes:

```
mov [ebx + 4], edx           ; last-alloc ptr
mov dword [ebx + 8], 0x1C8   ; last-alloc size
xchg [ebx], eax              ; release the spinlock (eax=0)
```

So the "bookkeeping" offsets that x64 was at `+0xba0`/`+0xba8` from GCHeap
are `+0x674`/`+0x678` in x86 (i.e. `+4`/`+8` from the lock itself).

## 5. PlatformSocketWrapper::vftable dump (x86)

```
slot  0 (+0x00): 0x10515160   # destructor
slot  1 (+0x04): 0x1001eff0   # ret (adjustor)
slot  2 (+0x08): 0x1007cba0   # xor eax,eax; ret
slot  3 (+0x0C): 0x10766130
slot  4 (+0x10): 0x1001eff0   # ret (adjustor)
slot  5 (+0x14): 0x1001f650   # ret 8
slot  6 (+0x18): 0x10390ec0   # push esi; mov esi,ecx; call 0x515053
slot  7 (+0x1C): 0x10518ca0
slot  8 (+0x20): 0x10518d60
slot  9 (+0x24): 0x10392e90   # send path (see SocketTransport::send_bytes body)
slot 10 (+0x28): 0x10393840   # close dispatcher (close_1 or close_2)
slot 11 (+0x2C): 0x10393830   # adjustor thunk to close_1 (0x39385c)
slot 12 (+0x30): 0x1001f650   # ret 8
slot 13 (+0x34): 0x103938e0   # close dispatcher variant
slot 14 (+0x38): 0x10320130   # trivial getter: mov eax,[ecx+0x48]; ret
slot 15 (+0x3C): 0x103e94f0
slot 16 (+0x40): 0x10278880   # lea eax,[ecx+0x58]; ret
slot 17 (+0x44): 0x10515590
slot 18 (+0x48): 0x10515110
slot 19 (+0x4C): 0x105145d0
```

`SocketTransport::vftable` dump:

```
slot  0 (+0x00): 0x103d44d0   # mov [ecx+0x14],eax; ret 4
slot  1 (+0x04): 0x10393940   # open/connect
slot  2 (+0x08): 0x10391090   # SEH-wrapped entry
slot  3 (+0x0C): 0x10393820   # mov al,[ecx+4]; ret
slot  4 (+0x10): 0x10393f80   # <-- SEND_BYTES (see §2 row 21) [HOOK CANDIDATE]
slot  5 (+0x14): 0x10392e50   # flush-style (calls [0x108c2c44] IAT guard + wrapper vtable slot 9)
slot  6 (+0x18): 0x10393070
slot  7 (+0x1C): 0x103934b0
slot  8 (+0x20): 0x10390ff0
slot  9 (+0x24): 0x103939e0
slot 10 (+0x28): 0x10d4f9a0   # raw data (not code)
slot 11 (+0x2C): 0x10515160   # destructor
slot 12 (+0x30): 0x1001eff0
slot 13 (+0x34): 0x1007cba0
slot 14 (+0x38): 0x10766130
slot 15 (+0x3C): 0x1001eff0
slot 16 (+0x40): 0x1001f650
slot 17 (+0x44): 0x10390ec0
slot 18 (+0x48): 0x10518ca0
slot 19 (+0x4C): 0x10518d60
```

### TLS frame accessor — open question

The x86 DLL has **54 occurrences** of the x86 TLS prologue pattern
`mov eax, fs:[0x2c]; mov ecx, [tls_idx_var]; mov eax, [eax+ecx*4]`. **All 54
are static-init-once counters** for locale/category C++11 magic-statics
(they compare against a counter, set up an RTTI block, and take an `esi`
parameter). None of them is the AS3-frame-stack accessor.

This strongly suggests that in x86 (but not x64) MSVC **inlined** the
`FRE::getActiveFrame_TLS` helper into every call site, or the runtime switched
to `kernel32!TlsGetValue` (IAT `0x108c260c`, 227 callers). Without Ghidra
decompile output it is not trivial to pick the exactly-right pattern from 227
candidates.

Mitigation: Mode B on x86 should obtain `Player*` via the FREContext-to-Player
path (§6.2) rather than the TLS helper chain. That path uses
`FRE::Player_from_Frame` at **`0x45fe80`** and needs only a valid FREContext
(which the ANE always has when a FREFunction is being dispatched).

## 6. Calling conventions — summary

| Function                                  | Convention | Notes |
|-------------------------------------------|-----------|-------|
| `Player::init_telemetry`                  | `__thiscall` | ecx = Player*; 0 stack args |
| `TelemetryConfig::default_init`           | `__thiscall` | ecx = cfg*; `ret` (no stack cleanup) — writes done then returns eax = ecx |
| `TelemetryConfig::dtor`                   | `__thiscall` | ecx = cfg*; MSVC-managed `ret` |
| `TelemetryConfig::has_address`            | `__thiscall` | ecx = cfg*; no stack args; `setg al; ret` |
| `TelemetryConfig::parse_addr_port`        | `__cdecl`    | 3 stack args: `(const char* value, AirString* host, uint32_t* port)` — caller `add esp,0xC` |
| `TelemetryConfig::parse_bool`             | `__cdecl`    | 1 stack arg: `(const char* value)` — returns bool in al |
| `TelemetryConfig::parse_bool_pair`        | `__cdecl`    | 3 stack args: `(const char* value, u8* a, u8* b)` — `ret` cleanup by caller (uses `esp+0xC`, `esp+0x10`) |
| `TelemetryConfig::read_file`              | `__thiscall` | ecx = Player*? (arg `[esp+0x18]` also observed); does SEH; pushes `%USERPROFILE%\.telemetry.cfg` then `telemetry.cfg` |
| `TelemetryConfig::parse_kv` (real)        | `__cdecl`    | 3 stack args: `(const char* key_line, …)`; body wrapped in SEH |
| `TelemetryConfig::parse_kv` thunk         | `__cdecl`    | pass-through of 3 stack args |
| `TelemetryConfig::set_kv`                 | `__thiscall` | ecx = cfg*; 2 stack args: `(const char* key, const char* value)` — callees clean (`ret 8` at end) |
| `AirString::assign`                       | `__thiscall` | ecx = AirString*; 2 stack args: `(const char* str, size_t len_or_-1)` |
| `AirString::clear`                        | `__thiscall` | ecx = AirString* |
| `MMgc::alloc_small`                       | `__cdecl`    | 2 stack args: `(size_t size, int flags)` — caller `add esp,8` |
| `MMgc::alloc_locked`                      | `__fastcall` | ecx = GCHeap*, edx = size, stack = flags |
| `SocketTransport::ctor`                   | `__thiscall` | ecx = self, 3 stack args: `(Player*, const char* host, uint32_t port)` |
| `Telemetry::ctor`                         | `__thiscall` | ecx = self, 1 stack arg: `(SocketTransport* transport)` |
| `Telemetry::bindTransport`                | thunk uses `__cdecl` (single `push 1` + call) to reach the real body (`__thiscall`) |
| `PlayerTelemetry::ctor`                   | `__thiscall` | ecx = self, 3 stack args: `(Player*, Telemetry*, TelemetryConfig*)` |
| `SocketTransport::send_bytes`             | `__thiscall` | ecx = self, 2 stack args: `(char* data, int size)`; ends with `ret 8` |
| `FRE::Player_from_Frame`                  | `__thiscall` | ecx = frame; `ret` |

## 7. Recommended hook strategy for x86

The x86 build differs from x64 in one important way: **the send_bytes call
does NOT flow through the PlatformSocketWrapper vtable**; it is called
directly from Telemetry through `SocketTransport::vftable` slot 4 (+0x10).

### Option A (recommended) — hook via SocketTransport::vftable slot 4

Patch `*(void**)(IMAGE_BASE + 0x00c2b470 + 0x10) = &my_send_bytes`.
- Single slot, one VirtualProtect of 4 bytes.
- Hook receives `(this=SocketTransport*, char* data, int size)` — identical
  ABI to the original.
- No adjustor thunks or multi-inheritance offset fiddling needed.

### Option B — trampoline-hook on `0x00393f80` (the function body)

For parity with the x64 hook that goes through
`PlatformSocketWrapper::vftable+0x58`, an inline detour on the function
entry at `0x00393f80` also works. The first 5 bytes are `51 a1 08 a0 db 10`
(`push ecx; mov eax, [0x10dba008]`) — the `push ecx` byte is a clean start
for a 5-byte near jump (`E9 rel32`), and re-emitting `push ecx` before the
trampoline tail is trivial. This is a full 5-byte prologue replacement, not
a vtable swap; requires careful `VirtualProtect` + per-thread stop of
execution while patching.

Option A is preferred — no code patching, only `.rdata` write. Matches the
ane-awesome-utils Mode B pattern already established on x64.

## 8. Enable-telemetry sequence on x86 (analogue of Option A in the x64 plan)

```
;; all RVAs on 51.1.3.10 Win32 x86 — prefix with IMAGE_BASE (0x10000000) when used.
;; ABI noted after each call.

cfg = <stack 0x28 bytes>
(ecx=cfg)  call 0x0038b2a7        ; default_init() — sets port=7934, threshold=1024
(ecx=&cfg.host)                    ;
 push len; push host_cstr;         ;
  call 0x0024ea9a                  ; AirString::assign(&cfg.host, host, len)
cfg.port = user_port
cfg.SamplerEnabled = s
cfg.Stage3DCapture = s3d
... (direct byte writes OK; no standby flag needed for runtime)

;; SocketTransport
push 1; push 0x20
 call 0x0014f323                  ; alloc_small(0x20, 1) -> eax = transport
add  esp, 8
(ecx=0)   ; clear first
push cfg.port
push cfg.host.data
push player
(ecx=eax) call 0x00390501         ; SocketTransport::ctor(this, player, host, port)
mov  [player + 0xDB8], eax        ; Player::socket_transport

;; Telemetry
push 1; push 0xB0
 call 0x0014f323                  ; alloc_small(0xB0, 1) -> eax = telemetry
add  esp, 8
push [player + 0xDB8]
(ecx=eax) call 0x00388be7         ; Telemetry::ctor(this, transport)
mov  [player + 0xDBC], eax
(ecx=eax) call 0x00389f78         ; Telemetry::bindTransport(this)

;; PlayerTelemetry (locked alloc)
mov  esi, [0x10e08570]            ; GCHeap singleton
mov  edx, 0x1C8                   ; size
push 1                             ; flags
mov  ecx, esi
spinlock: lock cmpxchg [esi+0x670], <1>   ; acquire per-heap spin-lock
          test eax, eax; jne yield; ...
call 0x001573de                   ; alloc_locked(heap, 0x1C8, 1) -> eax = ptel
mov  [esi+0x674], eax             ; last-alloc ptr  (optional bookkeeping)
mov  dword [esi+0x678], 0x1C8     ; last-alloc size (optional)
xchg [esi+0x670], 0               ; release

push &cfg
push [player + 0xDBC]             ; telemetry
push player
(ecx=eax) call 0x0038ffc9         ; PlayerTelemetry::ctor(this, player, telemetry, cfg)
mov  [player + 0xDC0], eax
mov  [[player + 0xDB8] + 0x1C], eax  ; transport->player_telemetry = ptel

(ecx=&cfg) call 0x0038b729        ; TelemetryConfig::dtor — frees strings
```

## 9. Risks specific to x86

1. **Inlined `FRE::getActiveFrame_TLS`**: the x86 MSVC build inlined the
   TLS-frame-stack accessor. We cannot point the ANE at a stable RVA for
   it. Workaround: use our own FREContext to reach the Internal Frame
   (via the equivalent of `FREContext_to_Internal`, an analog of x64
   `0x58d3f8`). The path is then
   `Internal Frame → Player_from_Frame (0x45fe80) → Player*`. Requires
   Ghidra decompile to pin down `FREContext_to_Internal` at HIGH
   confidence; current static analysis lets us only point at the
   vtable at `0xbccac0` (row 25 of §2 / row 27 of the table).

2. **No send_bytes thunk in PlatformSocketWrapper::vftable**. The x86
   layout routes `send_bytes` differently from x64. Any cross-build hook
   code must branch on platform: x64 hooks PlatformSocketWrapper
   vtable slot 11 (+0x58); x86 hooks SocketTransport vtable slot 4 (+0x10).
   Hook-table constants must be duplicated per arch.

3. **MSVC function boundary without `int3` padding**. `set_kv` starts
   at `0x38ce1c` immediately after the `ret 8` at `0x38ce1b`, with no
   alignment padding. Any tooling that identifies function boundaries
   by `CC` padding (e.g. our capstone heuristic) will mis-locate this
   function; rely on the `__thiscall` prologue signature instead.

4. **GCHeap lock / CAS offset is `+0x670` in x86** (vs `+0xB98` in x64).
   Hard-coded constants from the x64 Mode B plan must be re-emitted.

5. **SEH frame linkage**: x86 uses the legacy `fs:[0]` SEH chain on every
   init_telemetry / read_file / set_kv / dtor call. If our hook diverts
   execution while the original's SEH state is partially set up, an
   unrelated exception can walk into our hook's stack frame. Make all
   manual invocations from a FREFunction body where SEH is already
   installed; don't call from a DLL thread-proc.

6. **PlatformSocket::send IAT path**. `ws2_32!send` is IAT slot at VA
   `0x108c2b08` — stable. But 4 callsites exist (vs 1 in x64). Tapping
   the IAT is overkill; stick to the vtable hook.

7. **`alloc_locked` is `__fastcall`** on x86 (not `__cdecl`), with
   `ecx = GCHeap`, `edx = size`, `[esp+4] = flags`. Caller pushes only
   1 stack arg. The ANE's signature cast must reflect this.

8. **`Player::init_telemetry` early-exit is at `has_address == 0`** — same
   as x64. Boot-time idle path is still zero cost. No change to Mode B
   idle semantics.

## 10. Viability verdict

**Mode B is viable on x86.** All the functional pieces are present, with
the same semantics as x64:

- Configuration struct layout is simpler (40 B vs 56 B). No padding quirks.
- All allocator and constructor functions are identified with HIGH confidence.
- SocketTransport::vftable slot 4 provides a cleaner hook point than the
  x64 PlatformSocketWrapper::vftable slot 11 (one indirection instead of
  two).
- `Player::init_telemetry` orchestrator exists unchanged.
- `AirString::assign` works with `len=-1` (the first branch of the
  function at `0x24eaae` handles `cmp ecx, -1 / jne skip_strlen` → same
  contract as x64 and the shared x64 Mode B doc assumption).

**Open items before shipping**:

- Pin down `FRE::getActiveFrame_TLS` exact RVA (LOW-confidence in §2
  row 26). Use the in-progress Ghidra headless project to dump the
  call graph and find the TLS accessor by its single use inside
  `FRE::FREContext_to_Internal`.
- Alternatively, avoid `getActiveFrame_TLS` entirely: obtain
  `Player*` via the FREContext-to-Internal path (row 27 analogue
  below). This is what x64 Mode B already prefers as a fallback.
- Confirm `FREContext_to_Internal` in x86 (row 27 in §2 of the x64
  doc was `0x58d3f8`). A caller-side scan would be: find
  `AirString`-free tiny functions that read from a global hashmap
  with lock and call the output of `Player_from_Frame` (`0x45fe80`)
  on the internal handle.

## 11. Quick-copy constants for the ANE (x86)

```c
// 51.1.3.10 Windows x86 — Adobe AIR.dll (Win32, PE32)
// SHA256  812980fcc3dff6d25abdfacc17dfd96baf83ca77f7c872aa06c5a544ba441ce0
#define AIR_51_1_3_10_X86_IMAGE_BASE            0x10000000U

// Hook points
#define X86_RVA_PLAYER_INIT_TELEMETRY           0x0018e840  // Player::init_telemetry
#define X86_RVA_SOCKET_TRANSPORT_CTOR           0x00390501
#define X86_RVA_SOCKET_TRANSPORT_SEND_BYTES     0x00393f80  // hook candidate (SocketTransport::vftable slot 4)
#define X86_RVA_SOCKET_TRANSPORT_CLOSE_1_BODY   0x0039385c
#define X86_RVA_SOCKET_TRANSPORT_CLOSE_2_BODY   0x003938a8
#define X86_RVA_TELEMETRY_CTOR                  0x00388be7
#define X86_RVA_TELEMETRY_BINDTRANSPORT_THUNK   0x00389f78
#define X86_RVA_PLAYERTELEMETRY_CTOR            0x0038ffc9
#define X86_RVA_PLATFORMSOCKETWRAPPER_CTOR      0x0038ff87

// Config / helpers
#define X86_RVA_TELEMETRY_CONFIG_DEFAULT_INIT   0x0038b2a7
#define X86_RVA_TELEMETRY_CONFIG_DTOR           0x0038b729
#define X86_RVA_TELEMETRY_CONFIG_HAS_ADDRESS    0x0038e2dd
#define X86_RVA_TELEMETRY_CONFIG_PARSE_ADDRPORT 0x0038e2e5
#define X86_RVA_TELEMETRY_CONFIG_PARSE_BOOL     0x0038e365
#define X86_RVA_TELEMETRY_CONFIG_PARSE_BOOLPAIR 0x0038e3c8
#define X86_RVA_TELEMETRY_CONFIG_READ_FILE      0x0038e474
#define X86_RVA_TELEMETRY_CONFIG_PARSE_KV       0x0038e0e3
#define X86_RVA_TELEMETRY_CONFIG_SET_KV         0x0038ce1c

#define X86_RVA_AIRSTRING_ASSIGN                0x0024ea9a
#define X86_RVA_AIRSTRING_CLEAR                 0x0024e1ee

#define X86_RVA_MMGC_ALLOC_SMALL                0x0014f323
#define X86_RVA_MMGC_ALLOC_LOCKED               0x001573de  // __fastcall

// FRE helpers
#define X86_RVA_FRE_PLAYER_FROM_FRAME           0x0045fe80  // (frame+8)[+0x14][+4]+0x294

// Vtables (in .rdata)
#define X86_RVA_VT_SOCKET_TRANSPORT             0x00c2b470  // slot 4 (+0x10) = send_bytes
#define X86_RVA_VT_PLATFORM_SOCKET_WRAPPER      0x00c2b49c  // slot 11 (+0x2C) = close_1 thunk (NOT send_bytes on x86)
#define X86_RVA_VT_PLAYER_TELEMETRY             0x00c2a788

// Player struct offsets
#define X86_PLAYER_OFFSET_SOCKET_TRANSPORT      0x00000DB8
#define X86_PLAYER_OFFSET_TELEMETRY             0x00000DBC
#define X86_PLAYER_OFFSET_PLAYER_TELEMETRY      0x00000DC0
#define X86_SOCKET_TRANSPORT_OFFSET_PLAYER_TEL  0x0000001C

// GCHeap singleton + lock
#define X86_GCHEAP_SINGLETON_VA                 0x10E08570  // dword pointer in .data
#define X86_GCHEAP_OFFSET_SPINLOCK              0x00000670
#define X86_GCHEAP_OFFSET_LAST_ALLOC_PTR        0x00000674
#define X86_GCHEAP_OFFSET_LAST_ALLOC_SIZE       0x00000678

// ws2_32!send IAT slot
#define X86_IAT_WS2_32_SEND                     0x108C2B08

// Telemetry keys lookup table (array of 9 char*)
#define X86_RVA_TELEMETRY_KEYS_TABLE            0x00DD1710
```

## 12. Artefacts

- Probe script (Python + Capstone): `C:\tmp\air_x86_analysis\find_rvas_x86.py`
- Phase scripts: `C:\tmp\air_x86_analysis\phase1*.py`, `phase2.py`
- Ghidra headless project (for later decompile runs):
  `C:\Users\Joao\AppData\Local\Temp\air_x86_analysis\AirWinX86_10.rep`

## 13. Open items / follow-ups

1. Ghidra is still running `ExportFunctions.java` on the x86 project. Once
   it completes, cross-validate every HIGH row of §2 against the exported
   function hashes. Zero mismatches expected; anything off is a capstone
   heuristic misread (most likely for row 26 — `getActiveFrame_TLS`).
2. Pin down `FRE::FREContext_to_Internal` in x86 (row 27 in the x64 doc,
   RVA `0x58d3f8`). Use the Ghidra class-browser output to find a FRE
   helper that takes an `FREContext` opaque handle and returns an
   `Internal` pointer.
3. Implement and validate `enableTelemetryNow(player, host, port, …)` for
   x86 in the ANE (mirror the x64 `Option A` body). The constants in §11
   are drop-in; the only expected diff is the __fastcall cast for
   `alloc_locked` and the 4-byte `SocketTransport::vftable+0x10`
   write (vs the 8-byte `PlatformSocketWrapper::vftable+0x58` write on
   x64).
