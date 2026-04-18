// WindowsAirRuntime — binding to Adobe AIR.dll 51.1.3.10 (Windows x64 + x86)
// entry points for forcing telemetry on demand (Mode B).
//
// Per-arch RVAs live in AirTelemetryRvas.h under two namespaces; this
// translation unit switches between them at compile time via `air_rvas`.
//
// Calling conventions diverge between x64 and x86 so the function-pointer
// typedefs here are gated on arch macros:
//   x64: Microsoft fastcall (implicit; `__thiscall`/`__cdecl`/`__fastcall`
//        all fold to the same ABI).
//   x86: the runtime mixes `__thiscall`, `__cdecl`, `__fastcall` and
//        callee-cleanup thunks. Each pointer type declares the right one.

#include "profiler/WindowsAirRuntime.hpp"

#include "AirTelemetryRvas.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#if defined(_M_IX86) || defined(__i386__)
#  define ANE_AIR_THISCALL   __thiscall
#  define ANE_AIR_CDECL      __cdecl
#  define ANE_AIR_FASTCALL   __fastcall
#else
#  define ANE_AIR_THISCALL
#  define ANE_AIR_CDECL
#  define ANE_AIR_FASTCALL
#endif

namespace ane::profiler {

// -------------------------------------------------------------------------
// Compile-time guards — make sure the header actually exposed the right
// namespace for this build's target architecture.
// -------------------------------------------------------------------------
#if !defined(_M_X64) && !defined(__x86_64__) && !defined(_M_IX86) && !defined(__i386__)
#  error "Unsupported target architecture for WindowsAirRuntime"
#endif

// -------------------------------------------------------------------------
// Prologue signature check — prevents catastrophic crashes when the runtime
// DLL doesn't match the RVA map we were built against.
// -------------------------------------------------------------------------
static bool verify_runtime_signature(std::uintptr_t air_base) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(
        air_base + air_rvas::kRvaPlayerInitTelemetry);
    for (std::size_t i = 0; i < sizeof(air_rvas::kInitTelemetryPrologue); ++i) {
        if (bytes[i] != air_rvas::kInitTelemetryPrologue[i]) return false;
    }
    return true;
}

// -------------------------------------------------------------------------
// Typed function-pointer definitions (arch-aware).
// -------------------------------------------------------------------------

#if defined(_M_X64) || defined(__x86_64__)

// x64: one calling convention (MS x64). this-ptr in rcx as first arg.
using FnGetActiveFrame      = void* (*)();
using FnPlayerFromFrame     = void* (*)(void*);
using FnCfgDefaultInit      = void* (*)(void*);
using FnCfgDtor             = void  (*)(void*);
using FnCfgParseBoolPair    = void  (*)(const char*, std::uint8_t*, std::uint8_t*);
using FnAirStringAssign     = void  (*)(void*, const char*, std::size_t);
using FnAirStringClear      = void  (*)(void*);
using FnAllocSmall          = void* (*)(std::size_t, int);
using FnAllocLocked         = void* (*)(void*, std::size_t, int);
using FnSocketTransportCtor = void* (*)(void*, void*, const char*, std::uint32_t);
using FnTelemetryCtor       = void* (*)(void*, void*);
using FnTelemetryBind       = void  (*)(void*);
using FnPlayerTelemetryCtor = void* (*)(void*, void*, void*, void*);
using FnSocketTransportDtor = void  (*)(void*);
using FnTelemetryDtor       = void  (*)(void*);
using FnPlayerTelemetryDtor = void  (*)(void*);

#else // x86

