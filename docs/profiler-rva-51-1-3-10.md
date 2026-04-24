# Profiler Hook RVAs for Adobe AIR 51.1.3.10 (Windows)

Static binary analysis of `Adobe AIR.dll` 51.1.3.10 x64 to identify the RVAs
of telemetry-related functions for an on-demand Scout-compatible profiler
hook. All RVAs are relative to image base `0x180000000` (i.e. add `0x180000000`
to obtain the ghidra-style absolute address).

The same document now also records the x64/x86 Windows RVAs used by the
`.aneprof` backend for factual AS3 typed reference edges. Those hooks are not
part of the Scout socket path; they feed `as3_reference_ex` and
`as3_reference_remove` events directly into `.aneprof`.

## Target binary

- Path: `C:\AIRSDKs\AIRSDK_51.1.3.10\runtimes\air-captive\win64\Adobe AIR\Versions\1.0\Adobe AIR.dll`
- Size: 19,977,944 bytes
- Image base: `0x180000000`
- SHA256: `e24a635554dba434d2cd08ab5b76d0453787a947d0f4a2291e8f0cae9459d6cc`

For cross-reference:
- 51.1.3.12 SHA256: `e28d153d1360009c443224376bb7ea68210c70b8f98c9bd0916ed912ac508ede`

## Method

