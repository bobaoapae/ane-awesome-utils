# Profiler Mode B — enable telemetry on demand without `.telemetry.cfg`

Target: `Adobe AIR.dll` 51.1.3.10 Windows x64.
SHA256: `e24a635554dba434d2cd08ab5b76d0453787a947d0f4a2291e8f0cae9459d6cc`
Image base: `0x180000000`.

This doc extends `docs/profiler-rva-51-1-3-10.md` and `docs/PROFILER_IMPLEMENTATION_PLAN.md`.
It answers the 4 questions raised by the plan for Mode B:

1. How do we reach `Player*` from inside the ANE?
2. What is the `TelemetryConfig` struct layout used by `Player::init_telemetry`?
3. Is it safe to run the init sequence after boot with a synthetic config?
4. Is a binary patch bypass simpler than manual invocation?

Only static analysis was performed; no runtime validation yet. All RVAs were
resolved by wildcard byte-pattern matching of 24--128 B prologues from the
51.1.3.12 Ghidra project against the 51.1.3.10 `.text` section. Every target
resolved to a unique match in .10 (see `C:\tmp\air_rva_analysis\find_rvas_v3.py`).

## 1. Newly resolved RVAs (51.1.3.10 x64)

| # | Function | RVA .12 | RVA .10 | Role |
|---|----------|--------:|--------:|------|
| 25 | `FRE::getActiveFrame_TLS` (`FUN_18058d6b0`) | `0x58d6b0` | **`0x58d390`** | Returns current FRE frame from TLS stack |
| 26 | `FRE::Player_from_Frame` (`FUN_1802d6da8`) | `0x2d6da8` | **`0x2d71e8`** | `(frame+0x10)[+0x28][+8]+0xac0` = `Player*` |
| 27 | `FRE::FREContext_to_Internal` (`FUN_18058d718`) | `0x58d718` | **`0x58d3f8`** | Given our FREContext, finds the internal frame |
| 28 | `TelemetryConfig::default_init` (`FUN_1804881b8`) | `0x4881b8` | **`0x4882c8`** | Zero + set defaults (port=7934, threshold=1024) |
| 29 | `TelemetryConfig::dtor` (`FUN_1804886f4`) | `0x4886f4` | **`0x488804`** | Frees host and password strings |
| 30 | `TelemetryConfig::parse_bool_pair` (`FUN_18048bfc0`) | `0x48bfc0` | **`0x48c100`** | Sets two bool bytes (handles `"standby"`) |
| 31 | `AirString::assign` (`FUN_18030e930`) | `0x30e930` | **`0x30ec10`** | Copies a C-string into an `AirString` |
| 32 | `AirString::clear` (`FUN_18030dd9c`) | `0x30dd9c` | **`0x30e07c`** | Frees an `AirString` |
| 33 | `MMgc::alloc_small` (`FUN_1801a087c`) | `0x1a087c` | **`0x1a0a64`** | The allocator used for SocketTransport + Telemetry |
| 34 | `MMgc::alloc_locked` (`FUN_1801aaed0`) | `0x1aaed0` | **`0x1ab200`** | The allocator used for PlayerTelemetry (CAS-locked) |
| 35 | `Telemetry::bindTransport` (`thunk_FUN_18048644c`) | `0x48644c` | **`0x4864ec`** | Called by init_telemetry right after Telemetry::ctor |

All validated UNIQUE via 24--64 B wildcard byte-patterns in .10 `.text`.

## 2. `TelemetryConfig` struct layout

Reverse-engineered from `TelemetryConfig::set_kv` (`0x48a784`),
`TelemetryConfig::default_init` (`0x4882c8`), `TelemetryConfig::dtor` (`0x488804`),
`TelemetryConfig::has_address` (`0x48bfc8`), and `PlayerTelemetry::ctor`
(`0x48e4a0`) which consumes the struct. Size is **0x38 bytes** on x64.

