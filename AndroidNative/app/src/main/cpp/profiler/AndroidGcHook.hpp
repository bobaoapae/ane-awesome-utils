// Phase 7a — Android GC cycle observer hook.
//
// Mirrors the WindowsGcHook pattern: shadowhook libCore.so:MMgc::GC::Collect
// at a build-id-pinned offset so we know when GC fires. Emits one
// `GcCycleEvent{NativeObserved}` per Collect() call, with timing.
//
// Triggering GC programmatically (`profilerRequestGc` AS3 entry point) is NOT
// done by this hook — that requires a global GC singleton pointer which has
// not yet been RA'd on Android. profilerRequestGc remains a no-op until the
// singleton is found.

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

private:
    bool installed_ = false;
};

} // namespace ane::profiler

#endif // ANE_PROFILER_ANDROID_GC_HOOK_HPP