1. Started from the 51.1.3.12 Ghidra analysis project already present at
   `C:\AIRSDKs\AIRSDK_51.1.3.12\binary-optimizer\`, which has:
   - `analysis-output/win-x64/all_functions_decompiled.c` (full decompile of the DLL)
   - `analysis-output/win-x64/string_xref_map.csv` (string → caller)
   - Ghidra already resolved C++ class/vtable names (e.g. `PlayerTelemetry::vftable`,
     `SocketTransport::vftable`, `telemetry::Telemetry::vftable`), so the
     class identities did not need to be inferred — they were annotated in
     the decompile output.

2. Identified the target functions in 51.1.3.12 by:
   - String xrefs (`TelemetryAddress`, `TelemetryPassword`, `.telemetry.cfg`,
     `.player.version`, `51,1,3,12`, `.tlm.category.enable`, `.mem.telemetry.overhead`)
   - vtable-references in the decompile (`*param_1 = PlayerTelemetry::vftable`,
     `*param_1 = SocketTransport::vftable`, etc.)
   - Direct reading of the initialization orchestrator `FUN_1801e9410`

3. Extracted 24–96 byte prologues of each target in 51.1.3.12, then searched
   for the **same byte sequence with RIP-relative 4-byte displacements masked
   as wildcards** in 51.1.3.10's `.text` section. Mask positions are computed
   from x86-64 opcode decoding (`48 8D /5`, `48 8B /5`, `48 89 /5`, `4C 8D /5`,
   `3A 05`, `E8 rel32`, `E9 rel32`, `FF 15`, `FF 25`, etc.).

4. All 24 targets resolved to a **unique match** in the .10 `.text` section
   — no ambiguity. The complete script is at `C:\tmp\air_rva_analysis\find_rvas_v2.py`.

5. **Cross-validation**: the `PlayerTelemetry::ctor` RVA in .10 was independently
   validated by checking that its body contains a `lea r?, [rip+disp32]` that
   targets the string `51,1,3,10` (at RVA `0xec02c0`). It does — at
   `0x48e4a0 + 0x1be` inside the constructor. 51.1.3.12 does not contain the
   substring `51.1.3.12` anywhere in the DLL — only `51.1.3.10` and `51,1,3,10`
   — confirming the DLL is the correct version. The ctor also references
   `.player.airversion` at the same call shape that FUN_18048e3e4 in .12 uses
   to pass `"51.1.3.12"`.

6. For `SocketTransport::vftable`, the vtable itself was located in .rdata
   at `0x180ecb7d0` (.10). Slot 1 (the `open/connect` method) of that vtable
   points at `0x1804930d0`, which byte-pattern matches `FUN_180492fc0`
   (.12 open/connect). This is a second independent validation of the
   SocketTransport family of RVAs.

## RVA table

All RVAs below are relative to `0x180000000` (add that to get the ghidra-style
full address). Confidence:

- **HIGH** — unique byte-pattern match at 24–64 B, plus a second validation
  (string xref / vtable slot / expected delta)
- **MED** — unique byte-pattern match only

| # | Function | Class / purpose | RVA .12 | RVA .10 | Size (.12) | Confidence | Validation |
|---|----------|-----------------|---------|---------|-----------:|-----------|------------|
| 1 | `TelemetryConfig::parse_kv` | reads each line of `.telemetry.cfg` and dispatches to set_kv | `0x48bc20` | **`0x48bd60`** | 613 B | HIGH | strings `TelemetryPassword`, `SamplerEnabled` still at this callsite |
| 2 | `TelemetryConfig::set_kv` | stores TelemetryAddress/TelemetryPassword/SamplerEnabled/Stage3DCapture/DisplayObjectCapture/CPUCapture/ScriptObjectAllocationTraces/AllGCAllocationTraces/GCAllocationTracesThreshold in config struct | `0x48a644` | **`0x48a784`** | 541 B | HIGH | vtable-free function body matches with RIP-masked 24 B |
| 3 | `TelemetryConfig::read_file` | reads `%USERPROFILE%\.telemetry.cfg` then `telemetry.cfg`; calls `ExpandEnvironmentStringsW`, builds path, invokes parser via `thunk_FUN_18048bc20` | `0x48c0a8` | **`0x48c1e8`** | 316 B | HIGH | bytes unique; string `.telemetry.cfg` still reachable from this RVA in .10 |
| 4 | `TelemetryConfig::has_address` | `return 0 < *(int*)(cfg+0x18)` — tests if an address was loaded from cfg | `0x48be88` | **`0x48bfc8`** | 8 B | HIGH | direct match; 8-byte function |
| 5 | `TelemetryConfig::parse_addr_port` | parses `"host:port"`, splits on `:`, uses `FUN_18030ecb8` for port atoi; rejects >2 colons and port ≥0x10000 | `0x48be90` | **`0x48bfd0`** | 177 B | HIGH | |
| 6 | `TelemetryConfig::parse_bool` | `"1"`/`"true"`/`"yes"`/`"on"` → 1 else 0 | `0x48bf44` | **`0x48c084`** | 122 B | HIGH | |
| 7 | `telemetry::Telemetry::ctor` | concrete `Telemetry` class ctor. Sets 6 `telemetry::Telemetry::vftable` pointers (multiple-inheritance from ITransportCallback, ITelemetryCommandHandler, IOutputBufferFlush, IInputBufferNotification, ITelemetryCmdResponse, base), installs `PlatformCriticalSection`, `VarHashTable` for cmd dispatch, and keeps the transport in field `+0xf8` | `0x485270` | **`0x4852c0`** | 386 B | HIGH | body contains `lea` to 7 distinct vtables in .rdata; vtable pattern intact |
| 8 | `telemetry::Telemetry::dtor_unwind` | destructor; unwinds vtables in reverse order | `0x4855b4` | **`0x485604`** | 379 B | HIGH | |
| 9 | `SocketTransport::ctor` | creates the TCP transport: allocates 0x298 B, installs `SocketTransport::vftable`, copies host string, heap-allocates a `PlatformSocketWrapper` subobject | `0x48ea54` | **`0x48eb10`** | 184 B | HIGH | vtable at `.10 0x180ecb7d0` |
| 10 | `SocketTransport::dtor` | destructor; releases PlatformSocketWrapper, frees host string | `0x48f05c` | **`0x48f118`** | 64 B | HIGH | |
| 11 | `SocketTransport::open/connect` | virtual "open" method (vtable slot 1 of SocketTransport vftable). Creates a fresh `PlatformSocketWrapper` via `FUN_18048e394`, calls `wrapper->connect(host, port, 1)` through vtable slot `0x38` of PlatformSocketWrapper | `0x492fc0` | **`0x4930d0`** | 222 B | HIGH | present in SocketTransport vtable slot 1 (0x8) at `0x180ecb7d8` |
| 12 | `SocketTransport::send_bytes` (real body) | the function that calls `wrapper->send(data, size)` via PlatformSocketWrapper vtable slot `0x10`. Caller is a vtable-thunk at 0x492f50 | `0x492f50`⁽ᵃ⁾ | **`0x493060`** | 106 B | HIGH | bytes + PlatformSocketWrapper vtable slot 11 refers to the .10 thunk at `0x492f50`, which jumps to `0x493060` |
| 13 | `SocketTransport::close_1` | virtual close (simpler variant) | `0x492eb8` | **`0x492fc8`** | 107 B | HIGH | |
| 14 | `SocketTransport::close_2` | virtual close (variant called on destructor path) | `0x492e50` | **`0x492f60`** | 102 B | HIGH | PlatformSocketWrapper vtable slot 10 → this |
| 15 | `PlatformSocketWrapper::ctor` | wraps `PlatformSocket` with SocketTransport-aware vtable. Size of subobject = 0x298 B. Stores (socket_transport*, parent*) at offsets 0x290, 0x288 | `0x48e394` | **`0x48e450`** | 80 B | HIGH | vtable @ `0x180ecb828` in .10 |
| 16 | `PlayerTelemetry::ctor` | the glue class that wires Telemetry to the runtime: pushes `.player.version`/`.player.airversion`/`.player.type`/`.player.instance`/`.player.debugger`/`.player.root`/`.platform.capabilities`/`.platform.cpucount` metadata; emits `.tlm.category.enable`/`.tlm.category.disable` for `sampler`, `displayobjects`, `alloctraces`, `allalloctraces` based on the cfg flags; calls `FUN_1806af0bc` to register command handlers for `.snapshot.get`, `.displayList.get`, `.player.gc` | `0x48e3e4` | **`0x48e4a0`** | 1568 B | HIGH | contains `lea` to `"51,1,3,10"` at `0x180ec02c0` |
| 17 | `PlayerTelemetry::dtor` | destructor | `0x48eec8` | **`0x48ef84`** | 403 B | HIGH | |
| 18 | `Player::init_telemetry` | the orchestrator: calls cfg parser, gates on `has_address`, allocates SocketTransport → Telemetry → PlayerTelemetry and stores them at `player+0x1650/0x1658/0x1660` | `0x1e9410` | **`0x1e96d0`** | 388 B | HIGH | single caller of `FUN_18048c0a8` (`.telemetry.cfg` reader) |
| 19 | `PlatformSocket::send` (low-level) | `send(SOCKET, buf, len, 0)` — the literal `send()` syscall via ws2_32 IAT | `0x5de5a0` | **`0x5de350`** | 14 B | HIGH | |
| 20 | `PlatformSocket::sendto_opt` | `setsockopt(TCP_NODELAY) + sendto(...)`; alt send path for datagram/debug | `0x5de5b0` | **`0x5de360`** | 135 B | HIGH | |
| 21 | `Player::handle_EnableTelemetry_child_SWF` | logs `"EnableTelemetry tag on child SWF was ignored"` | `0x35f820` | **`0x35f9c0`** | 388 B | HIGH | string xref |
| 22 | `MemoryTelemetrySampler::init_probe` | references `.mem.telemetry.overhead` | `0x493c2c` | **`0x493d3c`** | 1316 B | HIGH | string xref |
| 23 | `telemetry::TelemetrySampler::ctor` | base `TelemetrySampler` ctor; installs `telemetry::TelemetrySampler::vftable`, `PlatformCriticalSection`, `TelemetrySampler::SamplerTimerClient::vftable` at `+0x4d0` | `0x488094` | **`0x4881a4`** | 291 B | HIGH | |
| 24 | `MemoryTelemetrySampler::ctor` | extends `TelemetrySampler`; installs `avmplus::IMemorySampler::vftable` at object head, then overwrites with `MemoryTelemetrySampler::vftable` | `0x487f50` | **`0x488060`** | 324 B | HIGH | |

⁽ᵃ⁾ **Note about the send_bytes slot.** In both .12 and .10, the
`PlatformSocketWrapper::vftable` slot for the send_bytes method holds the RVA
of a 5-byte **MSVC adjustor thunk** (pattern `48 8b 89 80 02 00 00 e9 XX XX XX XX`
= `mov rcx, [rcx+0x280]; jmp +offset`). This thunk jumps to the real function.
Both RVAs are documented:
- `.10 thunk` @ `0x492f50` (14 B) — what the vtable references directly
- `.10 real body` @ `0x493060` (106 B) — what actually does the work and
  calls `PlatformSocket::send`

Hooking the thunk is fine for interception because every call to send_bytes
goes through it.

## AS3 typed edge hooks

These are real operation hooks used by `.aneprof` to label retainer edges
without relying on `IMemorySampler.addDependentObject` inference.

- x64 targets were mapped from the 51.1.3.12 Ghidra decompile to AIR
  `51.1.3.10` with the same masked prologue search method described above.
- x86 targets were pinned in AIR `51.1.3.10` by string xrefs/disassembly around
  the `child` and `listener` argument paths, then guarded by byte prologues at
  install time.
- `addEventListener` records only successful non-weak listeners. Weak listeners
  are intentionally skipped because they should not retain the listener.
- Event-listener identity includes event type and capture/bubble phase.
- Display-list reparenting emits a removal for the previously observed parent
  when a successful add hook moves the child.
- `removeChild`, `removeChildAt` and `removeEventListener` emit
  `as3_reference_remove`; the analyzer replays add/remove mutations before
  building the live graph.

| Operation | Edge kind | x64 RVA | x86 RVA | Event |
| --- | --- | ---: | ---: | --- |
| `DisplayObjectContainer.addChild` | `display_child` | `0x0050724c` | `0x003eccb2` | `as3_reference_ex` |
| `DisplayObjectContainer.addChildAt` | `display_child` | `0x005073e0` | `0x003ecdf0` | `as3_reference_ex` |
| `DisplayObjectContainer.removeChild` | `display_child` | `0x00507cac` | `0x003ed486` | `as3_reference_remove` |
| `DisplayObjectContainer.removeChildAt` | `display_child` | `0x00507d5c` | `0x003ed501` | `as3_reference_remove` |
| `EventDispatcher.addEventListener` | `event_listener` / `timer_callback` | `0x001fc6fc` | `0x0019d862` | `as3_reference_ex` |
| `EventDispatcher.removeEventListener` | `event_listener` / `timer_callback` | `0x001ff3e4` | `0x0019fe4e` | `as3_reference_remove` |

Timer callbacks are classified conservatively at hook time: if the dispatcher is
already sampled and its AS3 type name contains `Timer` or `SetIntervalTimer`,
the edge is stored as `timer_callback`; otherwise it remains `event_listener`.

## Init sequence (narrated)

The path from "Player is booting" → "first byte sent to Scout" is:

1. `Player::ctor` (at RVA `0x1bb0fc`-ish, not a direct target here) calls
   `Player::init_telemetry` (.10 RVA `0x1e96d0`).

2. `Player::init_telemetry`:
   1. Allocates a small on-stack `TelemetryConfig` struct and initialises
      its fields to defaults via `FUN_1804881b8` (address=NULL, port=0,
      sampler=off, all capture flags off).
   2. Calls `TelemetryConfig::read_file` (.10 `0x48c1e8`) with the
      `Player*` as context.
   3. `read_file` expands `%USERPROFILE%` with `ExpandEnvironmentStringsW`,
      then tries `%USERPROFILE%\.telemetry.cfg` first, then `telemetry.cfg`
      in the cwd. For each file it calls `FUN_18033c054` which reads the
      file line-by-line and invokes a callback equal to the thunk of
      `TelemetryConfig::parse_kv` (`.10 thunk_FUN_18048bd60`) per line.
   4. `parse_kv` compares each key to the 9 recognised strings
      (`TelemetryAddress`, `TelemetryPassword`, `SamplerEnabled`,
      `Stage3DCapture`, `DisplayObjectCapture`, `CPUCapture`,
      `ScriptObjectAllocationTraces`, `AllGCAllocationTraces`,
      `GCAllocationTracesThreshold`), duplicates the value via
      `FUN_18033e880`, and dispatches to `TelemetryConfig::set_kv`
      (.10 `0x48a784`). `set_kv` re-does a cascade of string compares and
      writes into the appropriate cfg struct field (see source notes
      below).
   5. After read_file returns, `init_telemetry` calls
      `TelemetryConfig::has_address` (.10 `0x48bfc8`). If the address
      length is **zero, telemetry is left disabled and the function
      returns** — the player runs without any socket.
   6. If the address is non-empty, `init_telemetry`:
      - `alloc 0x40 bytes` → `FUN_18048ea54` constructs a
        `SocketTransport` (.10 `0x48eb10`). The ctor stores the target
        host string at `transport+0x10`, sets port at `transport+0x18`,
        and heap-allocates a 0x298-byte `PlatformSocketWrapper` sub-object.
      - stores `transport` at `player + 0x1650`.
      - `alloc 0x110 bytes` → `FUN_180485270` constructs a concrete
        `Telemetry` (.10 `0x4852c0`) with the SocketTransport as its
        wrapped transport. The ctor installs 6 `telemetry::Telemetry::vftable`
        pointers (multi-inheritance from `ITelemetry`, `ITransportCallback`,
        `ITelemetryCommandHandler`, `IOutputBufferFlush`,
        `IInputBufferNotification`, `ITelemetryCmdResponse`), initialises a
        `PlatformCriticalSection`, wires up a `VarHashTable` for command
        dispatch, and **calls transport->method0(Telemetry*)** through a
        vtable (binds the Telemetry as the transport callback target).
      - stores `telemetry` at `player + 0x1658`.
      - `alloc 0x220 bytes` → `FUN_18048e3e4` constructs a
        `PlayerTelemetry` (.10 `0x48e4a0`) bound to
        `(player, telemetry, &cfg_struct)`. The ctor immediately emits,
        via calls through the `Telemetry::log*` virtual methods (vtable
        slot offsets 0x30 for string, 0x38 for number, 0x40 for int,
        0x48 for generic, 0x50 for flag):
        - `.player.version` = `"51,1,3,10"`
        - `.player.airversion` = `"51.1.3.10"`
        - `.player.type` = `"AirDebugger"` / `"AirLauncher"` / similar
        - `.player.debugger` = bool
        - `.player.global.date` = u64
        - `.player.instance` = uint (monotonic counter)
        - `.player.root` = uint (root SWF id, if any)
        - `.player.scriptplayerversion` = int (debugger only)
        - `.platform.capabilities` = string
        - `.platform.cpucount` = int
        - `.tlm.category.start` / `.tlm.category.enable` /
          `.tlm.category.disable` for `sampler`, `displayobjects`,
          `alloctraces`, `allalloctraces`, `sdktype` depending on cfg
        - registers command handlers for `.snapshot.get`,
          `.displayList.get`, `.player.gc` via `FUN_1806af0bc`.
      - stores `playertelemetry` at `player + 0x1660`.
      - finally wires `transport->field_0x38 = playertelemetry` so the
        transport knows its top-level recipient.

3. During the very first call to a Telemetry virtual method in
   `PlayerTelemetry::ctor` (e.g. the `.player.version` log), the Telemetry
   concrete class:
   - queues an AMF3-encoded event into its internal output buffer.
   - when the buffer fills or the call reaches `flush_boundary`, Telemetry
     calls `SocketTransport::open` (vtable slot 1 → .10 RVA `0x4930d0`)
     if not already open, which in turn calls `PlatformSocketWrapper::connect`
     (PlatformSocket vtable slot 7 → internal). This runs a Winsock
     `WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP)` then a blocking `connect()`.
   - `Telemetry` then calls `SocketTransport::send_bytes` (vtable slot 11
     of PlatformSocketWrapper, which is a thunk to .10 `0x493060`). The
     real body calls `PlatformSocket::send(sock, buf, len, 0)` at .10
     `0x5de350`.

So the **first bytes written to the socket are the AMF3-encoded
`.player.version` event**, exactly as stock Scout expects. This matches the
Scout wire format: Scout expects the server to immediately receive the
`.player.*` handshake events on connection.

## ITelemetry virtual table (PlayerTelemetry implementation)

PlayerTelemetry vtable RVA (.10): `0x180ec95e0` (validated via the first
`lea r?,[rip+X]` in `PlayerTelemetry::ctor`).

The classes have multiple inheritance. The base ITelemetry vtable has these
slots (offsets into vtable in bytes):

| offset | purpose | evidence |
|--------|---------|----------|
| 0x00 | `scalar deleting destructor` (`operator delete`) | first slot by MSVC ABI |
| 0x08 | adjustor thunk to other subobject | |
| 0x10 | (adjustor or `AddRef`) | |
| 0x18 | (adjustor or `Release`) | |
| 0x30 | `log_string(name, const char*)` | `PlayerTelemetry::ctor` calls `(*vtable[0x30])(ptr, ".player.version", "51,1,3,10")` |
| 0x38 | `log_int64_or_double(name, u64)` | `(*vtable[0x38])(ptr, ".player.global.date", u64)` |
| 0x40 | `log_int(name, int)` | `(*vtable[0x40])(ptr, ".player.scriptplayerversion", int)` |
| 0x48 | `log_uint_or_value(name, uint)` | `(*vtable[0x48])(ptr, ".player.instance", uint)` |
| 0x50 | `log_bool(name, u8)` | `(*vtable[0x50])(ptr, ".player.debugger", u8)` |

Because of multiple inheritance, the Telemetry concrete class also exposes
five other vtables at successive `+0x8` offsets from object head
(`ITransportCallback::vftable` at obj[0x10], `ITelemetryCommandHandler::vftable`
at obj[0x18], `IOutputBufferFlush::vftable` at obj[0x20],
`IInputBufferNotification::vftable` at obj[0x28],
`ITelemetryCmdResponse::vftable` at obj[0x30]). These are used internally
only and are not good hook targets.

### SocketTransport vtable (.10 `0x180ecb7d0`) — 20 slots observed

```
slot 0 (0x00) = 0x180817de0   # scalar deleting dtor
slot 1 (0x08) = 0x1804930d0   # open/connect  [HOOK CANDIDATE 1]
slot 2 (0x10) = 0x18048f840   # ???
slot 3 (0x18) = 0x1804a6090   # ???
slot 4 (0x20) = 0x180493a00   # probably recv callback
slot 5 (0x28) = 0x180492140   # ???
slot 6 (0x30) = 0x180492420   # ???
slot 7 (0x38) = 0x1804929e0   # ???
slot 8 (0x40) = 0x18048f720   # ???
slot 9 (0x48) = 0x1804931b0   # ???
```

### PlatformSocketWrapper vtable (.10 `0x180ecb828`) — 20 slots observed

```
slot 0 (0x00) = 0x180680bf0   # destructor
slot 1 (0x08) = 0x180022b50   # adjustor
slot 2 (0x10) = 0x1806ba8e0   # addref/release?
slot 3 (0x18) = 0x180ac15d0
slot 4 (0x20) = 0x180022b50   # adjustor
slot 5 (0x28) = 0x180022b50   # adjustor
slot 6 (0x30) = 0x18048f5d0
slot 7 (0x38) = 0x180680c60   # connect()  [used by SocketTransport::open]
slot 8 (0x40) = 0x180685650
slot 9 (0x48) = 0x1804921a0
slot10 (0x50) = 0x180492f60   # = SocketTransport::close_2 (adjustor thunk)
slot11 (0x58) = 0x180492f50   # = send_bytes THUNK → jmps to 0x493060  [HOOK CANDIDATE 2]
slot12 (0x60) = 0x180022b50
slot13 (0x68) = 0x180493040
slot14 (0x70) = 0x18043b210
slot15 (0x78) = 0x180401910
slot16 (0x80) = 0x180401920
slot17 (0x88) = 0x180681300
slot18 (0x90) = 0x180680b80
slot19 (0x98) = 0x18067fd60
```

## `AvmCore::setTelemetry` / `AvmCore::enableSampler` / `AvmCore::getTelemetry`

These three methods are **inline** in the Adobe source
(`AvmCore-inlines.h:62/67` and `AvmCore.cpp:5473`). Concretely:

- `void AvmCore::setTelemetry(ITelemetry*)` is `m_telemetry = p` — one
  write instruction at the call site.
- `ITelemetry* AvmCore::getTelemetry()` is `return m_telemetry` — one read.
- `void AvmCore::enableSampler(ISampler*)` is `m_sampler = p` at
  `AvmCore.cpp:5473`, a 4-line function, but in an optimised release build
  it is also likely inlined.

Because these are inlined, **there is no standalone function to hook by RVA.**
The observable write happens inside `Player::init_telemetry` (and possibly
elsewhere when a SWF drives the `.telemetry.cfg` reload). In the layout
observed in FUN_1801e9410 (.10 `0x1e96d0`), the offsets touched on the
`param_1` struct are `+0x1650 / +0x1658 / +0x1660`. These are **Player**
struct offsets, not AvmCore. In the decompile of other functions the
expression `*(*(core + 0xac0) + 0x1658)` is used (e.g. at .12 line 360132,
530363), where `core + 0xac0` is the `Player*`, and `player + 0x1658` is
the concrete `Telemetry*`. So the field commonly referred to as the
`AvmCore::m_telemetry` in Adobe's abstraction is actually stored on the
`Player`, and `AvmCore::getTelemetry()` likely performs
`return m_player->field_0x1658` (or dereferences a pointer chain). This
means the "AvmCore::setTelemetry point-of-install" in this runtime is
actually a write at `player + 0x1658` inside `Player::init_telemetry`.

## Recommended hook strategy

The project goal is: generate a Scout-compatible `.fls` (TCP stream bytes).
Two viable options:

### Option A (RECOMMENDED): hook `SocketTransport::send_bytes` at `0x493060`

**Pros**
- The hook sees exactly the bytes Adobe's Telemetry already produced
  (AMF3-encoded, length-prefixed), which is what any Scout-compatible
  server expects. Zero protocol re-implementation.
- Hook is fire-and-forget: copy the buffer to our own sink (file / pipe /
  ANE socket), then optionally either call the real function (letting
  real Scout receive the bytes too) or skip it (using it only as a tap).
- The ABI is stable: `void send_bytes(this, char* data, int size)` with
  MSVC calling convention (rcx, rdx, r8).
- The function is vtable-called, so a simpler alternative is to **overwrite
  slot 11 of PlatformSocketWrapper vtable at `0x180ecb828 + 0x58`**
  (`.rdata` write — requires VirtualProtect to RW first). This is less
  invasive than trampolining and survives code motion across builds.

**Cons**
- We see the bytes after the Telemetry class has buffered/framed them.
  If the Telemetry decides not to connect (e.g. cfg missing), there is no
  stream at all. So we also need a way to **force** telemetry on even
  when the user hasn't provided a `.telemetry.cfg`.

### Option B: hook `Player::init_telemetry` at `0x1e96d0`

**Pros**
- Single hook covers the full init sequence. We can override the config
  (forcing `TelemetryAddress=127.0.0.1:7934` regardless of user cfg),
  then let the real init run — the runtime will connect, set up
  `PlayerTelemetry`, and the rest of the hook in Option A becomes
  straightforward.
- Lets us enable the sampler unconditionally (`SamplerEnabled=1` cfg bit).

**Cons**
- Complex parameter — we have to synthesise a `TelemetryConfig` struct
  matching layout expected by `FUN_1804881b8` + cfg offsets.

### Combined recommendation

Do **both**:

1. Hook `Player::init_telemetry` @ `0x1e96d0` with a prolog detour
   (`jmp rel32` or `jmp [rip+disp]` that runs our code first to force
   a cfg).
2. Hook `SocketTransport::send_bytes` @ `0x493060` (or vtable slot
   `0x180ecb828+0x58`) to tap the outgoing byte stream.

The ANE then gets a complete Scout-format stream that is guaranteed to
be accepted by any Scout-compatible server or recorded to a `.fls` file.

## Open questions / things that need human decision

1. **Enabling telemetry without a cfg file.** We haven't yet identified
   whether `setTelemetry(null)` ever fires at runtime when cfg is missing
   — i.e. whether the init_telemetry function is even called in the
   "no cfg" path. It is called unconditionally from the Player ctor, but
   returns early at `has_address == 0`. So the Player always goes through
   `init_telemetry`, which is a good hook point for forcing a cfg before
   the `has_address` check.
2. **Sampler integration.** `AvmCore::enableSampler(ISampler*)` must be
   called with a `TelemetrySampler` (RVA `0x4881a4`) or
   `MemoryTelemetrySampler` (RVA `0x488060`) instance. The current
   decompile does not yet show where the Sampler is attached to AvmCore
   in init_telemetry — need a deeper pass through the callers of
   `TelemetrySampler::ctor` and `MemoryTelemetrySampler::ctor`. This is
   a follow-up.
3. **Whether the Scout wire format actually includes the AMF3 length
   prefix at the `send_bytes` layer.** The decompile shows
   `send(sock, buf, len)` is called directly on the assembled buffer —
   so whatever Telemetry hands to the transport is the literal TCP
   stream. But Scout expects a specific handshake. Verifying the stream
   by running it through a minimal Scout-compat parser is
   recommended before considering Option A production-ready.
4. **macOS / Android / iOS backports** of these RVAs are out of scope
   for this pass. The `.10` x64 Windows DLL is a single target.
5. **No dynamic validation performed.** Per instructions, analysis was
   static only. Before shipping the hook, run a captive debugger once
   to confirm offsets match the actual layout (especially the
   PlatformSocketWrapper vtable slot 11 assumption).

## Artifacts

- Script used for pattern matching: `C:\tmp\air_rva_analysis\find_rvas_v2.py`
- Script used for vtable extraction: `C:\tmp\air_rva_analysis\find_vtables.py`
- Raw vtable dump: `C:\Users\Joao\.claude\projects\...\tool-results\b1yytereh.txt`

## Quick-copy C/C++ constants for the ANE

```c
// 51.1.3.10 Windows x64 -- Adobe AIR.dll
// SHA256 e24a635554dba434d2cd08ab5b76d0453787a947d0f4a2291e8f0cae9459d6cc
#define AIR_51_1_3_10_IMAGE_BASE              0x180000000ULL

