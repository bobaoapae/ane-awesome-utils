// Windows concrete impl of IRuntimeHook.
//
// Uses IAT (Import Address Table) hooks on `Adobe AIR.dll`'s imports of
// three `ws2_32` entry points: `send` (#19), `connect` (#4), and
// `closesocket` (#3). The actual IAT slot addresses are *looked up at
// runtime* by walking the PE Import Directory — so the same code works
// for x86 and x64 without version-specific RVA tables, and survives any
// future AIR.dll rebuild that keeps importing these from ws2_32.dll.
//
// Why three hooks: `send` alone would capture every outgoing TCP byte in
// the process, including unrelated AS3 `flash.net.Socket` traffic (e.g.
// a game's own command channel on port 7936) that multiplexes with the
// Scout telemetry stream in the .flmc and breaks downstream replay. The
// connect/closesocket hooks maintain a set of sockets known to be peered
// on 127.0.0.1:<telemetry_port>; `send` only captures bytes from sockets
// in that set.
//
// Lifecycle:
//   1. AIR process starts, Adobe AIR.dll is mapped at some base.
//   2. ANE gets initialized (when the AS3 app first uses the extension).
//   3. AS3 calls Profiler.init() -> FRE bridge -> IRuntimeHook::install(
//      controller, telemetry_port).
//   4. install() finds Adobe AIR.dll, locates the three IAT slots,
//      VirtualProtects each RW, swaps in our hooks, restores protection.
//   5. Every ws2_32!connect/send/closesocket call by Adobe AIR.dll now
//      goes through the hook. connect marks qualifying sockets, send
//      filters by membership, closesocket unmarks.
//   6. uninstall() restores all three original IAT pointers and clears
//      the monitored-socket set.

#ifndef ANE_PROFILER_WINDOWS_RUNTIME_HOOK_HPP
#define ANE_PROFILER_WINDOWS_RUNTIME_HOOK_HPP

#include <atomic>
#include <cstdint>

#include "IRuntimeHook.hpp"

namespace ane::profiler {

class CaptureController;

class WindowsRuntimeHook : public IRuntimeHook {
public:
    WindowsRuntimeHook() = default;
    ~WindowsRuntimeHook() override;

    bool install(CaptureController* controller,
                 std::uint16_t telemetry_port = 0) override;
    void uninstall() override;
    bool installed() const noexcept override { return installed_; }

    // Diagnostic counters. Cumulative since install(); not reset across
    // start/stop cycles. Exposed through the bindings' status FREFunction
    // so we can tell, at runtime, whether the IAT hook is being exercised
    // at all — useful for triaging "zero bytes captured" scenarios.
    std::uint64_t diagSendCalls()        const;
    std::uint64_t diagSendCaptured()     const;
    std::uint64_t diagConnectCalls()     const;
    std::uint64_t diagConnectMatched()   const;
    std::uint64_t diagCloseCalls()       const;

private:
    bool   installed_              = false;
    void** send_slot_              = nullptr;
    void*  original_send_          = nullptr;
    void** connect_slot_           = nullptr;
    void*  original_connect_       = nullptr;
    void** closesocket_slot_       = nullptr;
    void*  original_closesocket_   = nullptr;
};

} // namespace ane::profiler

#endif // ANE_PROFILER_WINDOWS_RUNTIME_HOOK_HPP