// x86: this = ecx, args = stack (thiscall), callee cleans with `ret N`.
// MSVC folds __thiscall to fastcall via the first-two-in-ecx/edx rule only
// if the function is a member; for __thiscall declared via pointer we need
// to use the __thiscall keyword explicitly (or the class-member pointer
// syntax). Since these are C function-pointer typedefs, we use the
// explicit calling convention keywords.
using FnPlayerFromFrame     = void* (ANE_AIR_THISCALL*)(void*);
using FnCfgDefaultInit      = void* (ANE_AIR_THISCALL*)(void*);
using FnCfgDtor             = void  (ANE_AIR_THISCALL*)(void*);
using FnCfgParseBoolPair    = void  (ANE_AIR_CDECL*   )(const char*, std::uint8_t*, std::uint8_t*);
using FnAirStringAssign     = void  (ANE_AIR_THISCALL*)(void*, const char*, std::size_t);
using FnAirStringClear      = void  (ANE_AIR_THISCALL*)(void*);
using FnAllocSmall          = void* (ANE_AIR_CDECL*   )(std::size_t, int);
using FnAllocLocked         = void* (ANE_AIR_FASTCALL*)(void*, std::size_t, int);
using FnSocketTransportCtor = void* (ANE_AIR_THISCALL*)(void*, void*, const char*, std::uint32_t);
using FnTelemetryCtor       = void* (ANE_AIR_THISCALL*)(void*, void*);
using FnTelemetryBind       = void  (ANE_AIR_CDECL*   )(int); // thunk: push 1; call body
using FnPlayerTelemetryCtor = void* (ANE_AIR_THISCALL*)(void*, void*, void*, void*);
using FnSocketTransportDtor = void  (ANE_AIR_THISCALL*)(void*);
using FnTelemetryDtor       = void  (ANE_AIR_THISCALL*)(void*);
using FnPlayerTelemetryDtor = void  (ANE_AIR_THISCALL*)(void*);

#endif

// -------------------------------------------------------------------------
// Opaque storage for the function pointers. We store as void* in the
// class so the header doesn't need to pull in the typedefs; every use
// casts to the right FnXxx type at the call site.
// -------------------------------------------------------------------------

struct AirRuntimeFns {
#if defined(_M_X64) || defined(__x86_64__)
    FnGetActiveFrame       getActiveFrame      = nullptr;
#endif
    FnPlayerFromFrame      playerFromFrame     = nullptr;
    FnCfgDefaultInit       cfgDefaultInit      = nullptr;
    FnCfgDtor              cfgDtor             = nullptr;
    FnCfgParseBoolPair     cfgParseBoolPair    = nullptr;
    FnAirStringAssign      airStringAssign     = nullptr;
    FnAllocSmall           allocSmall          = nullptr;
    FnAllocLocked          allocLocked         = nullptr;
    FnSocketTransportCtor  socketTransportCtor = nullptr;
    FnTelemetryCtor        telemetryCtor       = nullptr;
    FnTelemetryBind        telemetryBind       = nullptr;
    FnPlayerTelemetryCtor  playerTelemetryCtor = nullptr;
    FnSocketTransportDtor  socketTransportDtor = nullptr;
    FnTelemetryDtor        telemetryDtor       = nullptr;
    FnPlayerTelemetryDtor  playerTelemetryDtor = nullptr;
};

// We keep one global AirRuntimeFns slot because WindowsAirRuntime is
// effectively a singleton (there's only one AIR runtime per process). This
// also avoids tunnelling C-style types through the class header.
static AirRuntimeFns g_fns;

// -------------------------------------------------------------------------
// initialize
// -------------------------------------------------------------------------

