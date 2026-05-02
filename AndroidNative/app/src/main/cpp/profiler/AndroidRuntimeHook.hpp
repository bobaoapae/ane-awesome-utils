// Android concrete impl of IRuntimeHook.
//
// Mirrors WindowsRuntimeHook but uses inline hooks via shadowhook (instead
// of IAT patching) on bionic libc's `connect`, `send`, `sendto` and `close`.
// shadowhook patches the symbols globally — we filter callers in the proxy
// by checking that the immediate caller PC lies inside `libCore.so`'s
// loaded address range, the Adobe AIR runtime on Android.
//
// The lifecycle and per-socket bookkeeping are identical to the Windows
// impl: `connect` marks sockets peered to 127.0.0.1:<telemetry_port>;
// `send`/`sendto` capture bytes only on monitored sockets; `close` unmarks.
// Unmonitored sockets pass through with negligible overhead.
//
// Why this approach over PLT hooking: bytehook's PLT-mode partial filter
// (caller_path) requires the AAR to be on Maven, which it no longer is —
// only shadowhook 2.x is published. shadowhook is global by design but
// the libCore.so caller filter we apply per-call gives equivalent
// selectivity for the Scout TCP byte tap.
//
// Why not hook libc.so directly via shadowhook_hook_sym_name(libc, send):
// works for both AIR and non-AIR callers; the dladdr call inside the
// proxy adds ~50 ns/call but ensures correctness without RA on libCore.so.

#ifndef ANE_PROFILER_ANDROID_RUNTIME_HOOK_HPP
#define ANE_PROFILER_ANDROID_RUNTIME_HOOK_HPP

#include <atomic>
#include <cstdint>

#include "IRuntimeHook.hpp"

namespace ane::profiler {

class CaptureController;

class AndroidRuntimeHook : public IRuntimeHook {
public:
    AndroidRuntimeHook() = default;
    ~AndroidRuntimeHook() override;

    bool install(CaptureController* controller,
                 std::uint16_t telemetry_port = 0) override;
    void uninstall() override;
    bool installed() const noexcept override { return installed_; }

    // Diagnostic counters — same shape as Windows so the AS3-side bindings
    // stay symmetric.
    std::uint64_t diagSendCalls()       const;
    std::uint64_t diagSendCaptured()    const;
    std::uint64_t diagConnectCalls()    const;
    std::uint64_t diagConnectMatched()  const;
    std::uint64_t diagCloseCalls()      const;

private:
    bool installed_ = false;
};

} // namespace ane::profiler

#endif // ANE_PROFILER_ANDROID_RUNTIME_HOOK_HPP
