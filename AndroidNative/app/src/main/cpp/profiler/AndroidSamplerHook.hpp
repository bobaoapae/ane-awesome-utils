// Phase 4a — Android sampler vftable diagnostic hook.
//
// Adobe AIR Android libCore.so DOES ship a working IMemorySampler — confirmed
// via flash.sampler.getSamples() returning 4000+ NewObjectSample/
// DeleteObjectSample objects on Cat S60 (build-id 7dde220f...). The earlier
// "DEBUGGER stripped" hypothesis was wrong: only the assert/log strings
// (presample/recordAllocationSample/...) are absent; the code is there.
//
// Sampler location recovered dynamically:
//   - GC singleton captured by AndroidGcHook on first MMgc::GC::Collect cycle
//   - gc_this+0x10 holds a heap pointer (originally suspected AvmCore*)
//   - That object's first 8 bytes hold a vftable in .data ≈ 0x7f640ad160
//   - Sample buffer at offsets 0x1500-0x2000 of that object (cleared by
//     IMemorySampler::clearSamples on startSampling, repopulated on alloc)
//   - vftable shape: 10 non-null slots in 16 with multi-base-class layout
//     (Adobe's MemoryTelemetrySampler extends both IMemorySampler and
//     TelemetrySampler)
//
// This DIAGNOSTIC hook installs a proxy on every non-null slot of the vftable.
// Each proxy increments a per-slot atomic counter and forwards to the original.
// After running a churn scenario with the sampler active, the slot frequency
// distribution reveals:
//   - ~1 hit per AS3 allocation        → recordAllocationSample
//   - ~1 hit per AS3 deallocation      → recordDeallocationSample
//   - ~1 hit per GC sweep              → sampleInternalAllocs / presample
//   - low/zero hits                    → start/stop/clear/sampling-state
//
// Once slot indexes are identified, AndroidAs3SamplerHook replaces this
// diagnostic hook with a productive implementation that intercepts only
// recordAllocationSample/Deallocation and emits typed As3Alloc/As3Free events.

#ifndef ANE_PROFILER_ANDROID_SAMPLER_HOOK_HPP
#define ANE_PROFILER_ANDROID_SAMPLER_HOOK_HPP

#include <atomic>
#include <cstdint>

namespace ane::profiler {

class DeepProfilerController;

class AndroidSamplerHook {
public:
    AndroidSamplerHook() = default;
    ~AndroidSamplerHook();

    // Install proxies on every non-null vftable slot of the sampler. Requires
    // gc_singleton to have been captured by AndroidGcHook (call after
    // forceGcViaChurn or natural Collect). Returns false if sampler can't be
    // located.
    bool install(DeepProfilerController* controller);
    void uninstall();
    bool installed() const noexcept { return installed_; }

    // Returns invocation count for slot index `i` (0..15). Used by RA pass
    // to identify which slot is recordAllocationSample by frequency.
    std::uint64_t slotHits(std::size_t i) const;

    // Number of vftable slots actually hooked (= count of non-null slots).
    std::size_t hookedSlotCount() const noexcept { return hooked_count_; }

    // Recovered sampler addr + vftable addr for offline analysis.
    std::uintptr_t samplerObjAddr() const noexcept { return sampler_obj_; }
    std::uintptr_t samplerVftableAddr() const noexcept { return sampler_vftable_; }

private:
    bool installed_ = false;
    std::size_t hooked_count_ = 0;
    std::uintptr_t sampler_obj_ = 0;
    std::uintptr_t sampler_vftable_ = 0;
};

} // namespace ane::profiler

#endif // ANE_PROFILER_ANDROID_SAMPLER_HOOK_HPP