bool WindowsAirRuntime::initialize() {
    if (initialized_.load(std::memory_order_acquire)) return true;

    HMODULE mod = GetModuleHandleA(air_rvas::kDllName);
    if (mod == nullptr) return false;
    air_base_ = reinterpret_cast<std::uintptr_t>(mod);

    if (!verify_runtime_signature(air_base_)) {
        // Wrong runtime version — Mode B disabled. Mode A via .telemetry.cfg
        // still works because all it needs is the send() IAT hook.
        air_base_ = 0;
        return false;
    }

    auto resolve = [&](std::uint32_t rva) -> void* {
        return reinterpret_cast<void*>(air_base_ + rva);
    };

#if defined(_M_X64) || defined(__x86_64__)
    g_fns.getActiveFrame  = reinterpret_cast<FnGetActiveFrame>(resolve(air_rvas::kRvaFreGetActiveFrame));
#endif
    g_fns.playerFromFrame = reinterpret_cast<FnPlayerFromFrame>(resolve(air_rvas::kRvaFrePlayerFromFrame));
    g_fns.cfgDefaultInit  = reinterpret_cast<FnCfgDefaultInit>(resolve(air_rvas::kRvaTelemetryConfigDefaultInit));
    g_fns.cfgDtor         = reinterpret_cast<FnCfgDtor>       (resolve(air_rvas::kRvaTelemetryConfigDtor));
    g_fns.cfgParseBoolPair= reinterpret_cast<FnCfgParseBoolPair>(resolve(air_rvas::kRvaTelemetryConfigParseBoolPair));
    g_fns.airStringAssign = reinterpret_cast<FnAirStringAssign>(resolve(air_rvas::kRvaAirStringAssign));
    g_fns.allocSmall      = reinterpret_cast<FnAllocSmall>    (resolve(air_rvas::kRvaMMgcAllocSmall));
    g_fns.allocLocked     = reinterpret_cast<FnAllocLocked>   (resolve(air_rvas::kRvaMMgcAllocLocked));
    g_fns.socketTransportCtor = reinterpret_cast<FnSocketTransportCtor>(resolve(air_rvas::kRvaSocketTransportCtor));
    g_fns.telemetryCtor   = reinterpret_cast<FnTelemetryCtor> (resolve(air_rvas::kRvaTelemetryCtor));
    g_fns.telemetryBind   = reinterpret_cast<FnTelemetryBind> (resolve(air_rvas::kRvaTelemetryBindTransport));
    g_fns.playerTelemetryCtor = reinterpret_cast<FnPlayerTelemetryCtor>(resolve(air_rvas::kRvaPlayerTelemetryCtor));
    g_fns.socketTransportDtor = reinterpret_cast<FnSocketTransportDtor>(resolve(air_rvas::kRvaSocketTransportDtor));
    g_fns.telemetryDtor   = reinterpret_cast<FnTelemetryDtor> (resolve(air_rvas::kRvaTelemetryDtor));
    g_fns.playerTelemetryDtor = reinterpret_cast<FnPlayerTelemetryDtor>(resolve(air_rvas::kRvaPlayerTelemetryDtor));

    psw_vtable_addr_ = air_base_ + air_rvas::kRvaVtPlatformSocketWrapper;

    initialized_.store(true, std::memory_order_release);
    return true;
}

// -------------------------------------------------------------------------
// tryCapturePlayer
// -------------------------------------------------------------------------

// Loosely validates that `p` looks like a Player pointer we can dereference.
// We require the memory range around `p + kPlayerOffsetPlayerTelemetry` to
// be readable, since init_telemetry writes there.
static bool looks_like_player(void* p) {
    if (p == nullptr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    // Must include the full Player struct (up to the Telemetry slot).
    const auto need = reinterpret_cast<std::uintptr_t>(p) +
                      air_rvas::kPlayerOffsetPlayerTelemetry + sizeof(void*);
    const auto end  = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (need > end) return false;
    if ((mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                        PAGE_EXECUTE_WRITECOPY)) == 0) return false;
    return true;
}

