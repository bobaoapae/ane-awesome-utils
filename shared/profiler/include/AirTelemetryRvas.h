// SPDX-License-Identifier: MIT
//
// RVAs estáticos extraídos de Adobe AIR.dll 51.1.3.10 (Windows x64 e x86).
//
// Fontes:
//   docs/profiler-rva-51-1-3-10.md       (x64 base)
//   docs/profiler-mode-b-plan.md         (x64 Mode B)
//   docs/profiler-mode-b-plan-x86.md     (x86 Mode B)
//
// Todos os RVAs são relativos à image base:
//   x64: tipicamente 0x180000000
//   x86: tipicamente 0x10000000
//
// IMPORTANTE: estes arquivos são específicos para a build 51.1.3.10. Outras
// versões do runtime terão RVAs diferentes — não portáveis.

#ifndef ANE_PROFILER_AIR_TELEMETRY_RVAS_H
#define ANE_PROFILER_AIR_TELEMETRY_RVAS_H

#include <cstddef>
#include <cstdint>

namespace ane::profiler::air_51_1_3_10_win_x64 {

// ---------------------------------------------------------------------------
// Binary identity
// ---------------------------------------------------------------------------
inline constexpr const char* kDllName    = "Adobe AIR.dll";
inline constexpr const char* kAirVersion = "51.1.3.10";
inline constexpr const char* kSha256     =
    "e24a635554dba434d2cd08ab5b76d0453787a947d0f4a2291e8f0cae9459d6cc";
inline constexpr std::uint64_t kExpectedImageBase = 0x180000000ULL;
inline constexpr std::uint64_t kDllSizeBytes      = 19977944ULL;

// ---------------------------------------------------------------------------
// Hook entry points
// ---------------------------------------------------------------------------

inline constexpr std::uint32_t kRvaPlayerInitTelemetry = 0x001e96d0;

// IAT slot for `ws2_32!send` (ordinal #19) inside Adobe AIR.dll.
inline constexpr std::uint32_t kRvaIatWs2Send = 0x00b05630;

inline constexpr std::uint32_t kRvaSocketTransportSendBytes      = 0x00493060;
inline constexpr std::uint32_t kRvaSocketTransportSendBytesThunk = 0x00492f50;

// ---------------------------------------------------------------------------
// Vtables (em .rdata)
// ---------------------------------------------------------------------------

inline constexpr std::uint32_t kRvaVtPlatformSocketWrapper      = 0x00ecb828;
inline constexpr std::uint32_t kVtOffsetSendBytes               = 0x58; // slot 11
inline constexpr std::uint32_t kVtOffsetConnect                 = 0x38; // slot 7
inline constexpr std::uint32_t kRvaVtSocketTransport            = 0x00ecb7d0;
inline constexpr std::uint32_t kRvaVtPlayerTelemetry            = 0x00ec95e0;

// ---------------------------------------------------------------------------
// Construtores/destrutores
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kRvaTelemetryCtor                = 0x004852c0;
inline constexpr std::uint32_t kRvaTelemetryDtor                = 0x00485604;
inline constexpr std::uint32_t kRvaTelemetryBindTransport       = 0x004864ec;
inline constexpr std::uint32_t kRvaPlayerTelemetryCtor          = 0x0048e4a0;
inline constexpr std::uint32_t kRvaPlayerTelemetryDtor          = 0x0048ef84;
inline constexpr std::uint32_t kRvaSocketTransportCtor          = 0x0048eb10;
inline constexpr std::uint32_t kRvaSocketTransportDtor          = 0x0048f118;
inline constexpr std::uint32_t kRvaSocketTransportOpen          = 0x004930d0;
inline constexpr std::uint32_t kRvaPlatformSocketWrapperCtor    = 0x0048e450;
inline constexpr std::uint32_t kRvaPlatformSocketSend           = 0x005de350;

// ---------------------------------------------------------------------------
// Modo B — invocar init manual sem depender de .telemetry.cfg
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kRvaFreGetActiveFrame        = 0x0058d390; // void*()
inline constexpr std::uint32_t kRvaFrePlayerFromFrame       = 0x002d71e8; // void*(frame*)
inline constexpr std::uint32_t kRvaFreContextToInternal     = 0x0058d3f8; // void*(FREContext)

inline constexpr std::uint32_t kRvaTelemetryConfigDefaultInit  = 0x004882c8;
inline constexpr std::uint32_t kRvaTelemetryConfigDtor         = 0x00488804;
inline constexpr std::uint32_t kRvaTelemetryConfigParseBoolPair= 0x0048c100;

inline constexpr std::uint32_t kRvaAirStringAssign  = 0x0030ec10;
inline constexpr std::uint32_t kRvaAirStringClear   = 0x0030e07c;

inline constexpr std::uint32_t kRvaMMgcAllocSmall   = 0x001a0a64;
inline constexpr std::uint32_t kRvaMMgcAllocLocked  = 0x001ab200;
inline constexpr std::uint32_t kRvaMMgcFree         = 0x001ab370;
// Hook body starts after the `test rdx,rdx; je ret` null guard so the
// trampoline only copies plain register-save instructions.
inline constexpr std::uint32_t kRvaMMgcFreeHookBody = 0x001ab379;

inline constexpr std::uint32_t kRvaIatKernel32HeapReAlloc = 0x00b049c0;
inline constexpr std::uint32_t kRvaIatKernel32HeapAlloc   = 0x00b04b70;
inline constexpr std::uint32_t kRvaIatKernel32HeapFree    = 0x00b04b68;

inline constexpr std::uint32_t kRvaGcHeapPtrSlot    = 0x01223f60;
inline constexpr std::uint32_t kGcHeapLockOffset           = 0xb98;
inline constexpr std::uint32_t kGcHeapLastAllocPtrOffset   = 0xba0;
inline constexpr std::uint32_t kGcHeapLastAllocSizeOffset  = 0xba8;

inline constexpr std::size_t   kSocketTransportSize  = 0x40;
inline constexpr std::size_t   kTelemetrySize        = 0x110;
inline constexpr std::size_t   kPlayerTelemetrySize  = 0x220;

// Config + AirString + Player offsets.
inline constexpr std::size_t   kTelemetryConfigSize      = 0x38;
inline constexpr std::uint32_t kTelemetryConfigOffHost   = 0x10;
inline constexpr std::uint32_t kTelemetryConfigOffHostLen = 0x18;  // host.len
inline constexpr std::uint32_t kTelemetryConfigOffPort   = 0x20;
inline constexpr std::uint32_t kAirStringSize            = 0x10;
inline constexpr std::uint32_t kPlayerOffsetSocketTransport  = 0x1650;
inline constexpr std::uint32_t kPlayerOffsetTelemetry        = 0x1658;
inline constexpr std::uint32_t kPlayerOffsetPlayerTelemetry  = 0x1660;
inline constexpr std::uint32_t kSocketTransportOffsetPtel    = 0x38;

// Init-telemetry prologue signature (first 12 bytes) — used to guard against
// accidentally binding to a differently-built runtime.
inline constexpr std::uint8_t  kInitTelemetryPrologue[12] = {
    0x48, 0x8B, 0xC4, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x60
};

inline constexpr std::uint32_t kRvaTelemetrySamplerCtor         = 0x004881a4;
inline constexpr std::uint32_t kRvaMemoryTelemetrySamplerCtor   = 0x00488060;
inline constexpr std::uint32_t kRvaAttachSampler                = 0x000d308c;
inline constexpr std::uint32_t kRvaGetSampler                   = 0x000d30dc;
inline constexpr std::uint32_t kRvaMethodInfoGetMethodNameWithTraits = 0x000fc5e4;
inline constexpr std::uint32_t kAvmCoreOffsetCurrentMethodFrame = 0x0060;
inline constexpr std::uint32_t kAvmCoreOffsetCallStack          = 0x0128;

// x64 sampler replay is not yet wired up — dynamic verification on x64 is
// pending. x86 is the proven path (see note in the x86 namespace). These
// placeholders keep the resolver table symmetric; forceEnableTelemetry on
// x64 currently skips the sampler step entirely.
inline constexpr std::uint32_t kRvaPlayerPopulateNameMaps       = 0;
inline constexpr std::uint32_t kRvaAvmCoreRegisterSamplerMeta   = 0;
inline constexpr std::size_t   kMemoryTelemetrySamplerSize      = 0;
inline constexpr std::size_t   kTelemetrySamplerSize            = 0;
inline constexpr std::uint32_t kAvmCoreOffsetSamplerSlot        = 0;
inline constexpr std::uint32_t kAvmCoreOffsetSamplerAttached    = 0;
inline constexpr std::uint32_t kAvmCoreOffsetSamplerDirty       = 0;
inline constexpr std::uint32_t kAvmCoreOffsetSamplerActive      = 0;
inline constexpr std::uint32_t kAvmCoreOffsetNameMapA           = 0;
inline constexpr std::uint32_t kAvmCoreOffsetNameMapB           = 0;
inline constexpr std::uint32_t kPlayerOffsetNameMapA            = 0;
inline constexpr std::uint32_t kPlayerOffsetNameMapB            = 0;
inline constexpr std::uint32_t kPlayerTelemetryOffsetSamplerOn  = 0;

} // namespace ane::profiler::air_51_1_3_10_win_x64

