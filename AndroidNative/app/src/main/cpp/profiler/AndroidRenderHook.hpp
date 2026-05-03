// Phase 6 — Android render frame hook.
//
// Intercepts EGL/GLES calls via shadowhook to emit RenderFrame events into the
// .aneprof stream. NOT a port of WindowsRenderHook (which uses D3D9/DXGI
// Present vtable patching) — Adobe AIR on Android renders through OpenGL ES
// over EGL, fundamentally different surface.
//
// Event model:
//   - eglSwapBuffers       → end of frame N, start of frame N+1. Compute
//                            interval_ns, present_ns, emit RenderFrameEvent.
//   - glDrawElements/glDrawArrays → draw_calls counter for current frame.
//   - glTexImage2D / glTexSubImage2D → texture_upload_bytes (best-effort).
//
// Lifecycle:
//   - install(): resolves EGL/GLES symbols via shadowhook_hook_sym_name on
//                "libEGL.so" + "libGLESv2.so". Caller filter unused — render
//                calls in Adobe AIR all originate from libCore.so but EGL
//                is also called by other system components (Dialog widgets);
//                we accept the small noise rather than dlopen-walking each
//                call.
//   - uninstall(): unhooks all stubs.
//
// Threading: eglSwapBuffers runs on the GL render thread (one per Stage3D
// context, typically one per app). glDraw* runs on the same thread. We use
// thread_local frame state — multi-Context3D would emit interleaved events
// but the analyzer can group by frame_index globally.

#ifndef ANE_PROFILER_ANDROID_RENDER_HOOK_HPP
#define ANE_PROFILER_ANDROID_RENDER_HOOK_HPP

#include <atomic>
#include <cstdint>

namespace ane::profiler {

class DeepProfilerController;

class AndroidRenderHook {
public:
    AndroidRenderHook() = default;
    ~AndroidRenderHook();

    bool install(DeepProfilerController* controller);
    void uninstall();
    bool installed() const noexcept { return installed_; }

    // Diagnostic counters — read by AndroidProfilerBridge::nativeGetStatusDeep.
    std::uint64_t diagSwapCalls()  const;
    std::uint64_t diagDrawCalls()  const;
    std::uint64_t diagFramesEmit() const;

private:
    bool installed_ = false;
};

} // namespace ane::profiler

#endif // ANE_PROFILER_ANDROID_RENDER_HOOK_HPP