void* WindowsAirRuntime::tryCapturePlayer(void* fre_context) {
    if (auto p = player_.load(std::memory_order_acquire); p != nullptr) return p;
    if (!initialized_.load(std::memory_order_acquire)) {
        if (!initialize()) return nullptr;
    }

#if defined(_M_X64) || defined(__x86_64__)
    // x64 has a stable TLS accessor — use it.
    (void)fre_context;
    if (g_fns.getActiveFrame == nullptr || g_fns.playerFromFrame == nullptr) return nullptr;
    void* frame = g_fns.getActiveFrame();
    if (frame == nullptr) return nullptr;
    void* player = g_fns.playerFromFrame(frame);
    if (looks_like_player(player)) {
        player_.store(player, std::memory_order_release);
        return player;
    }
    return nullptr;
#else
    // x86: getActiveFrame_TLS was inlined by MSVC so we can't call it
    // directly. Try walking the FREContext we were handed:
    //   FREContext → [maybe one deref] → frame-like pointer → Player_from_Frame.
    // The tries are guarded by VirtualQuery-based heuristics; anything
    // that doesn't look right is skipped, and we fall back to nullptr.
    if (g_fns.playerFromFrame == nullptr || fre_context == nullptr) return nullptr;

    auto tryChain = [&](void* candidate) -> void* {
        if (candidate == nullptr) return nullptr;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(candidate, &mbi, sizeof(mbi)) == 0) return nullptr;
        if (mbi.State != MEM_COMMIT) return nullptr;
        __try {
            void* p = g_fns.playerFromFrame(candidate);
            if (looks_like_player(p)) return p;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            // Bad pointer walk — ignore and continue.
        }
        return nullptr;
    };

    // Strategy 1: treat the FREContext handle itself as a frame.
    if (void* p = tryChain(fre_context); p) { player_.store(p); return p; }

    // Strategy 2: single indirection.
    __try {
        void* deref = *static_cast<void**>(fre_context);
        if (void* p = tryChain(deref); p) { player_.store(p); return p; }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    // Strategy 3: double indirection.
    __try {
        void* d1 = *static_cast<void**>(fre_context);
        if (d1 != nullptr) {
            void* d2 = *static_cast<void**>(d1);
            if (void* p = tryChain(d2); p) { player_.store(p); return p; }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    return nullptr;
#endif
}

// -------------------------------------------------------------------------
// forceEnableTelemetry / forceDisableTelemetry
//
// These are compiled only for x64. On x86 the alloc_locked path has
// subtle ABI and GCHeap-layout differences; a stub returning false keeps
// the API linkable while Mode B on x86 is still being shaken out.
// -------------------------------------------------------------------------

#if defined(_M_X64) || defined(__x86_64__)

bool WindowsAirRuntime::forceEnableTelemetry(const std::string& host,
                                             std::uint32_t port,
                                             const ProfilerFeatures& f) {
    if (!initialized_.load(std::memory_order_acquire)) return false;
    void* player = tryCapturePlayer();
    if (player == nullptr) return false;
    if (host.empty() || port == 0) return false;

    auto** playerTransportSlot  = reinterpret_cast<void**>(
        reinterpret_cast<char*>(player) + air_rvas::kPlayerOffsetSocketTransport);
    auto** playerTelemetrySlot  = reinterpret_cast<void**>(
        reinterpret_cast<char*>(player) + air_rvas::kPlayerOffsetTelemetry);
    auto** playerPtelemetrySlot = reinterpret_cast<void**>(
        reinterpret_cast<char*>(player) + air_rvas::kPlayerOffsetPlayerTelemetry);

    if (*playerTransportSlot != nullptr || *playerTelemetrySlot != nullptr ||
        *playerPtelemetrySlot != nullptr) {
        return false;
    }

    alignas(8) std::uint8_t cfg_mem[air_rvas::kTelemetryConfigSize];
    std::memset(cfg_mem, 0, sizeof(cfg_mem));
    g_fns.cfgDefaultInit(cfg_mem);

    g_fns.airStringAssign(cfg_mem + air_rvas::kTelemetryConfigOffHost, host.c_str(), host.size());
    *reinterpret_cast<std::uint32_t*>(cfg_mem + air_rvas::kTelemetryConfigOffPort) = port;

    g_fns.cfgParseBoolPair(f.sampler_enabled ? "1" : "0", cfg_mem + 0x03, cfg_mem + 0x04);
    g_fns.cfgParseBoolPair(f.stage3d_capture ? "1" : "0", cfg_mem + 0x00, cfg_mem + 0x01);
    cfg_mem[0x02] = f.display_object_capture ? 1 : 0;
    cfg_mem[0x05] = f.cpu_capture ? 1 : 0;
    cfg_mem[0x06] = f.script_object_allocation_traces ? 1 : 0;
    cfg_mem[0x07] = f.all_gc_allocation_traces ? 1 : 0;
    *reinterpret_cast<std::uint32_t*>(cfg_mem + 0x08) = f.gc_allocation_traces_threshold;

    void* transport = g_fns.allocSmall(air_rvas::kSocketTransportSize, 1);
    if (transport == nullptr) { g_fns.cfgDtor(cfg_mem); return false; }
    g_fns.socketTransportCtor(transport, player, host.c_str(), port);

    void* telemetry = g_fns.allocSmall(air_rvas::kTelemetrySize, 1);
    if (telemetry == nullptr) {
        g_fns.socketTransportDtor(transport);
        g_fns.cfgDtor(cfg_mem);
        return false;
    }
    g_fns.telemetryCtor(telemetry, transport);
    g_fns.telemetryBind(telemetry);

    void* gcheap = *reinterpret_cast<void**>(air_base_ + air_rvas::kRvaGcHeapPtrSlot);
    void* playerTelemetry = nullptr;
    if (gcheap != nullptr) {
        auto* lock_slot = reinterpret_cast<std::atomic<std::int32_t>*>(
            reinterpret_cast<char*>(gcheap) + air_rvas::kGcHeapLockOffset);
        for (int attempts = 0; attempts < 10000; ++attempts) {
            std::int32_t expected = 0;
            if (lock_slot->compare_exchange_weak(expected, 1,
                                                 std::memory_order_acquire,
                                                 std::memory_order_relaxed)) break;
            if ((attempts & 0x3f) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            else std::this_thread::yield();
        }
        playerTelemetry = g_fns.allocLocked(gcheap, air_rvas::kPlayerTelemetrySize, 1);
        *reinterpret_cast<void**>(reinterpret_cast<char*>(gcheap) +
                                   air_rvas::kGcHeapLastAllocPtrOffset)  = playerTelemetry;
        *reinterpret_cast<std::size_t*>(reinterpret_cast<char*>(gcheap) +
                                         air_rvas::kGcHeapLastAllocSizeOffset) =
                                             air_rvas::kPlayerTelemetrySize;
        lock_slot->store(0, std::memory_order_release);
    } else {
        playerTelemetry = g_fns.allocSmall(air_rvas::kPlayerTelemetrySize, 1);
    }
    if (playerTelemetry == nullptr) {
        g_fns.telemetryDtor(telemetry);
        g_fns.socketTransportDtor(transport);
        g_fns.cfgDtor(cfg_mem);
        return false;
    }

    g_fns.playerTelemetryCtor(playerTelemetry, player, telemetry, cfg_mem);

    *playerTransportSlot  = transport;
    *playerTelemetrySlot  = telemetry;
    *playerPtelemetrySlot = playerTelemetry;
    *reinterpret_cast<void**>(reinterpret_cast<char*>(transport) +
                               air_rvas::kSocketTransportOffsetPtel) = playerTelemetry;

    g_fns.cfgDtor(cfg_mem);
    return true;
}

void WindowsAirRuntime::forceDisableTelemetry() {
    void* player = player_.load(std::memory_order_acquire);
    if (player == nullptr || !initialized_.load(std::memory_order_acquire)) return;

    // Just null the Player slots. The previous trio (SocketTransport,
    // Telemetry, PlayerTelemetry) are unreachable from the Player after
    // this; MMgc will reclaim them on the next GC cycle. We intentionally
    // DO NOT call their C++ destructors — doing so crashes the runtime on
    // a subsequent force-enable because subsystems (sampler, event bus,
    // command registry) cache pointers we can't unregister without
    // deeper RE. Leaking the objects is cheap and safe.
    //
    // Zero-idle after stop is preserved because:
    //   - Player.telemetry is null → runtime's getTelemetry() check skips
    //     emission in the AS3 event dispatch path.
    //   - The loopback listener is torn down separately by the caller, so
    //     any straggling bytes from cached subsystems hit a closed socket.
    auto** playerTransportSlot  = reinterpret_cast<void**>(
        reinterpret_cast<char*>(player) + air_rvas::kPlayerOffsetSocketTransport);
    auto** playerTelemetrySlot  = reinterpret_cast<void**>(
        reinterpret_cast<char*>(player) + air_rvas::kPlayerOffsetTelemetry);
    auto** playerPtelemetrySlot = reinterpret_cast<void**>(
        reinterpret_cast<char*>(player) + air_rvas::kPlayerOffsetPlayerTelemetry);

    *playerTransportSlot  = nullptr;
    *playerTelemetrySlot  = nullptr;
    *playerPtelemetrySlot = nullptr;
}

#else // x86

// x86 Mode B is currently best-effort — we can invoke init_telemetry as
// long as Player* is obtainable from FREContext. The force-enable path is
// left unimplemented for now: it requires pinning down the x86 GCHeap layout
// and the MSVC fastcall ABI for alloc_locked. Mode A (via .telemetry.cfg)
// still works on x86 because the IAT send hook is architecture-neutral.

bool WindowsAirRuntime::forceEnableTelemetry(const std::string& /*host*/,
                                             std::uint32_t /*port*/,
                                             const ProfilerFeatures& /*f*/) {
    return false;
}

void WindowsAirRuntime::forceDisableTelemetry() {
    // Nothing was forced on; nothing to tear down.
}

#endif

} // namespace ane::profiler
