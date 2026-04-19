// WindowsAirRuntime — binding to Adobe AIR.dll 51.1.3.10 (Windows x64 + x86)
// entry points for forcing telemetry on demand (Mode B).
//
// Per-arch RVAs live in AirTelemetryRvas.h under two namespaces; this
// translation unit switches between them at compile time via `air_rvas`.
//
// x64 uses the Microsoft x64 ABI for everything (first 4 args in
// rcx/rdx/r8/r9). x86 mixes `__thiscall`, `__cdecl` and `__fastcall`
// depending on the function — each typedef below declares the right one.

#include "profiler/WindowsAirRuntime.hpp"

#include "AirTelemetryRvas.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace ane::profiler {

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
// Typed function pointers (arch-aware).
// -------------------------------------------------------------------------

#if defined(_M_X64) || defined(__x86_64__)

using FnGetActiveFrame      = void* (*)();
using FnPlayerFromFrame     = void* (*)(void*);
using FnCfgDefaultInit      = void* (*)(void*);
using FnCfgDtor             = void  (*)(void*);
using FnCfgParseBoolPair    = void  (*)(const char*, std::uint8_t*, std::uint8_t*);
using FnAirStringAssign     = void  (*)(void*, const char*, std::size_t);
using FnAllocSmall          = void* (*)(std::size_t, int);
using FnAllocLocked         = void* (*)(void*, std::size_t, int);
using FnSocketTransportCtor = void* (*)(void*, void*, const char*, std::uint32_t);
using FnTelemetryCtor       = void* (*)(void*, void*);
using FnTelemetryBind       = void  (*)(void*);
using FnPlayerTelemetryCtor = void* (*)(void*, void*, void*, void*);
using FnSamplerCtor         = void* (*)(void*, void*);
using FnPopulateNameMaps    = void  (*)(void*);
using FnRegisterSamplerMeta = void  (*)(void*, void*);

#else // x86

// `__thiscall` on a free-function pointer is an MSVC extension; the compiler
// knows to put the first arg in ECX and leave the rest on the stack. Callees
// clean with `ret N`. For `__cdecl`, caller cleans. For `__fastcall`, first
// two args in ECX/EDX and callee cleans.
using FnGetActiveFrame      = void* (__cdecl   *)();
using FnPlayerFromFrame     = void* (__thiscall*)(void*);
using FnCfgDefaultInit      = void* (__thiscall*)(void*);
using FnCfgDtor             = void  (__thiscall*)(void*);
using FnCfgParseBoolPair    = void  (__cdecl   *)(const char*, std::uint8_t*, std::uint8_t*);
using FnAirStringAssign     = void  (__thiscall*)(void*, const char*, std::size_t);
using FnAllocSmall          = void* (__cdecl   *)(std::size_t, int);
using FnAllocLocked         = void* (__fastcall*)(void*, std::size_t, int);
using FnSocketTransportCtor = void* (__thiscall*)(void*, void*, const char*, std::uint32_t);
using FnTelemetryCtor       = void* (__thiscall*)(void*, void*);
using FnTelemetryBind       = void  (__thiscall*)(void*);           // thunk at +0x389f78 fronts the real body with an extra push 1
using FnPlayerTelemetryCtor = void* (__thiscall*)(void*, void*, void*, void*);
using FnSamplerCtor         = void* (__thiscall*)(void*, void*);
using FnPopulateNameMaps    = void  (__thiscall*)(void*);
using FnRegisterSamplerMeta = void  (__thiscall*)(void*, void*);

#endif

