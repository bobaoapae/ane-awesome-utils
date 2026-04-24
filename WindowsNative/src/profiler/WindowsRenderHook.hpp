// Windows D3D/DXGI render hook for aggregated .aneprof performance frames.
//
// The hook is installed only for live profiler captures with render=true. It
// patches shared D3D9/DXGI Present vtable slots discovered from dummy devices,
// then records one aggregate render_frame per Present without per-draw event spam.

#ifndef ANE_PROFILER_WINDOWS_RENDER_HOOK_HPP
#define ANE_PROFILER_WINDOWS_RENDER_HOOK_HPP

#include <cstdint>

namespace ane::profiler {

class DeepProfilerController;

class WindowsRenderHook {
public:
    WindowsRenderHook() = default;
    ~WindowsRenderHook();

    bool install(DeepProfilerController* controller);
    void uninstall();
    bool installed() const noexcept { return installed_; }

    std::uint64_t hookInstalls() const;
    std::uint64_t deviceHookInstalls() const;
    std::uint64_t textureHookInstalls() const;
    std::uint64_t failedInstalls() const;
    std::uint32_t lastFailureStage() const;
    std::uint64_t patchedSlots() const;
    std::uint64_t renderFrames() const;
    std::uint64_t presentCalls() const;
    std::uint64_t drawCalls() const;
    std::uint64_t primitiveCount() const;
    std::uint64_t textureCreates() const;
    std::uint64_t textureUpdates() const;
    std::uint64_t textureUploadBytes() const;
    std::uint64_t textureCreateBytes() const;

private:
    bool installed_ = false;
};

} // namespace ane::profiler

#endif // ANE_PROFILER_WINDOWS_RENDER_HOOK_HPP
