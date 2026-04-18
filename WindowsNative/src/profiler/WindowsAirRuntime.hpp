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

private:
    std::uintptr_t       air_base_        = 0;
    std::atomic<bool>    initialized_{false};
    std::atomic<void*>   player_{nullptr};
    std::uintptr_t       psw_vtable_addr_ = 0;
};

} // namespace ane::profiler

#endif // ANE_PROFILER_WINDOWS_AIR_RUNTIME_HPP