struct AirRuntimeFns {
    FnGetActiveFrame      getActiveFrame      = nullptr;
    FnPlayerFromFrame     playerFromFrame     = nullptr;
    FnCfgDefaultInit      cfgDefaultInit      = nullptr;
    FnCfgDtor             cfgDtor             = nullptr;
    FnCfgParseBoolPair    cfgParseBoolPair    = nullptr;
    FnAirStringAssign     airStringAssign     = nullptr;
    FnAllocSmall          allocSmall          = nullptr;
    FnAllocLocked         allocLocked         = nullptr;
    FnSocketTransportCtor socketTransportCtor = nullptr;
    FnTelemetryCtor       telemetryCtor       = nullptr;
    FnTelemetryBind       telemetryBind       = nullptr;
    FnPlayerTelemetryCtor playerTelemetryCtor = nullptr;
    FnSamplerCtor         samplerCtor         = nullptr;
    FnPopulateNameMaps    populateNameMaps    = nullptr;
    FnRegisterSamplerMeta registerSamplerMeta = nullptr;
    FnSamplerCtor         basicSamplerCtor    = nullptr;  // TelemetrySampler (no memory tracking)
};

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
        air_base_ = 0;
        return false;
    }

    auto resolve = [&](std::uint32_t rva) -> void* {
        return reinterpret_cast<void*>(air_base_ + rva);
    };

    g_fns.getActiveFrame      = reinterpret_cast<FnGetActiveFrame>     (resolve(air_rvas::kRvaFreGetActiveFrame));
    g_fns.playerFromFrame     = reinterpret_cast<FnPlayerFromFrame>    (resolve(air_rvas::kRvaFrePlayerFromFrame));
    g_fns.cfgDefaultInit      = reinterpret_cast<FnCfgDefaultInit>     (resolve(air_rvas::kRvaTelemetryConfigDefaultInit));
    g_fns.cfgDtor             = reinterpret_cast<FnCfgDtor>            (resolve(air_rvas::kRvaTelemetryConfigDtor));
    g_fns.cfgParseBoolPair    = reinterpret_cast<FnCfgParseBoolPair>   (resolve(air_rvas::kRvaTelemetryConfigParseBoolPair));
    g_fns.airStringAssign     = reinterpret_cast<FnAirStringAssign>    (resolve(air_rvas::kRvaAirStringAssign));
    g_fns.allocSmall          = reinterpret_cast<FnAllocSmall>         (resolve(air_rvas::kRvaMMgcAllocSmall));
    g_fns.allocLocked         = reinterpret_cast<FnAllocLocked>        (resolve(air_rvas::kRvaMMgcAllocLocked));
    g_fns.socketTransportCtor = reinterpret_cast<FnSocketTransportCtor>(resolve(air_rvas::kRvaSocketTransportCtor));
    g_fns.telemetryCtor       = reinterpret_cast<FnTelemetryCtor>      (resolve(air_rvas::kRvaTelemetryCtor));
    g_fns.telemetryBind       = reinterpret_cast<FnTelemetryBind>      (resolve(air_rvas::kRvaTelemetryBindTransport));
    g_fns.playerTelemetryCtor = reinterpret_cast<FnPlayerTelemetryCtor>(resolve(air_rvas::kRvaPlayerTelemetryCtor));
    if (air_rvas::kMemoryTelemetrySamplerSize != 0) {
        g_fns.samplerCtor     = reinterpret_cast<FnSamplerCtor>        (resolve(air_rvas::kRvaMemoryTelemetrySamplerCtor));
        g_fns.basicSamplerCtor = reinterpret_cast<FnSamplerCtor>       (resolve(air_rvas::kRvaTelemetrySamplerCtor));
        g_fns.populateNameMaps = reinterpret_cast<FnPopulateNameMaps>  (resolve(air_rvas::kRvaPlayerPopulateNameMaps));
        g_fns.registerSamplerMeta = reinterpret_cast<FnRegisterSamplerMeta>(resolve(air_rvas::kRvaAvmCoreRegisterSamplerMeta));
    }

    psw_vtable_addr_ = air_base_ + air_rvas::kRvaVtPlatformSocketWrapper;

    initialized_.store(true, std::memory_order_release);
    return true;
}

// -------------------------------------------------------------------------
// tryCapturePlayer  — shared x64/x86 path via the TLS-backed FRE helper.
// -------------------------------------------------------------------------

