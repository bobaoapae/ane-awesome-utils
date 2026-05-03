// Phase 6 — Android render frame hook implementation. See header.

#include "AndroidRenderHook.hpp"

#include <android/log.h>
#include <shadowhook.h>
#include <atomic>
#include <chrono>
#include <cstdint>

#include "DeepProfilerController.hpp"

#define LOG_TAG "AneRenderHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace ane::profiler {
namespace {

// EGL surface handles are opaque pointers in the host app — we don't deref
// them, just pass through. Same for context.
using EGLDisplay = void*;
using EGLSurface = void*;

// Function pointer typedefs matching the EGL/GLES headers we hook.
typedef unsigned int (*eglSwapBuffers_t)(EGLDisplay, EGLSurface);
typedef void         (*glDrawArrays_t)(unsigned int mode, int first, int count);
typedef void         (*glDrawElements_t)(unsigned int mode, int count, unsigned int type, const void* indices);

// ----- State -----
//
// The hook is a process-wide singleton (one render thread per app). We keep
// state at namespace scope rather than as instance members to avoid a
// per-call indirection (the proxy's hot path runs at ~3600 Hz at 60fps).

static std::atomic<DeepProfilerController*> g_controller{nullptr};
static std::atomic<bool>                    g_active{false};
static std::atomic<std::uint64_t>           g_frame_index{0};
static std::atomic<std::uint64_t>           g_diag_swap_calls{0};
static std::atomic<std::uint64_t>           g_diag_draw_calls{0};
static std::atomic<std::uint64_t>           g_diag_frames_emit{0};

// Per-thread frame state. eglSwapBuffers and glDraw* run on the same render
// thread; multi-context apps spawn one thread per Context3D and we'd emit
// interleaved events keyed by frame_index globally.
static thread_local std::uint64_t t_last_swap_ns      = 0;
static thread_local std::uint64_t t_swap_start_ns     = 0;
static thread_local std::uint64_t t_frame_draw_calls  = 0;

// shadowhook stub handles.
static void* g_stub_swap          = nullptr;
static void* g_stub_drawarrays    = nullptr;
static void* g_stub_drawelements  = nullptr;

// Original function pointers populated by shadowhook on hook.
static eglSwapBuffers_t g_orig_swap         = nullptr;
static glDrawArrays_t   g_orig_drawarrays   = nullptr;
static glDrawElements_t g_orig_drawelements = nullptr;

static inline std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count());
}

// ----- Proxies -----
//
// Hot path. Keep cheap: no logging, no syscalls beyond the timestamp clock,
// no locks.

static unsigned int proxy_eglSwapBuffers(EGLDisplay display, EGLSurface surface) {
    g_diag_swap_calls.fetch_add(1, std::memory_order_relaxed);

    const std::uint64_t before_ns = nowNs();
    const std::uint64_t cpu_between_ns = (t_last_swap_ns != 0) ? (before_ns - t_last_swap_ns) : 0;
    t_swap_start_ns = before_ns;

    const unsigned int rc = (g_orig_swap != nullptr) ? g_orig_swap(display, surface) : 1u;

    const std::uint64_t after_ns = nowNs();
    const std::uint64_t present_ns = after_ns - before_ns;
    const std::uint64_t interval_ns = (t_last_swap_ns != 0) ? (after_ns - t_last_swap_ns) : 0;
    t_last_swap_ns = after_ns;

    if (g_active.load(std::memory_order_acquire)) {
        DeepProfilerController* dpc = g_controller.load(std::memory_order_acquire);
        if (dpc != nullptr) {
            const std::uint64_t fi = g_frame_index.fetch_add(1, std::memory_order_relaxed);
            const std::uint64_t draws = t_frame_draw_calls;
            // Phase 6 v1: only swap timing + draw count populated. Texture
            // bytes/counts are zero (would need glTexImage2D hooks — TBD).
            dpc->record_render_frame(
                fi,
                interval_ns,
                cpu_between_ns,
                present_ns,
                draws,
                /* primitive_count */         0,
                /* texture_upload_bytes */    0,
                /* texture_create_bytes */    0,
                /* texture_create_count */    0,
                /* texture_update_count */    0,
                /* set_texture_count */       0,
                /* render_target_change_count */ 0,
                /* clear_count */             0,
                /* present_result */          rc);
            g_diag_frames_emit.fetch_add(1, std::memory_order_relaxed);
        }
    }
    t_frame_draw_calls = 0;
    return rc;
}