// =============================================================================
//                                    x86
// =============================================================================

namespace ane::profiler::air_51_1_3_10_win_x86 {

inline constexpr const char* kDllName    = "Adobe AIR.dll";
inline constexpr const char* kAirVersion = "51.1.3.10";
inline constexpr const char* kSha256     =
    "812980fcc3dff6d25abdfacc17dfd96baf83ca77f7c872aa06c5a544ba441ce0";
inline constexpr std::uint64_t kExpectedImageBase = 0x10000000ULL;
inline constexpr std::uint64_t kDllSizeBytes      = 15478488ULL;

// Hook points ---------------------------------------------------------------
inline constexpr std::uint32_t kRvaPlayerInitTelemetry = 0x0018e840;

// IAT slot for ws2_32!send. VA = 0x108c2b08, RVA = 0x8c2b08.
inline constexpr std::uint32_t kRvaIatWs2Send = 0x008c2b08;

// Vtables & hook candidates -------------------------------------------------
// On x86 the send_bytes flows through SocketTransport::vftable slot 4 (+0x10),
// NOT through PlatformSocketWrapper like x64.
inline constexpr std::uint32_t kRvaVtSocketTransport            = 0x00c2b470;
inline constexpr std::uint32_t kVtOffsetSendBytes               = 0x10; // slot 4 in ST::vftable
inline constexpr std::uint32_t kRvaVtPlatformSocketWrapper      = 0x00c2b49c;
inline constexpr std::uint32_t kRvaVtPlayerTelemetry            = 0x00c2a788;

inline constexpr std::uint32_t kRvaSocketTransportSendBytes     = 0x00393f80;

// Init sequence -------------------------------------------------------------
inline constexpr std::uint32_t kRvaSocketTransportCtor          = 0x00390501;
inline constexpr std::uint32_t kRvaTelemetryCtor                = 0x00388be7;
// bindTransport thunk `push 1; call body` is the right entry point.
inline constexpr std::uint32_t kRvaTelemetryBindTransport       = 0x00389f78;
inline constexpr std::uint32_t kRvaPlayerTelemetryCtor          = 0x0038ffc9;

// Sampler ctor — the missing piece for Scout's AS3 function-level sampling.
// AvmCore::ctor normally calls this conditionally (gated on Player+0xDC0 !=
// null, i.e. PlayerTelemetry exists). Because our Mode B enables telemetry
// LATER than AvmCore::ctor runs, the gate evaluates false at AvmCore
// construction time and the sampler is never created. We replay this block
// ourselves after installing PlayerTelemetry.
//   MTS ctor:          __thiscall(this=sampler_buffer, player)
//   populateNameMaps:  __thiscall(player)  -- idempotent, creates player+DDC/DD8
inline constexpr std::uint32_t kRvaMemoryTelemetrySamplerCtor   = 0x0038b02c;
inline constexpr std::uint32_t kRvaTelemetrySamplerCtor         = 0x0038b1ae;
inline constexpr std::uint32_t kRvaAttachSampler                = 0x000b836c;
inline constexpr std::uint32_t kRvaGetSampler                   = 0x000b83a4;
inline constexpr std::uint32_t kRvaMethodInfoGetMethodNameWithTraits = 0x000d5754;
inline constexpr std::uint32_t kAvmCoreOffsetCurrentMethodFrame = 0x0034;
inline constexpr std::uint32_t kAvmCoreOffsetCallStack          = 0x00c8;
inline constexpr std::uint32_t kRvaPlayerPopulateNameMaps       = 0x002151a9;
inline constexpr std::uint32_t kRvaAvmCoreRegisterSamplerMeta   = 0x000a8754;
inline constexpr std::size_t   kMemoryTelemetrySamplerSize      = 0x1d810;
inline constexpr std::size_t   kTelemetrySamplerSize            = 0x1d778;

// AvmCore field layout (only what we touch) --------------------------------
inline constexpr std::uint32_t kAvmCoreOffsetSamplerSlot        = 0x5F4;   // AvmCore+0x5F4 = sampler ptr
inline constexpr std::uint32_t kAvmCoreOffsetSamplerAttached    = 0x40;    // AvmCore+0x40 = sampler (attachSampler sets this)
inline constexpr std::uint32_t kAvmCoreOffsetSamplerDirty       = 0x38;    // AvmCore+0x38 = 0 at attach time
inline constexpr std::uint32_t kAvmCoreOffsetSamplerActive      = 0x3C;    // AvmCore+0x3C = (sampler!=null) byte
inline constexpr std::uint32_t kAvmCoreOffsetNameMapA           = 0xBC;    // AvmCore+0xBC = copy of player+0xDD8
inline constexpr std::uint32_t kAvmCoreOffsetNameMapB           = 0xC0;    // AvmCore+0xC0 = copy of player+0xDDC
inline constexpr std::uint32_t kPlayerOffsetNameMapA            = 0xDD8;
inline constexpr std::uint32_t kPlayerOffsetNameMapB            = 0xDDC;
inline constexpr std::uint32_t kPlayerTelemetryOffsetSamplerOn  = 0x38;    // PlayerTelemetry+0x38 = 1 after sampler attached

// Destructors (for shutdown cascade) ---------------------------------------
inline constexpr std::uint32_t kRvaSocketTransportDtor          = 0x00390501; // TODO — not yet pinned; close via vtable slot 11 thunk works
inline constexpr std::uint32_t kRvaTelemetryDtor                = 0x00388be7; // TODO — not yet pinned
inline constexpr std::uint32_t kRvaPlayerTelemetryDtor          = 0x0038ffc9; // TODO — not yet pinned

// TelemetryConfig helpers ---------------------------------------------------
inline constexpr std::uint32_t kRvaTelemetryConfigDefaultInit   = 0x0038b2a7;
inline constexpr std::uint32_t kRvaTelemetryConfigDtor          = 0x0038b729;
inline constexpr std::uint32_t kRvaTelemetryConfigHasAddress    = 0x0038e2dd;
inline constexpr std::uint32_t kRvaTelemetryConfigParseBoolPair = 0x0038e3c8;

// AirString -----------------------------------------------------------------
inline constexpr std::uint32_t kRvaAirStringAssign  = 0x0024ea9a;
inline constexpr std::uint32_t kRvaAirStringClear   = 0x0024e1ee;

// MMgc ---------------------------------------------------------------------
inline constexpr std::uint32_t kRvaMMgcAllocSmall   = 0x0014f323;   // __cdecl(size, flags)
inline constexpr std::uint32_t kRvaMMgcAllocLocked  = 0x001573de;   // __fastcall(ecx=heap, edx=size, [esp]=flags)
inline constexpr std::uint32_t kRvaMMgcFree         = 0x00157526;   // __fastcall(ecx=heap, edx=ptr)
inline constexpr std::uint32_t kRvaMMgcFreeHookBody = 0x00157526;

inline constexpr std::uint32_t kRvaIatKernel32HeapReAlloc = 0x008c22f0;
inline constexpr std::uint32_t kRvaIatKernel32HeapAlloc   = 0x008c2584;
inline constexpr std::uint32_t kRvaIatKernel32HeapFree    = 0x008c2588;

// FRE helpers ---------------------------------------------------------------
//
// Two AIR-internal helpers exist on x86 but the ANE DOES NOT use them:
//
//   - `getActiveFrame_TLS` @ 0x00458600 reads the TLS slot and pops-then-
//     pushes-back the top frame. Effectively non-destructive, but fragile
//     under concurrent GC. We reimplement the peek in pure C++ instead.
//   - `FRE::Player_from_Frame` @ 0x0045fe80 does 4 derefs ending at
//     +0x294 — which is NOT Player. (+0x294 is a sibling field, probably
//     Stage3D or similar.) The REAL Player is at +0x04 of the 3rd step.
//     24 independent call-sites in .text use the [+0x04] tail.
//
// We use FRE::getActiveFrame_TLS directly (safe enough — same pattern x64
// uses) and then apply the 4-step chain:
//   frame  = getActiveFrame_TLS()
//   Player = *(*(*(*(frame + 0x08) + 0x14) + 0x04) + 0x5E8)
//
// The +0x5E8 step4 was dynamically verified in CDB: consistent across
// 3 runs under ASLR, and corresponds to x64's +0xAC0 slot (roughly halved
// because half the fields in this AvmCore-ish struct are pointer-sized).
inline constexpr std::uint32_t kRvaFreGetActiveFrame            = 0x00458600;  // TLS-backed active-frame accessor
inline constexpr std::uint32_t kRvaFrePlayerFromFrame           = 0x0045fe80;  // WRONG OFFSET — DO NOT USE
inline constexpr std::uint32_t kRvaFreContextToInternal         = 0x00000000;

inline constexpr std::uint32_t kRvaFreFrameStackTlsIdx          = 0x00e13464;  // DWORD in .data holding the TLS slot index (reference only; we call getActiveFrame_TLS)
inline constexpr std::uint32_t kFreFrameStackOffArray           = 0x04;        // frame_stack+0x04 = void** array
inline constexpr std::uint32_t kFreFrameStackOffCount           = 0x08;        // frame_stack+0x08 = int32 count
inline constexpr std::uint32_t kFramePlayerChainStep1           = 0x08;        // frame → AbcEnv/MethodEnv
inline constexpr std::uint32_t kFramePlayerChainStep2           = 0x14;        // step1 → MethodInfo
inline constexpr std::uint32_t kFramePlayerChainStep3           = 0x04;        // step2 → PoolObject
inline constexpr std::uint32_t kFramePlayerChainStep4           = 0x5E8;       // step3 → AvmCore → Player (x86 only, dynamically verified)

// Allocator sizes (x86 = roughly half of x64) -------------------------------
inline constexpr std::size_t   kSocketTransportSize  = 0x20;
inline constexpr std::size_t   kTelemetrySize        = 0xb0;
inline constexpr std::size_t   kPlayerTelemetrySize  = 0x1c8;
inline constexpr std::size_t   kPlatformSocketWrapperSize = 0x230;

// TelemetryConfig struct layout (x86 — 0x28 bytes total) --------------------
inline constexpr std::size_t   kTelemetryConfigSize      = 0x28;
inline constexpr std::uint32_t kTelemetryConfigOffHost   = 0x0c;   // AirString host @ cfg+0x0c
inline constexpr std::uint32_t kTelemetryConfigOffHostLen = 0x10;  // host.len (has_address)
inline constexpr std::uint32_t kTelemetryConfigOffPort   = 0x18;
inline constexpr std::uint32_t kAirStringSize            = 0x0c;

// Player struct offsets (x86) ---------------------------------------------
inline constexpr std::uint32_t kPlayerOffsetSocketTransport  = 0xdb8;
inline constexpr std::uint32_t kPlayerOffsetTelemetry        = 0xdbc;
inline constexpr std::uint32_t kPlayerOffsetPlayerTelemetry  = 0xdc0;
inline constexpr std::uint32_t kSocketTransportOffsetPtel    = 0x1c;

// GCHeap -------------------------------------------------------------------
// In x86 the singleton pointer is in .data at VA 0x10e08570 (RVA 0xe08570).
inline constexpr std::uint32_t kRvaGcHeapPtrSlot           = 0x00e08570;
inline constexpr std::uint32_t kGcHeapLockOffset           = 0x670;
inline constexpr std::uint32_t kGcHeapLastAllocPtrOffset   = 0x674;
inline constexpr std::uint32_t kGcHeapLastAllocSizeOffset  = 0x678;

// Init-telemetry prologue signature (x86 first 6 bytes). Used to guard
// against wrong runtime versions. Actual prolog sets up SEH:
//   55              push ebp
//   8b ec           mov ebp, esp
//   6a ff           push -1                     (SEH cookie)
//   68 XX XX XX XX  push imm32 (handler)        (rest of prolog varies)
// We match only the first 6 bytes — enough to tell us this is
// init_telemetry on this build, without depending on the imm32 handler
// address which changes on rebase.
inline constexpr std::uint8_t  kInitTelemetryPrologue[6] = {
    0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68
};

} // namespace ane::profiler::air_51_1_3_10_win_x86

// =============================================================================
// Select the right namespace at compile time based on MSVC arch macros.
// Users of this header write e.g.:
//
//   using namespace ane::profiler::air_rvas;
//   auto rva = air_rvas::kRvaIatWs2Send;
//
// and the right set of constants is selected by the compiler.
// =============================================================================

namespace ane::profiler {
#if defined(_M_X64) || defined(__x86_64__)
    namespace air_rvas = ::ane::profiler::air_51_1_3_10_win_x64;
#elif defined(_M_IX86) || defined(__i386__)
    namespace air_rvas = ::ane::profiler::air_51_1_3_10_win_x86;
#endif
} // namespace ane::profiler

#endif // ANE_PROFILER_AIR_TELEMETRY_RVAS_H