```c
// Mirrors AIR's internal `TelemetryConfig` -- total 0x38 bytes, alignment 8.
struct AirString {                       // size 0x10
    char*    data;                       // +0x00
    int32_t  len;                        // +0x08   <-- has_address checks this at cfg+0x18
    int32_t  cap;                        // +0x0C
};

struct TelemetryConfig {                 // size 0x38
    uint8_t  Stage3DCapture;             // +0x00  (set by parse_bool_pair)
    uint8_t  Stage3DCapture_standby;     // +0x01  (1 if value == "standby")
    uint8_t  DisplayObjectCapture;       // +0x02  (bool)
    uint8_t  SamplerEnabled;             // +0x03  (bool)
    uint8_t  SamplerEnabled_standby;     // +0x04  (1 if value == "standby")
    uint8_t  CPUCapture;                 // +0x05  (bool)
    uint8_t  ScriptObjectAllocationTraces;   // +0x06 (bool)
    uint8_t  AllGCAllocationTraces;      // +0x07  (bool)
    uint32_t GCAllocationTracesThreshold;// +0x08  default 0x400 (1024)
    uint32_t _pad0;                      // +0x0C
    AirString host;                      // +0x10  host.len at +0x18 (has_address test)
    uint32_t port;                       // +0x20  default 0x1EFE (7934)
    uint32_t _pad1;                      // +0x24
    AirString password;                  // +0x28  freed by dtor
};
static_assert(sizeof(TelemetryConfig) == 0x38);
```

**Write path** — `set_kv` dispatches by key name, then:
- `TelemetryAddress=host:port` -> parse_addr_port writes `host` via `AirString::assign` and `port` as int32.
- `TelemetryPassword=...`       -> `AirString::assign(&cfg.password, value)`.
- `SamplerEnabled=v`            -> `parse_bool_pair(v, &cfg[3], &cfg[4])`.
- `Stage3DCapture=v`            -> `parse_bool_pair(v, &cfg[0], &cfg[1])`.
- `DisplayObjectCapture=v`      -> `cfg[2] = parse_bool(v)`.
- `CPUCapture=v`                -> `cfg[5] = parse_bool(v)`.
- `ScriptObjectAllocationTraces=v` -> `cfg[6] = parse_bool(v)`.
- `AllGCAllocationTraces=v`     -> `cfg[7] = parse_bool(v)`.
- `GCAllocationTracesThreshold=v` -> `*(int*)&cfg[8] = atoi(v)`.

**Read path by `PlayerTelemetry::ctor` (`0x48e4a0`)**:
```
playertelemetry+0x40 <- cfg[0]   (Stage3DCapture)
              +0x42 <- cfg[2]   (DisplayObjectCapture)
              +0x44 <- cfg[3]   (SamplerEnabled)
              +0x47 <- cfg[5]   (CPUCapture)
              +0x48 <- cfg[6]   (ScriptObjectAllocationTraces)
              +0x4b <- cfg[7]   (AllGCAllocationTraces)
              +0x50 <- *(u32*)(cfg+8)   (GCAllocationTracesThreshold)
```
`cfg[1]` and `cfg[4]` (the `_standby` flags) are **not** read by the ctor --
they are an internal set_kv side-channel. We can leave them zero.

**Consumption of host/port** -- `init_telemetry` passes the cfg values
directly to `SocketTransport::ctor`:
```
FUN_18048ea54(transport_alloc, player, cfg.host.data, cfg.port);
```
(it reads cfg+0x10 and cfg+0x20 out of its own `local_50` stack).
**Password** is stored only inside the cfg; `PlayerTelemetry::ctor` does not
touch it. In stock Scout it is used by the command handler for authenticated
commands. For a passive capture we can leave it empty.

## 3. Path to obtain `Player*` from the ANE

### Primary (recommended): TLS-backed FRE frame lookup

Inside any FREFunction we register, the AS3 thread is in the middle of an
AvmCore-managed call. The runtime maintains a **per-thread stack of FRE
frames** in TLS slot `DAT_18122f458`. The top of that stack is the active
frame whose `+0x10` is a `Toplevel*` whose `+0x28` is a `PoolObject` whose
`+0x8` is the owning `AvmCore*`, at `+0xac0` of which lives the `Player*`.

Adobe itself collapses this chain in **one exported helper**,
`FUN_1802d6da8` (.10 `0x2d71e8`, 20 B):

