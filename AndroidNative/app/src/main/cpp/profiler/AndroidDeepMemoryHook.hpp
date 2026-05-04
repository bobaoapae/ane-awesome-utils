// Phase 5 — Android deep memory hook (MMgc::GCHeap::Alloc / FixedMalloc slow path).
//
// Mirrors WindowsDeepMemoryHook for Android. The Windows side hooks
// `MMgc::FixedMalloc::alloc/allocLocked` (RVAs in docs/profiler-rva-51-1-3-10.md)
// directly via JIT inline patches. On Android we hook the equivalent function
// via shadowhook_hook_func_addr at a build-id-pinned offset in libCore.so.
//
// What's hooked: the alloc-with-retry loop function at libCore.so+0x89c42c on
// AArch64 (build-id 7dde220f...). This is the canonical
// `void* GCHeap::Alloc(size_t size, int flags)` slow path:
//
//   1. (dead since IMemorySampler stripped) call sampler vtable[0][0]
//   2. (dead) call sampler vtable[3]
//   3. bl 0x811800  → b malloc@plt   ★ ALLOC POINT
//   4. if NULL, call heap reclaim (0x89fcec) and retry
//   5. return allocated ptr
//
// alloc_tracer's libc shadowhook ALSO captures these (since `0x811800` is just
// `b malloc@plt`), but only the ones whose CALLER is inside libCore.so range.
// This deep hook captures the same set with cleaner attribution and lower
// overhead — no caller filter needed because we hook GCHeap::Alloc directly.
//
// The two layers can run simultaneously (they both fire DPC events with
// dedup); during the Phase 5 rollout we'll evaluate which is more useful and
// retire the redundant one. Default: deep hook ON, libc shadowhook OFF when
// deep hook installs successfully (controller toggles via bridge).

#ifndef ANE_PROFILER_ANDROID_DEEP_MEMORY_HOOK_HPP
#define ANE_PROFILER_ANDROID_DEEP_MEMORY_HOOK_HPP

#include <atomic>
#include <cstdint>

namespace ane::profiler {

class DeepProfilerController;
class AndroidAs3ObjectHook;

class AndroidDeepMemoryHook {
public:
    AndroidDeepMemoryHook() = default;
    ~AndroidDeepMemoryHook();

    // Install hook on libCore.so:GCHeap::Alloc. Returns false if:
    //   - libCore.so not loaded
    //   - build-id doesn't match expected (Adobe shipped new SDK)
    //   - shadowhook patch failed
    bool install(DeepProfilerController* controller);
    void uninstall();
    bool installed() const noexcept { return installed_; }

    // Phase 4c integration: when a typed-alloc hook is installed, the FixedAlloc
    // proxy will additionally enqueue (ptr, size) into it for deferred class
    // name resolution. May be set/cleared while hook is live.
    void setAs3ObjectHook(AndroidAs3ObjectHook* hook);

    // Diagnostics
    std::uint64_t diagAllocCalls()    const;
    std::uint64_t diagAllocBytes()    const;
    std::uint64_t diagAllocFailures() const;

private:
    bool installed_ = false;
};

} // namespace ane::profiler

#endif // ANE_PROFILER_ANDROID_DEEP_MEMORY_HOOK_HPP
