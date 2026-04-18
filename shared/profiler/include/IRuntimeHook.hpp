// Abstract interface for the platform-specific runtime byte tap.
//
// Each concrete impl (Windows / Android / macOS / iOS) installs a low-level
// hook that redirects the AIR runtime's outgoing Scout TCP bytes into a
// CaptureController. The hook stays installed for the life of the ANE; it
// is quiescent (just passes bytes through to the real socket) while the
// controller is Idle, and starts recording when the caller invokes
// `CaptureController::start()`.
//
// The interface does not describe HOW the hook is installed — that's left
// to the impl (IAT on Windows, PLT on Android, dyld interpose on Mac,
// inline patch on iOS static-lib). Callers just bind an instance to a
// controller via install() and forget about it.

#ifndef ANE_PROFILER_I_RUNTIME_HOOK_HPP
#define ANE_PROFILER_I_RUNTIME_HOOK_HPP

#include <memory>

namespace ane::profiler {

class CaptureController;

class IRuntimeHook {
public:
    virtual ~IRuntimeHook() = default;

    // Install the low-level hook. Wires it to `controller` which the hook
    // queries on every intercepted event. Idempotent: returns true if
    // already installed. Returns false on a hard failure (e.g. AIR runtime
    // module not loaded yet, or IAT slot not found).
    virtual bool install(CaptureController* controller) = 0;

    // Restore the original pointers and release resources. Safe to call
    // repeatedly; a no-op when not installed.
    virtual void uninstall() = 0;

    virtual bool installed() const noexcept = 0;

    // Platform factory. Returns a newly-allocated hook for the current
    // platform. Implemented per-platform in the corresponding .cpp.
    static std::unique_ptr<IRuntimeHook> create();
};

} // namespace ane::profiler

#endif // ANE_PROFILER_I_RUNTIME_HOOK_HPP