```asm
mov rax, [rcx + 0x10]          ; frame.toplevel
mov rax, [rax + 0x28]          ; toplevel.pool
mov rax, [rax + 0x08]          ; pool.core   (AvmCore*)
mov rax, [rax + 0xAC0]         ; core.player (Player*)
ret
```

Combined with `FUN_18058d6b0` (.10 `0x58d390`), which returns the current
frame (or 0), the ANE can obtain Player* with two indirect calls:

```cpp
// 51.1.3.10 x64 only
using fn_getActiveFrame = void* (*)();
using fn_playerFromFrame = void* (*)(void*);

static fn_getActiveFrame   getActiveFrame  =
    reinterpret_cast<fn_getActiveFrame>(air_base + 0x58d390);
static fn_playerFromFrame  playerFromFrame =
    reinterpret_cast<fn_playerFromFrame>(air_base + 0x2d71e8);

void* tryGetPlayer() {
    void* frame = getActiveFrame();
    if (!frame) return nullptr;                 // not in an AS3 call
    return playerFromFrame(frame);              // Player*
}
```

This **only** yields Player* when called on an AS3 thread during an active
FREFunction dispatch. The `Profiler.start()` dispatcher, being a FREFunction,
satisfies that. Cache the pointer at first call; reuse on stop/marker.

### Secondary fallback (not needed, kept for completeness)

Going through our own FREContext handle:

```cpp
// opaque FREContext ctx (first arg of every FREFunction)
using fn_ctxInternal = longlong* (*)(longlong);
fn_ctxInternal ctxInternal =
    reinterpret_cast<fn_ctxInternal>(air_base + 0x58d3f8);
void* internal = ctxInternal((longlong)ctx);
// internal+0x90 == our ctx, internal+0x60 == native data, etc.
// The same FUN_1802d6da8 trick also works here: internal+0x10 is the
// Toplevel of the *context that was registered with this extension*.
void* player = playerFromFrame(internal);
```

Use this only if the primary path returns null (e.g. if we ever need to call
from a non-AS thread; we currently do not).

### No-go paths (investigated and rejected)

- **Global `Player*` singleton** -- there is none. Unlike `AvmCore::getActiveCore()`
  which resolves to `GC::GetActiveGC()->core()` (both inlined), Player has no
  `getInstance`. The only globals found are `DAT_181223f60` (the per-process
  GCHeap), `DAT_181223f28` (GC config), `DAT_181223fa0` (allocator ref),
  `DAT_18122f448` (FREContext hashmap), `DAT_18122f450` (hashmap lock),
  `DAT_18122f458` (TLS index). None directly expose Player.
- **Hooking `init_telemetry` at boot to capture Player\*** -- the ANE's
  `initialize()` fires only *after* the Player has already booted and
  loaded the first SWF, which is well after `init_telemetry` has already
  run. So there is no earlier window. TLS path is strictly better.
- **`AvmCore::enableSampler` / `AvmCore::setTelemetry`** -- both inlined.

## 4. Manual invocation of the init sequence (Mode B real)

### Is it safe to call `Player::init_telemetry` a second time?

Short answer: **yes, provided the first call was a no-op**. Which is the
case in Mode B's idle state: `.telemetry.cfg` is missing -> `has_address == 0`
-> the function takes the early-exit branch and never writes to
`player+0x1650/0x1658/0x1660`. State is clean.

However the function constructs its own `TelemetryConfig local_50` on stack
and always runs `read_file`. To inject our synthetic config, our options
(in decreasing order of preference):

**Option A -- Replay the alloc sequence directly (recommended).** Skip
`init_telemetry` entirely and just do what its body does, with known RVAs
and a cfg struct we own. This is 6 function calls plus field writes; each
function is already identified:

```cpp
// Assume caller has cached air_base and Player* player.
// All RVAs are from 51.1.3.10.

extern "C" {
    // 0x58d390:
    void* air_getActiveFrame();
    // 0x2d71e8:
    void* air_playerFromFrame(void*);
    // 0x4882c8:
    void* air_cfgDefaultInit(void* cfg);
    // 0x488804:
    void  air_cfgDtor(void* cfg);
    // 0x30ec10:  (AirString*, const char*, size_t len_or_-1)
    void  air_airstringAssign(void* s, const char* c, size_t len);
    // 0x48bfc0: (const char* v, u8* out_a, u8* out_b)
    void  air_parseBoolPair(const char* v, uint8_t* a, uint8_t* b);
    // 0x1a0a64: (size_t, int flags) -> void*
    void* air_alloc(size_t size, int flags);
    // 0x1ab200: (GCHeap* heap, size_t, int flags) -> void*
    void* air_allocLocked(void* heap, size_t size, int flags);
    // 0x48eb10: SocketTransport::ctor(this, player, host_cstr, port)
    void* air_socketTransportCtor(void* self, void* player, const char* h, uint p);
    // 0x4852c0: Telemetry::ctor(this, transport)
    void* air_telemetryCtor(void* self, void* transport);
    // 0x4864ec: Telemetry::bindTransport(telemetry)
    void  air_telemetryBindTransport(void* telemetry);
    // 0x48e4a0: PlayerTelemetry::ctor(this, player, telemetry, &cfg)
    void* air_playerTelemetryCtor(void* self, void* player, void* telemetry, void* cfg);
}
// Globals from .data:
extern void** DAT_181223f60;            // GCHeap singleton
extern int*   DAT_181223f60_lock;       // = (int*)((char*)DAT_181223f60 + 0xb98)

struct TelemetryConfig cfg;

bool enableTelemetryNow(void* player, const char* host, uint port,
                        bool sampler, bool stage3d, bool disp, bool cpu,
                        bool scriptAlloc, bool allGc, uint threshold)
{
    // 1. default-init cfg (sets port=7934, threshold=1024, everything else zero)
    air_cfgDefaultInit(&cfg);

    // 2. fill in user values via the real setters (no manual byte poking
    //    so future runtime versions that reorder fields still work)
    air_airstringAssign(&cfg.host, host, strlen(host));
    cfg.port = port;
    uint8_t unused;
    air_parseBoolPair(sampler ? "1" : "0", &cfg.SamplerEnabled, &cfg.SamplerEnabled_standby);
    cfg.Stage3DCapture            = stage3d ? 1 : 0;
    cfg.DisplayObjectCapture      = disp    ? 1 : 0;
    cfg.CPUCapture                = cpu     ? 1 : 0;
    cfg.ScriptObjectAllocationTraces = scriptAlloc ? 1 : 0;
    cfg.AllGCAllocationTraces     = allGc   ? 1 : 0;
    cfg.GCAllocationTracesThreshold = threshold;

    // 3. has_address will be > 0 because host.len is >0 now -- replay init body:
    void* transport = air_alloc(0x40, 1);
    if (!transport) { air_cfgDtor(&cfg); return false; }
    air_socketTransportCtor(transport, player, cfg.host.data, cfg.port);

    void* telemetry = air_alloc(0x110, 1);
    if (!telemetry) { /* leak transport -- see risks */ air_cfgDtor(&cfg); return false; }
    air_telemetryCtor(telemetry, transport);
    air_telemetryBindTransport(telemetry);   // same call init_telemetry emits

    // 4. PlayerTelemetry uses the locked allocator (0x220 bytes).
    // Spinlock it in exactly the same pattern the runtime does:
    for (int attempts = 0; ; ++attempts) {
        int expected = 0;
        if (__atomic_compare_exchange_n(DAT_181223f60_lock, &expected, 1,
                                        false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        Sleep((attempts & 0x3f) == 0 ? 1 : 0);
    }
    void* playerTel = air_allocLocked(*DAT_181223f60, 0x220, 1);
    // (the runtime also does: *(DAT_181223f60+0xba0)=ptr; *(DAT_181223f60+0xba8)=size;)
    ((void**)DAT_181223f60)[0xba0/8] = playerTel;
    ((size_t*)DAT_181223f60)[0xba8/8] = 0x220;
    __atomic_store_n(DAT_181223f60_lock, 0, __ATOMIC_RELEASE);
    if (!playerTel) { /* leaks both */ air_cfgDtor(&cfg); return false; }
    air_playerTelemetryCtor(playerTel, player, telemetry, &cfg);

    // 5. store on Player and wire transport->callback
    *((void**)((char*)player + 0x1650)) = transport;
    *((void**)((char*)player + 0x1658)) = telemetry;
    *((void**)((char*)player + 0x1660)) = playerTel;
    *((void**)((char*)transport + 0x38)) = playerTel;

    air_cfgDtor(&cfg);   // frees strings; cfg is copied into substructures already
    return true;
}
```

