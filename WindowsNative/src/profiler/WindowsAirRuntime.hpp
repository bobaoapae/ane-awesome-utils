// WindowsAirRuntime — binding to Adobe AIR.dll 51.1.3.10 (x64 + x86)
// entry points that we call from the ANE to force-enable telemetry on
// demand without a `.telemetry.cfg` on disk (Mode B real).
//
// Implementation details (function-pointer typedefs, per-arch calling
// conventions, cached resolver table) live in the .cpp — the header keeps
// only the public surface and opaque state.
//
// Reference:
//   docs/profiler-mode-b-plan.md       (x64)
//   docs/profiler-mode-b-plan-x86.md   (x86, best-effort)

#ifndef ANE_PROFILER_WINDOWS_AIR_RUNTIME_HPP
#define ANE_PROFILER_WINDOWS_AIR_RUNTIME_HPP

#include "ProfilerFeatures.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ane::profiler {

class WindowsAirRuntime {
public:
    WindowsAirRuntime() = default;

    // Resolve the Adobe AIR.dll module base, verify its init_telemetry
    // prologue signature matches the build we were compiled against, and
    // cache every function pointer we'll call. Safe to call multiple times
    // (subsequent calls short-circuit after the first success). Returns
    // false if AIR.dll is not loaded or its signature doesn't match.
    bool initialize();

    // Obtain the Player pointer. On x64 this reads TLS directly; on x86 the
    // equivalent TLS helper was inlined by MSVC, so `fre_context` is used
    // to walk a heuristic indirection chain. Must be called on the AS3
    // thread during a live FREFunction dispatch. Caches the value on the
    // first success.
    void* tryCapturePlayer(void* fre_context = nullptr);

    void*          player()      const { return player_.load(std::memory_order_acquire); }
    std::uintptr_t airBase()     const { return air_base_; }
    bool           initialized() const { return initialized_.load(std::memory_order_acquire); }

    // Force-enable the telemetry subsystem. Replays the alloc + ctor
    // sequence that Player::init_telemetry would run if `.telemetry.cfg`
    // had pointed at `host:port`. `features` controls the capture flags.
    //
    // Must be called AFTER tryCapturePlayer() has succeeded.
    //
    // On x86 this currently returns false — Mode B there is not yet
    // wired up pending GCHeap-layout validation. Mode A via `.telemetry.cfg`
    // still works.
    bool forceEnableTelemetry(const std::string&   host,
                              std::uint32_t        port,
                              const ProfilerFeatures& features);

    // Destroy the PlayerTelemetry/Telemetry/SocketTransport trio and zero
    // the Player fields. Symmetric counterpart to forceEnableTelemetry().
    // Safe to call when telemetry was never force-enabled (no-op).
    void forceDisableTelemetry();

    std::uintptr_t platformSocketWrapperVtable() const { return psw_vtable_addr_; }

    // Diagnostic: latest failure reason for forceEnableTelemetry. Cleared
    // on a successful call. Values chosen to be stable-ish for triage
    // without needing to map a C enum across language boundaries.
    enum class Error : std::uint32_t {
        Ok                       = 0,
        NotInitialized           = 1,
        PlayerNull               = 2,
        BadHostOrPort            = 3,
        AlreadyEnabled           = 4,
        AllocSocketTransportFail = 5,
        AllocTelemetryFail       = 6,
        AllocPlayerTelemetryFail = 7,
        NullGcHeap               = 8,
    };
    Error lastError() const { return last_error_.load(std::memory_order_acquire); }

    // Diagnostic getters for the three Player slots the runtime populates
    // inside init_telemetry. Called from the status FREFunction to debug
    // "AlreadyEnabled" scenarios — should all be null pre-start.
    std::uintptr_t diagSlotTransport() const     { return diag_slot_transport_.load(std::memory_order_acquire); }
    std::uintptr_t diagSlotTelemetry() const     { return diag_slot_telemetry_.load(std::memory_order_acquire); }
    std::uintptr_t diagSlotPlayerTelemetry() const { return diag_slot_playertel_.load(std::memory_order_acquire); }

    // Capture chain diagnostics — each step of the FRE-frame-to-Player walk
    // on x86. All zero on x64 (we use the TLS helper directly there).
    std::uintptr_t diagChainFrame() const  { return diag_chain_frame_.load(std::memory_order_acquire); }
    std::uintptr_t diagChainStep1() const  { return diag_chain_step1_.load(std::memory_order_acquire); }
    std::uintptr_t diagChainStep2() const  { return diag_chain_step2_.load(std::memory_order_acquire); }
    std::uintptr_t diagChainStep3() const  { return diag_chain_step3_.load(std::memory_order_acquire); }

    // Vtable pointer at *(player) — a valid Player has its vtable in
    // Adobe AIR.dll's .rdata section. Non-.rdata value means we captured
    // something that isn't Player.
    std::uintptr_t diagPlayerVtable() const { return diag_player_vtable_.load(std::memory_order_acquire); }

private:
    std::uintptr_t       air_base_        = 0;
    std::atomic<bool>    initialized_{false};
    std::atomic<void*>   player_{nullptr};
    std::uintptr_t       psw_vtable_addr_ = 0;
    std::atomic<Error>         last_error_{Error::Ok};
    std::atomic<std::uintptr_t> diag_slot_transport_{0};
    std::atomic<std::uintptr_t> diag_slot_telemetry_{0};
    std::atomic<std::uintptr_t> diag_slot_playertel_{0};
    std::atomic<std::uintptr_t> diag_chain_frame_{0};
    std::atomic<std::uintptr_t> diag_chain_step1_{0};
    std::atomic<std::uintptr_t> diag_chain_step2_{0};
    std::atomic<std::uintptr_t> diag_chain_step3_{0};
    std::atomic<std::uintptr_t> diag_player_vtable_{0};
};

} // namespace ane::profiler

#endif // ANE_PROFILER_WINDOWS_AIR_RUNTIME_HPP
