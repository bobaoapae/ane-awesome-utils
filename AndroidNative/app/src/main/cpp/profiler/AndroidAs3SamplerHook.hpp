// Phase 4a — Android typed AS3 alloc capture via MemoryTelemetrySampler hook.
//
// Productive hook (replaces AndroidSamplerHook diagnostic). Hooks the
// sampler's recordAllocationSample slot. Sampler discovered dynamically:
//
//   gc_this  →  +0x10  =  AvmCore*
//   AvmCore  →  +0xa0  =  IMemorySampler*  (Cat S60, build-id 7dde220f...)
//   IMemorySampler*  →  +0    =  vftable
//   vftable  →  +0x38 (slot 7)  =  recordAllocationSample (candidate)
//
// Calling convention assumed (avmplus open source signature):
//   void recordAllocationSample(this, ptr, size, hasInlineFrame, fromMemUsage)
//   AAPCS64: x0=this, x1=ptr, x2=size, x3=bool, x4=bool
//
// First invocations log args so we can verify the shape. Once confirmed,
// the hook reads (ptr, size), walks ptr+vftable→Traits→name (reusing the
// AndroidAs3ObjectHook walker), and emits As3Alloc events into DPC.

#ifndef ANE_PROFILER_ANDROID_AS3_SAMPLER_HOOK_HPP
#define ANE_PROFILER_ANDROID_AS3_SAMPLER_HOOK_HPP

#include <atomic>
#include <cstdint>

namespace ane::profiler {

class DeepProfilerController;

class AndroidAs3SamplerHook {
public:
    AndroidAs3SamplerHook() = default;
    ~AndroidAs3SamplerHook();

    // Install hook on the recordAllocationSample slot. Requires GC singleton
    // captured (call after at least one Collect cycle).
    bool install(DeepProfilerController* controller);
    void uninstall();
    bool installed() const noexcept { return installed_; }

    // Diagnostics
    std::uint64_t hitCount() const;
    std::uint64_t resolvedCount() const;
    std::uint64_t unresolvedCount() const;

private:
    bool installed_ = false;
};

} // namespace ane::profiler

#endif
