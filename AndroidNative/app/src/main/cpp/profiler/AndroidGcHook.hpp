// Phase 7a — Android GC cycle observer + programmatic trigger.
//
// Mirrors the WindowsGcHook pattern: shadowhook libCore.so:MMgc::GC::Collect
// at a build-id-pinned offset so we know when GC fires. Emits one
// `GcCycleEvent{NativeObserved}` per Collect() call, with timing.
//
// Programmatic GC trigger: the hook captures `this` (x0/r0) on first fire and
// stashes it as the recovered GC singleton. requestCollect() can then invoke
// the original Collect() with the captured pointer — eliminating need for
// static RA on the global GC pointer (no `_ZN5MMgc2GC*` exports survive on
// Android, and BL-call-site scan finds zero direct callers — Adobe dispatches
// Collect via fn-pointer/vtable, so static singleton recovery is impractical).

#ifndef ANE_PROFILER_ANDROID_GC_HOOK_HPP
#define ANE_PROFILER_ANDROID_GC_HOOK_HPP

#include <atomic>
#include <cstdint>

namespace ane::profiler {

class DeepProfilerController;

class AndroidGcHook {
public:
    AndroidGcHook() = default;
    ~AndroidGcHook();

    bool install(DeepProfilerController* controller);
    void uninstall();
    bool installed() const noexcept { return installed_; }

    std::uint64_t diagCollectCalls() const;

    // Programmatic GC trigger. Returns true if a GC singleton has been
    // observed (captured from a prior Collect() call) and Collect was invoked.
    // Returns false if no Collect has fired yet and the singleton is unknown.
    //
    // Activation prerequisite: at least one runtime-triggered Collect must
    // have fired since hook install. Boot+idle workloads (<5MB AS3 alloc
    // pressure) typically do NOT trigger natural collections — synthetic
    // androidProbe churn for 70s with 4861 allocs was insufficient on
    // OnePlus 15. In real gameplay (PVP, scene transitions, asset loads)
    // collections fire within seconds; subsequent requestCollect() calls
    // work indefinitely.
    //
    // AS3 callers should retry on false to handle the activation race —
    // see Profiler.requestGc() Java doc for the recommended pattern.
    bool requestCollect();

private:
    bool installed_ = false;
};

} // namespace ane::profiler

#endif // ANE_PROFILER_ANDROID_GC_HOOK_HPP
