// Windows concrete impl of IRuntimeHook.
//
// Uses an IAT (Import Address Table) hook on `Adobe AIR.dll`'s import of
// `ws2_32!send` (ordinal #19). The actual IAT slot address is *looked up
// at runtime* by walking the PE Import Directory — so the same code works
// for x86 and x64 without version-specific RVA tables, and it survives
// any future AIR.dll rebuild that doesn't change the fact that it imports
// ws2_32.dll!send.
//
// Lifecycle:
//   1. AIR process starts, Adobe AIR.dll is mapped at some base.
//   2. ANE gets initialized (when the AS3 app first uses the extension).
//   3. AS3 calls Profiler.init() -> FRE bridge -> IRuntimeHook::install().
//   4. install() finds Adobe AIR.dll, locates the ws2_32!send IAT slot,
//      VirtualProtects it RW, swaps in our hook, restores protection.
//   5. Every subsequent call to ws2_32!send by Adobe AIR.dll goes through
//      our hook. The hook forwards to the real send() and then, if the
//      controller is Recording, pushes the same bytes into the ring.
//   6. uninstall() restores the original IAT pointer before ANE unloads.

#ifndef ANE_PROFILER_WINDOWS_RUNTIME_HOOK_HPP
#define ANE_PROFILER_WINDOWS_RUNTIME_HOOK_HPP

#include "IRuntimeHook.hpp"

namespace ane::profiler {

class CaptureController;

class WindowsRuntimeHook : public IRuntimeHook {
public:
    WindowsRuntimeHook() = default;
    ~WindowsRuntimeHook() override;

    bool install(CaptureController* controller) override;
    void uninstall() override;
    bool installed() const noexcept override { return installed_; }

private:
    bool        installed_      = false;
    void**      iat_slot_       = nullptr;  // address of the IAT entry we patched
    void*       original_send_  = nullptr;  // backup of the pointer we replaced
};

} // namespace ane::profiler

#endif // ANE_PROFILER_WINDOWS_RUNTIME_HOOK_HPP