**Option B -- call `Player::init_telemetry` a second time, with a
read_file detour.** Install an inline hook on `TelemetryConfig::read_file`
(`0x48c1e8`) whose body is *"write host/port into the passed cfg and
return"*. Then invoke `Player::init_telemetry(player)` with the cached
Player\*. Remove the detour immediately after.

This is simpler but has two drawbacks:
1. We must write thread-safe memory-patch code (VirtualProtect, atomic
   install/uninstall). Option A is a sequence of ordinary function calls.
2. If the runtime ever calls read_file for unrelated reasons while our
   detour is installed, we corrupt its caller's stack. Option A has no
   such global hook.

**Recommendation: Option A.**

### Sequence of `Adobe AIR.dll` functions the ANE needs to call

(Summarised table; all RVAs for 51.1.3.10, image base `0x180000000`.)

| Step | Function | RVA | Signature |
|------|----------|-----|-----------|
| 1 | `getActiveFrame_TLS`        | `0x58d390` | `void* ()` |
| 2 | `Player_from_Frame`         | `0x2d71e8` | `void* (void* frame)` |
| 3 | `TelemetryConfig::default_init` | `0x4882c8` | `void* (void* cfg)` |
| 4 | `AirString::assign`         | `0x30ec10` | `void (AirString*, const char* c, size_t len_or_-1)` |
| 5 | `TelemetryConfig::parse_bool_pair` | `0x48c100` | `void (const char*, u8* a, u8* b)` |
| 6 | `MMgc::alloc_small`         | `0x1a0a64` | `void* (size_t, int flags)` |
| 7 | `SocketTransport::ctor`     | `0x48eb10` | `void* (void*, void* player, const char* host, uint port)` |
| 8 | `Telemetry::ctor`           | `0x4852c0` | `void* (void*, void* transport)` |
| 9 | `Telemetry::bindTransport`  | `0x4864ec` | `void (void* telemetry)` |
| 10 | `MMgc::alloc_locked`       | `0x1ab200` | `void* (void* heap, size_t, int flags)` |
| 11 | `PlayerTelemetry::ctor`    | `0x48e4a0` | `void* (void*, void* player, void* telemetry, void* cfg)` |
| 12 | `TelemetryConfig::dtor`    | `0x488804` | `void (void* cfg)` |

And for stop/teardown (Mode B2 from PROFILER_IMPLEMENTATION_PLAN.md §4):

| Step | Function | RVA | Role |
|------|----------|-----|------|
| 13 | `PlayerTelemetry::dtor`     | `0x48ef84` | calls destructors on internal queues |
| 14 | `Telemetry::dtor_unwind`    | `0x485604` | unwinds 6 vtables |
| 15 | `SocketTransport::dtor`     | `0x48f118` | releases PlatformSocketWrapper, frees host |

(Plus zero out `player+0x1650/+0x1658/+0x1660` afterwards.)

## 5. Mode B full pseudocode

