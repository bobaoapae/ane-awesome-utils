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

// FRE helpers ---------------------------------------------------------------
// getActiveFrame_TLS in x86 is located by following the first `call` of the
// exported FREGetContextNativeData / FREGetContextActionScriptData — both
// dispatch through this helper to pick up the current FRE frame from the
// TLS slot (`DAT_10e13464` → TlsGetValue). The function peeks the top of
// the frame stack via a weird pop-then-push pattern but is otherwise
// semantically identical to the x64 helper. __cdecl, returns in eax.
inline constexpr std::uint32_t kRvaFreGetActiveFrame            = 0x00458600;
inline constexpr std::uint32_t kRvaFrePlayerFromFrame           = 0x0045fe80;  // __thiscall
inline constexpr std::uint32_t kRvaFreContextToInternal         = 0x00000000;  // not needed once getActiveFrame works

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
