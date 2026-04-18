// Cross-platform description of what the runtime should capture.
// Mirrors the TelemetryConfig fields AIR expects (see mode-b plan §2).

#ifndef ANE_PROFILER_FEATURES_HPP
#define ANE_PROFILER_FEATURES_HPP

#include <cstdint>

namespace ane::profiler {

struct ProfilerFeatures {
    bool        sampler_enabled                  = true;   // CPU stack sampler
    bool        cpu_capture                      = true;   // frame/render CPU metrics
    bool        display_object_capture           = false;  // display list events
    bool        stage3d_capture                  = false;  // GPU draw calls
    bool        script_object_allocation_traces  = false;  // per-allocation records (expensive)
    bool        all_gc_allocation_traces         = false;  // every GC alloc (very expensive)
    std::uint32_t gc_allocation_traces_threshold = 1024;   // ignore allocs smaller than this
};

} // namespace ane::profiler

#endif // ANE_PROFILER_FEATURES_HPP