```cpp
// Installed at module load of AneAwesomeUtilsANE.dll.
static HMODULE     g_air = nullptr;                // Adobe AIR.dll
static uintptr_t   g_airBase = 0;
static void*       g_player = nullptr;             // captured on first FRE call
static bool        g_active = false;

// --- Hook on send_bytes at +0x493060 (already implemented in ane-awesome-utils).

// --- On AneAwesomeUtilsExtension.initialize():
void initExtension() {
    g_air = GetModuleHandleW(L"Adobe AIR.dll");
    g_airBase = (uintptr_t)g_air;
    if (!verifyAirSha256())      // optional safety check
        return;
    // Do NOT enable anything yet. Zero idle overhead.
}

// --- On Profiler.start(cfg) (a FREFunction):
FREObject frStart(FREContext ctx, void*, uint32_t argc, FREObject argv[]) {
    if (!g_player) {
        auto getFrame = (void*(*)())(g_airBase + 0x58d390);
        auto playerFromFrame = (void*(*)(void*))(g_airBase + 0x2d71e8);
        void* fr = getFrame();
        if (!fr) return nullptr;                    // not on AS thread -- give up
        g_player = playerFromFrame(fr);
    }
    if (!g_player) return nullptr;
    // 1. patch the PlatformSocketWrapper vtable slot 11 so the first
    //    send_bytes goes to our tap instead of PlatformSocket::send.
    patchVtableSlot(g_airBase + 0x00ecb828 + 0x58, &my_send_bytes_hook);
    // 2. build the cfg with desired profile and replay init body.
    enableTelemetryNow(g_player, "127.0.0.1", 7934,
                       /*sampler=*/true,  /*stage3d=*/true,  /*disp=*/true,
                       /*cpu=*/true, /*scriptAlloc=*/false, /*allGc=*/false,
                       /*threshold=*/1024);
    g_active = true;
    return nullptr;
}

// --- On Profiler.stop():
FREObject frStop(FREContext ctx, void*, uint32_t, FREObject*) {
    if (!g_active) return nullptr;
    // Option B1 (simplest) -- swap vtable slot to a noop; Telemetry
    // keeps producing bytes but they vanish.
    patchVtableSlot(g_airBase + 0x00ecb828 + 0x58, &noop_send_bytes);
    // Option B2 -- destroy the whole chain so the runtime drops to idle.
    // destroyTelemetryChain(g_player, g_airBase);
    g_active = false;
    return nullptr;
}
```

## 6. Alternative: binary patch to force telemetry on at boot (Part 4)

If the above manual invocation proves too fragile, a simpler bypass exists:

**Patch A.** Overwrite `TelemetryConfig::has_address` (.10 `0x48bfc8`, 8 bytes)
with a 1-return stub:

```
original: 48 63 41 18              movsxd  rax, dword ptr [rcx+0x18]
          48 85 C0                 test    rax, rax       (paraphrased; real is shorter)
          ...
patched : B0 01 C3 CC CC CC CC CC  mov     al, 1 ; ret
```

**Patch B.** In `TelemetryConfig::default_init` (.10 `0x4882c8`, 39 bytes),
after the default writes, append a write to cfg+0x10 (host) and cfg+0x18
(host length). Simplest: leak a static `char host[] = "127.0.0.1\0"` in
our own DLL and point cfg->host.data at it, length=9, cap=10.

**Pros**
- Patches run once at module load. Runtime's normal `init_telemetry` at
  boot does all the work. Zero additional code path to debug.
- `send_bytes` hook (already done) captures the stream.

**Cons -- and this is why I do not recommend this path**
1. **It kills the Mode B zero-idle property.** Telemetry allocates
   SocketTransport + Telemetry + PlayerTelemetry on every run, not only
   when the user presses Start. `PlayerTelemetry::ctor` emits ~20
   `.player.*` events. SocketTransport tries to `connect()` to the
   faked address. That's real, measurable overhead on every boot.
2. **It connects a real socket.** Our send_bytes hook is the ONLY thing
   preventing the runtime from opening a TCP connection to 127.0.0.1:7934.
   If the hook is installed after the first connect() runs, we leak a
   blocking socket operation (WSASocketW + connect). Replacing slot 1
   of SocketTransport::vftable (open_connect, .10 `0x4930d0`) with a
   noop solves it but is yet another patch.
3. **Stop is impossible without destructors.** Once telemetry is live,
   the runtime has no "turn it off" path other than destroying the
   PlayerTelemetry + Telemetry + SocketTransport trio. We would need
   exactly the same code as Option A's stop path, so no simplification.

**Verdict**: Option A (manual invocation on Start) is strictly simpler
overall because it keeps the idle path at zero cost and moves all the
enable/disable logic to ANE code under our control. Patching only makes
sense if for some reason we cannot capture Player\* from TLS.

## 7. Risks and mitigations