static void proxy_glDrawArrays(unsigned int mode, int first, int count) {
    g_diag_draw_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_active.load(std::memory_order_relaxed)) {
        ++t_frame_draw_calls;
    }
    if (g_orig_drawarrays != nullptr) {
        g_orig_drawarrays(mode, first, count);
    }
}

static void proxy_glDrawElements(unsigned int mode, int count, unsigned int type, const void* indices) {
    g_diag_draw_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_active.load(std::memory_order_relaxed)) {
        ++t_frame_draw_calls;
    }
    if (g_orig_drawelements != nullptr) {
        g_orig_drawelements(mode, count, type, indices);
    }
}

static void* hookOne(const char* lib, const char* sym, void* proxy, void** orig) {
    void* stub = shadowhook_hook_sym_name(lib, sym, proxy, orig);
    if (stub == nullptr) {
        LOGE("shadowhook_hook_sym_name(%s, %s) failed: errno=%d %s",
             lib, sym, shadowhook_get_errno(),
             shadowhook_to_errmsg(shadowhook_get_errno()));
    }
    return stub;
}

} // namespace

AndroidRenderHook::~AndroidRenderHook() {
    uninstall();
}

bool AndroidRenderHook::install(DeepProfilerController* controller) {
    if (installed_) return true;
    if (controller == nullptr) {
        LOGE("install: controller is null");
        return false;
    }
    g_controller.store(controller, std::memory_order_release);
    g_frame_index.store(0);
    g_diag_swap_calls.store(0);
    g_diag_draw_calls.store(0);
    g_diag_frames_emit.store(0);

    g_stub_swap = hookOne("libEGL.so", "eglSwapBuffers",
                           reinterpret_cast<void*>(&proxy_eglSwapBuffers),
                           reinterpret_cast<void**>(&g_orig_swap));
    g_stub_drawarrays = hookOne("libGLESv2.so", "glDrawArrays",
                                 reinterpret_cast<void*>(&proxy_glDrawArrays),
                                 reinterpret_cast<void**>(&g_orig_drawarrays));
    g_stub_drawelements = hookOne("libGLESv2.so", "glDrawElements",
                                   reinterpret_cast<void*>(&proxy_glDrawElements),
                                   reinterpret_cast<void**>(&g_orig_drawelements));

    LOGI("install: swap=%p drawArrays=%p drawElements=%p",
         g_stub_swap, g_stub_drawarrays, g_stub_drawelements);

    // We require eglSwapBuffers at minimum — without it we have no frame
    // boundary signal. glDraw* are best-effort; their absence just means
    // RenderFrame events have draw_calls=0.
    if (g_stub_swap == nullptr) {
        LOGE("install: eglSwapBuffers hook failed — render hook disabled");
        uninstall();
        return false;
    }

    g_active.store(true, std::memory_order_release);
    installed_ = true;
    return true;
}

void AndroidRenderHook::uninstall() {
    g_active.store(false, std::memory_order_release);
    if (g_stub_swap)         { shadowhook_unhook(g_stub_swap);         g_stub_swap         = nullptr; }
    if (g_stub_drawarrays)   { shadowhook_unhook(g_stub_drawarrays);   g_stub_drawarrays   = nullptr; }
    if (g_stub_drawelements) { shadowhook_unhook(g_stub_drawelements); g_stub_drawelements = nullptr; }
    g_controller.store(nullptr, std::memory_order_release);
    installed_ = false;
}

std::uint64_t AndroidRenderHook::diagSwapCalls()  const { return g_diag_swap_calls.load(std::memory_order_relaxed); }
std::uint64_t AndroidRenderHook::diagDrawCalls()  const { return g_diag_draw_calls.load(std::memory_order_relaxed); }
std::uint64_t AndroidRenderHook::diagFramesEmit() const { return g_diag_frames_emit.load(std::memory_order_relaxed); }

} // namespace ane::profiler