static bool looks_like_player(void* p, std::uintptr_t air_base) {
    if (p == nullptr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const auto need = reinterpret_cast<std::uintptr_t>(p) +
                      air_rvas::kPlayerOffsetPlayerTelemetry + sizeof(void*);
    const auto end  = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (need > end) return false;
    if ((mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                        PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                        PAGE_EXECUTE_WRITECOPY)) == 0) return false;

    // A real Player has its vtable in the Adobe AIR.dll module. If *p
    // (the vtable pointer) doesn't fall inside the AIR module's image,
    // we captured something other than Player (a MethodInfo, a PoolObject,
    // or some intermediate struct that happens to have readable memory at
    // the Player-telemetry offsets).
    HMODULE air = GetModuleHandleA(air_rvas::kDllName);
    if (air == nullptr) return false;
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), air, &mi, sizeof(mi))) return false;
    const auto air_start = reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll);
    const auto air_end   = air_start + mi.SizeOfImage;

    std::uintptr_t vt = 0;
    __try {
        vt = *reinterpret_cast<std::uintptr_t*>(p);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (vt < air_start || vt >= air_end) return false;

    (void)air_base; // parameter kept for symmetry; vtable check uses GetModuleHandle
    return true;
}

void* WindowsAirRuntime::tryCapturePlayer(void* /*fre_context*/) {
    if (auto p = player_.load(std::memory_order_acquire); p != nullptr) return p;
    if (!initialized_.load(std::memory_order_acquire)) {
        if (!initialize()) return nullptr;
    }

#if defined(_M_X64) || defined(__x86_64__)
    // x64: TLS-backed FRE frame accessor → 4-deref chain → Player*.
    if (g_fns.getActiveFrame == nullptr || g_fns.playerFromFrame == nullptr) return nullptr;

    void* frame = nullptr;
    __try {
        frame = g_fns.getActiveFrame();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (frame == nullptr) return nullptr;

    void* player = nullptr;
    __try {
        player = g_fns.playerFromFrame(frame);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (!looks_like_player(player, air_base_)) return nullptr;

    player_.store(player, std::memory_order_release);
    return player;
#else
    // ---- x86 ----
    //
    // The FRE-frame-stack accessor is inlined by MSVC on this build, and
    // the existing FRE::Player_from_Frame at RVA 0x45fe80 ends at +0x294
    // which is NOT Player (it's a sibling field, probably Stage3D). We
    // bypass both and synthesise the chain in pure C++.
    //
    // Chain derived from 24 identical call-sites in .text (static analysis
    // cross-validated by 111 Player-method identifications):
    //   frame  = <peek top of FRE framestack stored in TLS slot>
    //   Player = *(*(*(*(frame + 0x08) + 0x14) + 0x04) + 0x04)
    //
    // The TLS slot index is held in a DWORD at RVA 0x00e13464, filled
    // once by TlsAlloc during AIR.dll init. We read it fresh every call
    // so a zero/uninitialised value simply fails the capture instead of
    // dereferencing an invalid slot.

    const auto* tls_idx_slot = reinterpret_cast<const volatile DWORD*>(
        air_base_ + air_rvas::kRvaFreFrameStackTlsIdx);
    const DWORD tls_idx = *tls_idx_slot;
    if (tls_idx == 0 || tls_idx == TLS_OUT_OF_INDEXES) return nullptr;

    void* frame_stack = nullptr;
    __try {
        frame_stack = ::TlsGetValue(tls_idx);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (frame_stack == nullptr) return nullptr;

    // FRE frame-stack struct:
    //   +0x04  void**  array
    //   +0x08  int32_t count
    // We peek array[count - 1] — safe to call from inside an FREFunction
    // because count >= 1 (we're in the middle of an AS3→native call).
    void* frame = nullptr;
    __try {
        auto* array = *reinterpret_cast<void* const**>(
            reinterpret_cast<char*>(frame_stack) + air_rvas::kFreFrameStackOffArray);
        const std::int32_t count = *reinterpret_cast<const std::int32_t*>(
            reinterpret_cast<char*>(frame_stack) + air_rvas::kFreFrameStackOffCount);
        if (count <= 0 || array == nullptr) return nullptr;
        frame = array[count - 1];
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (frame == nullptr) return nullptr;

    // Walk frame → step1 → step2 → step3 → Player.
    void* player = nullptr;
    __try {
        const auto* p0 = static_cast<char*>(frame);
        const auto* p1 = *reinterpret_cast<char* const*>(p0 + air_rvas::kFramePlayerChainStep1);
        diag_chain_frame_.store(reinterpret_cast<std::uintptr_t>(frame), std::memory_order_release);
        diag_chain_step1_.store(reinterpret_cast<std::uintptr_t>(p1), std::memory_order_release);
        if (p1 == nullptr) return nullptr;
        const auto* p2 = *reinterpret_cast<char* const*>(p1 + air_rvas::kFramePlayerChainStep2);
        diag_chain_step2_.store(reinterpret_cast<std::uintptr_t>(p2), std::memory_order_release);
        if (p2 == nullptr) return nullptr;
        const auto* p3 = *reinterpret_cast<char* const*>(p2 + air_rvas::kFramePlayerChainStep3);
        diag_chain_step3_.store(reinterpret_cast<std::uintptr_t>(p3), std::memory_order_release);
        if (p3 == nullptr) return nullptr;
        player = *reinterpret_cast<void* const*>(p3 + air_rvas::kFramePlayerChainStep4);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }

    // Read the candidate's vtable pointer for diagnostic purposes.
    __try {
        void* vt = *reinterpret_cast<void* const*>(player);
        diag_player_vtable_.store(reinterpret_cast<std::uintptr_t>(vt), std::memory_order_release);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // player wasn't readable at all
        return nullptr;
    }

    if (!looks_like_player(player, air_base_)) return nullptr;

    player_.store(player, std::memory_order_release);
    return player;
#endif
}

// -------------------------------------------------------------------------
// forceEnableTelemetry / forceDisableTelemetry
//
// Both arches follow the same 6-step playbook — only the struct layout,
// allocation sizes, Player offsets and GCHeap layout differ. We keep the
// two implementations side by side instead of trying to template them,
// because the byte-level struct writes are easier to audit in explicit
// form.
// -------------------------------------------------------------------------

#if defined(_M_X64) || defined(__x86_64__)

bool WindowsAirRuntime::forceEnableTelemetry(const std::string& host,
                                             std::uint32_t port,
                                             const ProfilerFeatures& f) {
    if (!initialized_.load(std::memory_order_acquire)) {
        last_error_.store(Error::NotInitialized); return false;
    }
    void* player = tryCapturePlayer();
    if (player == nullptr) {
        last_error_.store(Error::PlayerNull); return false;
    }
    if (host.empty() || port == 0) {
        last_error_.store(Error::BadHostOrPort); return false;
    }

    auto** playerTransportSlot  = reinterpret_cast<void**>(
        reinterpret_cast<char*>(player) + air_rvas::kPlayerOffsetSocketTransport);
    auto** playerTelemetrySlot  = reinterpret_cast<void**>(
        reinterpret_cast<char*>(player) + air_rvas::kPlayerOffsetTelemetry);
    auto** playerPtelemetrySlot = reinterpret_cast<void**>(
        reinterpret_cast<char*>(player) + air_rvas::kPlayerOffsetPlayerTelemetry);

    if (*playerTransportSlot != nullptr || *playerTelemetrySlot != nullptr ||
        *playerPtelemetrySlot != nullptr) {
        last_error_.store(Error::AlreadyEnabled); return false;
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
    if (transport == nullptr) {
        last_error_.store(Error::AllocSocketTransportFail);
        g_fns.cfgDtor(cfg_mem); return false;
    }
    g_fns.socketTransportCtor(transport, player, host.c_str(), port);

    void* telemetry = g_fns.allocSmall(air_rvas::kTelemetrySize, 1);
    if (telemetry == nullptr) {
        last_error_.store(Error::AllocTelemetryFail);
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
        last_error_.store(Error::AllocPlayerTelemetryFail);
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

#else // x86

bool WindowsAirRuntime::forceEnableTelemetry(const std::string& host,
                                             std::uint32_t port,
                                             const ProfilerFeatures& f) {
    if (!initialized_.load(std::memory_order_acquire)) {
        last_error_.store(Error::NotInitialized); return false;
    }
    void* player = tryCapturePlayer();
    if (player == nullptr) {
        last_error_.store(Error::PlayerNull); return false;
    }
    if (host.empty() || port == 0) {
        last_error_.store(Error::BadHostOrPort); return false;
    }

    auto** playerTransportSlot  = reinterpret_cast<void**>(
        reinterpret_cast<char*>(player) + air_rvas::kPlayerOffsetSocketTransport);
    auto** playerTelemetrySlot  = reinterpret_cast<void**>(
        reinterpret_cast<char*>(player) + air_rvas::kPlayerOffsetTelemetry);
    auto** playerPtelemetrySlot = reinterpret_cast<void**>(
        reinterpret_cast<char*>(player) + air_rvas::kPlayerOffsetPlayerTelemetry);

    diag_slot_transport_.store(reinterpret_cast<std::uintptr_t>(*playerTransportSlot),
                                std::memory_order_release);
    diag_slot_telemetry_.store(reinterpret_cast<std::uintptr_t>(*playerTelemetrySlot),
                                std::memory_order_release);
    diag_slot_playertel_.store(reinterpret_cast<std::uintptr_t>(*playerPtelemetrySlot),
                                std::memory_order_release);
    if (*playerTransportSlot != nullptr || *playerTelemetrySlot != nullptr ||
        *playerPtelemetrySlot != nullptr) {
        last_error_.store(Error::AlreadyEnabled); return false;
    }

    // x86 TelemetryConfig is 0x28 B — same 8 bool byte fields (0x00..0x07)
    // and u32 threshold at +0x08 as x64, but AirString is 0x0C and host
    // sits at +0x0C with port at +0x18.
    alignas(4) std::uint8_t cfg_mem[air_rvas::kTelemetryConfigSize];
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

    // SocketTransport — 0x20 bytes on x86.
    void* transport = g_fns.allocSmall(air_rvas::kSocketTransportSize, 1);
    if (transport == nullptr) {
        last_error_.store(Error::AllocSocketTransportFail);
        g_fns.cfgDtor(cfg_mem); return false;
    }
    g_fns.socketTransportCtor(transport, player, host.c_str(), port);

    // Telemetry — 0xB0 bytes on x86.
    void* telemetry = g_fns.allocSmall(air_rvas::kTelemetrySize, 1);
    if (telemetry == nullptr) {
        last_error_.store(Error::AllocTelemetryFail);
        g_fns.cfgDtor(cfg_mem);
        return false;
    }
    g_fns.telemetryCtor(telemetry, transport);
    g_fns.telemetryBind(telemetry);

    // PlayerTelemetry — 0x1C8 bytes via the GCHeap-locked allocator.
    // GCHeap singleton sits at RVA 0xe08570 on x86; the spinlock is at
    // heap+0x670 (vs heap+0xB98 on x64) and alloc_locked is __fastcall
    // (ecx=heap, edx=size, stack=flags).
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
        *reinterpret_cast<std::uint32_t*>(reinterpret_cast<char*>(gcheap) +
                                           air_rvas::kGcHeapLastAllocSizeOffset) =
                                               static_cast<std::uint32_t>(air_rvas::kPlayerTelemetrySize);
        lock_slot->store(0, std::memory_order_release);
    } else {
        playerTelemetry = g_fns.allocSmall(air_rvas::kPlayerTelemetrySize, 1);
    }
    if (playerTelemetry == nullptr) {
        last_error_.store(Error::AllocPlayerTelemetryFail);
        g_fns.cfgDtor(cfg_mem);
        return false;
    }

    g_fns.playerTelemetryCtor(playerTelemetry, player, telemetry, cfg_mem);

    // Player offsets on x86: SocketTransport @ +0xDB8, Telemetry @ +0xDBC,
    // PlayerTelemetry @ +0xDC0. transport->player_telemetry at +0x1C
    // (vs +0x38 on x64 — half because of the 4-byte pointer size).
    *playerTransportSlot  = transport;
    *playerTelemetrySlot  = telemetry;
    *playerPtelemetrySlot = playerTelemetry;
    *reinterpret_cast<void**>(reinterpret_cast<char*>(transport) +
                               air_rvas::kSocketTransportOffsetPtel) = playerTelemetry;

    // --- Sampler install --------------------------------------------------
    //
    // AvmCore::ctor normally instantiates a MemoryTelemetrySampler conditionally
    // on `[player+0xDC0] != null` (i.e. PlayerTelemetry already exists). In
    // Mode B that gate always fails because AvmCore construction happens
    // before the ANE gets a chance to force-enable. We replay the tiny
    // sub-sequence ourselves: alloc + ctor + store at AvmCore+0x5F4 + attach.
    //
    // AvmCore is `step3` from tryCapturePlayer's chain walk. It's cached in
    // diag_chain_step3_ by that function — which has already run (we depend
    // on `player` above, also produced by tryCapturePlayer).
    //
    // Failure here is non-fatal: the transport/telemetry/ptel trio is already
    // wired up, so Scout receives the non-sampler streams just as before.
    // The only consequence of sampler-install failure is an empty "Top
    // Activities" panel in Scout.
    void* avmcore = reinterpret_cast<void*>(diag_chain_step3_.load(std::memory_order_acquire));
    if (avmcore != nullptr && g_fns.samplerCtor != nullptr &&
        *reinterpret_cast<void**>(reinterpret_cast<char*>(avmcore) +
                                   air_rvas::kAvmCoreOffsetSamplerSlot) == nullptr)
    {
        // AvmCore::ctor logic chooses MemoryTelemetrySampler when memory-
        // allocation traces are requested, or plain TelemetrySampler
        // otherwise. We mirror that choice.
        const bool use_mts = f.script_object_allocation_traces ||
                             f.all_gc_allocation_traces;
        const std::size_t sampler_size = use_mts
            ? air_rvas::kMemoryTelemetrySamplerSize
            : air_rvas::kTelemetrySamplerSize;
        auto* ctor_fn = use_mts ? g_fns.samplerCtor : g_fns.basicSamplerCtor;
        void* sampler = (ctor_fn != nullptr)
            ? g_fns.allocSmall(sampler_size, 0)
            : nullptr;
        (void)ctor_fn;  // silence unused warning when x64 stubs are zero
        if (sampler != nullptr) {
            __try {
                ctor_fn(sampler, player);
                // AvmCore::attachSampler (inlined — body at RVA 0xa2ca4 is 3 writes).
                auto* av = reinterpret_cast<char*>(avmcore);
                auto* pl = reinterpret_cast<char*>(player);
                *reinterpret_cast<void**>(av + air_rvas::kAvmCoreOffsetSamplerSlot)     = sampler;
                *reinterpret_cast<void**>(av + air_rvas::kAvmCoreOffsetSamplerAttached) = sampler;
                *reinterpret_cast<std::uint32_t*>(av + air_rvas::kAvmCoreOffsetSamplerDirty) = 0;
                *reinterpret_cast<std::uint8_t*>(av + air_rvas::kAvmCoreOffsetSamplerActive) = 1;
                // PlayerTelemetry::attachAvmCore (inlined — RVA 0x39482e: one byte write).
                *reinterpret_cast<std::uint8_t*>(
                    reinterpret_cast<char*>(playerTelemetry) +
                    air_rvas::kPlayerTelemetryOffsetSamplerOn) = 1;
                // Populate method-name maps and propagate to AvmCore. This creates
                // the per-method lookup tables the sampler needs for `.sampler.sample`
                // records to carry AS3 function names instead of raw addresses.
                // Idempotent — player+0xDD5 guards re-allocation.
                if (g_fns.populateNameMaps != nullptr) {
                    g_fns.populateNameMaps(player);
                }
                *reinterpret_cast<void**>(av + air_rvas::kAvmCoreOffsetNameMapB) =
                    *reinterpret_cast<void**>(pl + air_rvas::kPlayerOffsetNameMapB);
                *reinterpret_cast<void**>(av + air_rvas::kAvmCoreOffsetNameMapA) =
                    *reinterpret_cast<void**>(pl + air_rvas::kPlayerOffsetNameMapA);
                // AvmCore::registerSamplerMeta wires up sampler-related rate
                // counters (avmcore+0xEC/F0/F4). Takes (avmcore, player_addr+0x176).
                // The +0x20+0x176 offset below mirrors the natural AvmCore::ctor
                // call site; the arg resolves to a small struct in the player.
                if (g_fns.registerSamplerMeta != nullptr) {
                    auto* player_x20 = *reinterpret_cast<char**>(pl + 0x20);
                    if (player_x20 != nullptr) {
                        g_fns.registerSamplerMeta(avmcore, player_x20 + 0x176);
                    }
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                // Sampler install failed — continue without it. Non-fatal.
            }
        }
    }

    g_fns.cfgDtor(cfg_mem);
    return true;
}

#endif

// -------------------------------------------------------------------------
// forceDisableTelemetry — same on both archs: null the three Player slots
// and let MMgc reclaim the trio. See the comment in this function for why
// we intentionally skip the C++ destructors.
// -------------------------------------------------------------------------

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

} // namespace ane::profiler