| Risk | Mitigation |
|------|-----------|
| `FUN_18058d6b0` returns null on non-AS thread | Profiler.start() is a FREFunction; AS thread is guaranteed. Still, null-check and fail the call with an error back to AS3. |
| Player layout (+0x1650/0x1658/0x1660) wrong for 51.1.3.10 | CDB-validate once on boot: after forced-enable, breakpoint on `0x493060` and verify `this == *(player+0x1650)+0x38`. |
| `MMgc::alloc_locked` lock contention with GC | Hold the `DAT_181223f60+0xb98` CAS lock for microseconds only; mirror the exact pattern the runtime uses. Do not hold across the PlayerTelemetry::ctor call. |
| Second-time run hits the early-exit (has_address==0) not clean | Our synthetic cfg has `host.len > 0` so has_address is true; branch is taken. But we're not calling `init_telemetry`, we're replaying its body. So this doesn't apply to Option A. |
| `TelemetryConfig::dtor` double-frees our stack cfg | `dtor` only frees the AirStrings; as long as we either let `AirString::assign` allocate the strings (heap) or leave cap=0 (so AirString::clear is a no-op on DAT_180e9c8c0 empty sentinel), no double-free. |
| `PlayerTelemetry::ctor` keeps `&cfg` after return | It does not; ctor copies all fields (see §2 read path). Cfg is safe to free after ctor returns. |
| Winsock send from runtime's thread blocks on broken peer | Our vtable-slot patch on PlatformSocketWrapper+0x58 replaces send_bytes *before* init is triggered. The runtime never calls real `send()`. |
| Stop (B1) leaves Telemetry producing bytes | Bytes go to our noop thunk; CPU cost is ~1 AMF3 encode + 1 vtable call per event. Acceptable for seconds-long sessions; for long idle, use B2 (destructor cascade). |
| Version drift (51.1.3.11/.12/.13) | All RVAs keyed to this SHA256; verify at boot with `verifyAirSha256()`. Abort feature if mismatch. |

## 8. Recommendation

**Ship Mode B via manual invocation (Option A), not via patch bypass.**

Reasons:
- Zero idle overhead is preserved because Player::init_telemetry's early-exit
  branch continues to fire on boot.
- All required runtime functions are already catalogued with RVAs and signatures.
- Stop/start are symmetric: call `enableTelemetryNow()` on start, patch vtable
  slot to noop on stop (Option B1 from PROFILER_IMPLEMENTATION_PLAN.md §4).
- No binary patches outside our own ANE's memory (the vtable slot patch is
  already in the plan). VirtualProtect footprint is a single 8-byte slot in
  `.rdata`.
- Fails gracefully on version mismatch; doesn't brick the runtime.

Effort delta over the existing plan: add `WindowsRuntimeHook::enable()` to
call `enableTelemetryNow()` (above); extend `AirTelemetryRvas.h` with the
11 new RVAs from §1. No Ghidra re-analysis needed.

## 9. Artefacts

- RVA resolution script: `C:\tmp\air_rva_analysis\find_rvas_v3.py`
- Previous RVA table: `docs/profiler-rva-51-1-3-10.md`
- Plan: `docs/PROFILER_IMPLEMENTATION_PLAN.md` (§4 "Modo B")
- Source of decompiled references: `C:\AIRSDKs\AIRSDK_51.1.3.12\binary-optimizer\analysis-output\win-x64\all_functions_decompiled.c`

## 10. Open items / things worth an hour of CDB before committing

1. Confirm `*((void**)(DAT_181223f60)+0xba0/8)` is the right global slot for
   the last-alloc bookkeeping, or if the runtime ignores it. (If ignored,
   we can skip that write and the lock.)
2. Confirm `AirString::assign` with len=-1 computes strlen. We believe it
   does (see line 593546-593548 of all_functions_decompiled.c), but worth
   a quick assert.
3. Sniff-test: call `enableTelemetryNow` with `host="127.0.0.1"` and
   confirm we see (a) PlayerTelemetry writing `.player.version` via
   the vtable chain, (b) our send_bytes hook catching the bytes,
   (c) no `WSASocketW` call on the trace.
4. x86 backport (Windows32): all structure offsets will halve for pointer
   fields; requires a separate find_rvas_v4.py run on the 32-bit DLL.
   Out of scope for this doc.