// Hook points
#define RVA_PLAYER_INIT_TELEMETRY             0x001e96d0  // Player::init_telemetry
#define RVA_SOCKET_TRANSPORT_CTOR             0x0048eb10  // SocketTransport::ctor
#define RVA_SOCKET_TRANSPORT_OPEN             0x004930d0  // SocketTransport::open/connect
#define RVA_SOCKET_TRANSPORT_SEND_BYTES       0x00493060  // ** real body of sendBytes (HOOK)
#define RVA_SOCKET_TRANSPORT_SEND_BYTES_THUNK 0x00492f50  //    adjustor thunk (vtable slot)
#define RVA_PLATFORMSOCKET_SEND               0x005de350  // raw send(SOCKET, buf, len, 0)
#define RVA_PLAYERTELEMETRY_CTOR              0x0048e4a0
#define RVA_TELEMETRY_CTOR                    0x004852c0  // concrete Telemetry::ctor
#define RVA_TELEMETRY_SAMPLER_CTOR            0x004881a4
#define RVA_MEMORY_TELEMETRY_SAMPLER_CTOR     0x00488060

// Config readers
#define RVA_TELEMETRY_CONFIG_READ_FILE        0x0048c1e8  // reads .telemetry.cfg
#define RVA_TELEMETRY_CONFIG_PARSE_KV         0x0048bd60
#define RVA_TELEMETRY_CONFIG_SET_KV           0x0048a784
#define RVA_TELEMETRY_CONFIG_HAS_ADDRESS      0x0048bfc8
#define RVA_TELEMETRY_CONFIG_PARSE_ADDR_PORT  0x0048bfd0
#define RVA_TELEMETRY_CONFIG_PARSE_BOOL       0x0048c084

// Vtables (in .rdata)
#define RVA_VT_SOCKET_TRANSPORT               0x00ecb7d0
#define RVA_VT_PLATFORM_SOCKET_WRAPPER        0x00ecb828
#define RVA_VT_PLAYER_TELEMETRY               0x00ec95e0

// The simplest hook: overwrite vtable slot 11 (0x58 bytes in) of
// PlatformSocketWrapper::vftable to point at our thunk.
// Address to patch: base + RVA_VT_PLATFORM_SOCKET_WRAPPER + 0x58
// (Use VirtualProtect PAGE_READWRITE first.)
```
