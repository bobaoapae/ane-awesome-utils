// Windows AVM object hook for typed AS3 allocation events in .aneprof.
//
// AIR SDK 51.1.3.10 only. This attaches a native IMemorySampler to the
// active AVM GC so AS3 object alloc/free callbacks arrive in the ANE without
// going through flash.sampler, Scout telemetry, or compiler-injected probes.

#ifndef ANE_PROFILER_WINDOWS_AS3_OBJECT_HOOK_HPP
#define ANE_PROFILER_WINDOWS_AS3_OBJECT_HOOK_HPP

#include <cstdint>
#include <string>

namespace ane::profiler {

class DeepProfilerController;

class WindowsAs3ObjectHook {
public:
    WindowsAs3ObjectHook() = default;
    ~WindowsAs3ObjectHook();

    bool install(DeepProfilerController* controller);
    void uninstall();
    bool installed() const noexcept { return installed_; }

    std::uint64_t as3AllocCalls() const;
    std::uint64_t as3FreeCalls() const;
    std::uint64_t genericAllocCalls() const;
    std::uint64_t failedInstalls() const;
    std::uint64_t chainedInstalls() const;
    std::uint64_t directSlotInstalls() const;
    std::uint64_t directSlotFailures() const;
    std::uint64_t forwardedCalls() const;
    std::uint64_t forwardFailures() const;
    std::uint64_t realEdgeHookInstalls() const;
    std::uint64_t realEdgeHookFailures() const;
    std::uint64_t realDisplayChildEdges() const;
    std::uint64_t realDisplayChildRemoves() const;
    std::uint64_t realEventListenerEdges() const;
    std::uint64_t realEventListenerRemoves() const;
    std::uint32_t realEdgeLastFailureStage() const;
    std::uint32_t lastFailureStage() const;
    bool chainedSampler() const;
    std::uintptr_t currentSamplerPtr() const;
    std::uintptr_t currentSamplerVtable() const;
    std::uintptr_t samplerSlotPtr() const;
    std::uintptr_t previousSamplerPtr() const;
    std::uintptr_t previousSamplerVtable() const;
    std::string currentSamplerModule() const;
    std::string currentSamplerVtableModule() const;
    std::string previousSamplerModule() const;
    std::string previousSamplerVtableModule() const;
    std::string previousSamplerVtableHead() const;

private:
    bool installed_ = false;
};

} // namespace ane::profiler

#endif // ANE_PROFILER_WINDOWS_AS3_OBJECT_HOOK_HPP
