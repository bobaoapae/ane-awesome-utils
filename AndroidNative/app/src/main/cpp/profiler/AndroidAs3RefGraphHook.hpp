// Phase 4b — Android AS3 reference graph hook.
//
// Captures owner→dependent edges by hooking the 4 canonical AS3 reference-
// creation methods in libCore.so:
//   - DisplayObjectContainer::addChild / removeChild
//   - EventDispatcher::addEventListener / removeEventListener
//
// Each invocation records (owner, dependent, kind) and emits an
// As3Reference / As3ReferenceRemove event into DPC.
//
// Build-id pinned offsets (cross-arch matched against Windows AdobeAIR.dll
// via call-graph fingerprinting — see tools/crash-analyzer/
// android_phase4b_match*.py for the matching pipeline):
//
//   AArch64 (build-id 7dde220f...):
//     addEventListener:    libCore.so + 0x00c98060  (HIGH confidence)
//     addChild:            TBD
//     removeChild:         TBD
//     removeEventListener: TBD
//
//   ARMv7 (build-id 582a8f65...):
//     all TBD (re-RA needed when AArch64 confirmed in production)
//
// AAPCS64 calling convention recovered from Windows function signatures:
//   addEventListener(this, type_str, listener_obj, useCapture, priority,
//                    useWeakRef)
//   x0 = this (EventDispatcher subclass)
//   x1 = type (Stringp)
//   x2 = listener (Function or method-closure object)
//   w3 = useCapture (bool)
//   w4 = priority (int)
//   w5 = useWeakRef (bool)
//
// The hook reads (x0=owner, x2=listener) and emits an As3Reference edge
// owner→listener with kind=EventListener.

#ifndef ANE_PROFILER_ANDROID_AS3_REFGRAPH_HOOK_HPP
#define ANE_PROFILER_ANDROID_AS3_REFGRAPH_HOOK_HPP

#include <atomic>
#include <cstdint>

namespace ane::profiler {

class DeepProfilerController;

class AndroidAs3RefGraphHook {
public:
    AndroidAs3RefGraphHook() = default;
    ~AndroidAs3RefGraphHook();

    bool install(DeepProfilerController* controller);
    void uninstall();
    bool installed() const noexcept { return installed_; }

    // Diagnostics — counts per hooked method
    std::uint64_t addEventListenerHits() const;
    std::uint64_t removeEventListenerHits() const;
    std::uint64_t addChildHits() const;
    std::uint64_t removeChildHits() const;

private:
    bool installed_ = false;
};

} // namespace ane::profiler

#endif
