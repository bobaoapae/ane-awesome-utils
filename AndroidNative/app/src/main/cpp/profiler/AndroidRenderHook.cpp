// Phase 6 — Android render frame hook implementation. See header.

#include "AndroidRenderHook.hpp"

#include <android/log.h>
#include <dlfcn.h>
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
typedef void         (*glClear_t)(unsigned int mask);
typedef void         (*glTexImage2D_t)(unsigned int target, int level, int internalformat,
                                       int width, int height, int border,
                                       unsigned int format, unsigned int type, const void* pixels);
typedef void         (*glTexSubImage2D_t)(unsigned int target, int level, int xoffset, int yoffset,
                                          int width, int height, unsigned int format, unsigned int type,
                                          const void* pixels);
typedef void         (*glBindTexture_t)(unsigned int target, unsigned int texture);
typedef void         (*glBindFramebuffer_t)(unsigned int target, unsigned int framebuffer);

// Bytes-per-pixel for a GLES2 (format, type) pair. Returns 0 for unknown
// combinations (compressed formats, etc.) — caller falls back to "unknown
// size" and skips byte accounting for that texture upload.
static inline std::size_t gles_bpp(unsigned int format, unsigned int type) {
    // GL types we recognise:
    constexpr unsigned int GL_UNSIGNED_BYTE          = 0x1401;
    constexpr unsigned int GL_UNSIGNED_SHORT_5_6_5   = 0x8363;
    constexpr unsigned int GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033;
    constexpr unsigned int GL_UNSIGNED_SHORT_5_5_5_1 = 0x8034;
    constexpr unsigned int GL_FLOAT                  = 0x1406;
    constexpr unsigned int GL_HALF_FLOAT_OES         = 0x8D61;
    // GL formats we recognise (channel count):
    constexpr unsigned int GL_ALPHA           = 0x1906;
    constexpr unsigned int GL_RGB             = 0x1907;
    constexpr unsigned int GL_RGBA            = 0x1908;
    constexpr unsigned int GL_LUMINANCE       = 0x1909;
    constexpr unsigned int GL_LUMINANCE_ALPHA = 0x190A;
    // Packed types — 2 bytes regardless of format channels.
    if (type == GL_UNSIGNED_SHORT_5_6_5 ||
        type == GL_UNSIGNED_SHORT_4_4_4_4 ||
        type == GL_UNSIGNED_SHORT_5_5_5_1) {
        return 2;
    }
    std::size_t channels = 0;
    switch (format) {
        case GL_ALPHA:           channels = 1; break;
        case GL_LUMINANCE:       channels = 1; break;
        case GL_LUMINANCE_ALPHA: channels = 2; break;
        case GL_RGB:             channels = 3; break;
        case GL_RGBA:            channels = 4; break;
        default:                 return 0;
    }
    std::size_t bytes_per_channel = 0;
    switch (type) {
        case GL_UNSIGNED_BYTE:   bytes_per_channel = 1; break;
        case GL_FLOAT:           bytes_per_channel = 4; break;
        case GL_HALF_FLOAT_OES:  bytes_per_channel = 2; break;
        default:                 return 0;
    }
    return channels * bytes_per_channel;
}

// Derive number of primitives from GLES draw `mode` and vertex/index `count`.
// Mirrors Windows' `add_draw_call(primitive_count)` semantics where D3D9 hands
// us a direct primitive count; GLES instead provides vertex/index count and
// requires per-mode math. Returns 0 when the count is too small to form even
// one primitive (e.g. count=2 on GL_TRIANGLES).
static inline std::uint64_t primitives_from_mode_count(unsigned int mode, int count) {
    if (count <= 0) return 0;
    switch (mode) {
        case 0x0000: return static_cast<std::uint64_t>(count);                       // GL_POINTS
        case 0x0001: return static_cast<std::uint64_t>(count) / 2u;                  // GL_LINES
        case 0x0002: return static_cast<std::uint64_t>(count);                       // GL_LINE_LOOP
        case 0x0003: return count >= 2 ? static_cast<std::uint64_t>(count) - 1u : 0; // GL_LINE_STRIP
        case 0x0004: return static_cast<std::uint64_t>(count) / 3u;                  // GL_TRIANGLES
        case 0x0005:                                                                  // GL_TRIANGLE_STRIP
        case 0x0006: return count >= 3 ? static_cast<std::uint64_t>(count) - 2u : 0; // GL_TRIANGLE_FAN
        default:     return 0;
    }
}

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
static thread_local std::uint64_t t_last_swap_ns                = 0;
static thread_local std::uint64_t t_swap_start_ns               = 0;
static thread_local std::uint64_t t_frame_draw_calls            = 0;
static thread_local std::uint64_t t_frame_primitives            = 0;
static thread_local std::uint64_t t_frame_clear_count           = 0;
static thread_local std::uint64_t t_frame_texture_create_count  = 0;
static thread_local std::uint64_t t_frame_texture_create_bytes  = 0;
static thread_local std::uint64_t t_frame_texture_update_count  = 0;
static thread_local std::uint64_t t_frame_texture_upload_bytes  = 0;
static thread_local std::uint64_t t_frame_set_texture_count     = 0;
static thread_local std::uint64_t t_frame_render_target_changes = 0;

// shadowhook stub handles.
static void* g_stub_swap            = nullptr;
static void* g_stub_drawarrays      = nullptr;
static void* g_stub_drawelements    = nullptr;
static void* g_stub_clear           = nullptr;
static void* g_stub_texImage2d      = nullptr;
static void* g_stub_texSubImage2d   = nullptr;
static void* g_stub_bindTexture     = nullptr;
static void* g_stub_bindFramebuffer = nullptr;

// Original function pointers populated by shadowhook on hook.
static eglSwapBuffers_t   g_orig_swap            = nullptr;
static glDrawArrays_t     g_orig_drawarrays      = nullptr;
static glDrawElements_t   g_orig_drawelements    = nullptr;
static glClear_t          g_orig_clear           = nullptr;
static glTexImage2D_t     g_orig_texImage2d      = nullptr;
static glTexSubImage2D_t  g_orig_texSubImage2d   = nullptr;
static glBindTexture_t    g_orig_bindTexture     = nullptr;
static glBindFramebuffer_t g_orig_bindFramebuffer = nullptr;

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
            const std::uint64_t fi              = g_frame_index.fetch_add(1, std::memory_order_relaxed);
            const std::uint64_t draws           = t_frame_draw_calls;
            const std::uint64_t primitives      = t_frame_primitives;
            const std::uint64_t clears          = t_frame_clear_count;
            const std::uint64_t tex_create_cnt  = t_frame_texture_create_count;
            const std::uint64_t tex_create_b    = t_frame_texture_create_bytes;
            const std::uint64_t tex_update_cnt  = t_frame_texture_update_count;
            const std::uint64_t tex_upload_b    = t_frame_texture_upload_bytes;
            const std::uint64_t set_tex_cnt     = t_frame_set_texture_count;
            const std::uint64_t rt_changes      = t_frame_render_target_changes;
            dpc->record_render_frame(
                fi,
                interval_ns,
                cpu_between_ns,
                present_ns,
                draws,
                /* primitive_count */         primitives,
                /* texture_upload_bytes */    tex_upload_b,
                /* texture_create_bytes */    tex_create_b,
                /* texture_create_count */    tex_create_cnt,
                /* texture_update_count */    tex_update_cnt,
                /* set_texture_count */       set_tex_cnt,
                /* render_target_change_count */ rt_changes,
                /* clear_count */             clears,
                /* present_result */          rc);
            g_diag_frames_emit.fetch_add(1, std::memory_order_relaxed);
        }
    }
    t_frame_draw_calls            = 0;
    t_frame_primitives            = 0;
    t_frame_clear_count           = 0;
    t_frame_texture_create_count  = 0;
    t_frame_texture_create_bytes  = 0;
    t_frame_texture_update_count  = 0;
    t_frame_texture_upload_bytes  = 0;
    t_frame_set_texture_count     = 0;
    t_frame_render_target_changes = 0;
    return rc;
}

static void proxy_glDrawArrays(unsigned int mode, int first, int count) {
    g_diag_draw_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_active.load(std::memory_order_relaxed)) {
        ++t_frame_draw_calls;
        t_frame_primitives += primitives_from_mode_count(mode, count);
    }
    if (g_orig_drawarrays != nullptr) {
        g_orig_drawarrays(mode, first, count);
    }
}

static void proxy_glDrawElements(unsigned int mode, int count, unsigned int type, const void* indices) {
    g_diag_draw_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_active.load(std::memory_order_relaxed)) {
        ++t_frame_draw_calls;
        t_frame_primitives += primitives_from_mode_count(mode, count);
    }
    if (g_orig_drawelements != nullptr) {
        g_orig_drawelements(mode, count, type, indices);
    }
}

static void proxy_glClear(unsigned int mask) {
    if (g_active.load(std::memory_order_relaxed)) {
        ++t_frame_clear_count;
    }
    if (g_orig_clear != nullptr) {
        g_orig_clear(mask);
    }
}

static void proxy_glTexImage2D(unsigned int target, int level, int internalformat,
                                int width, int height, int border,
                                unsigned int format, unsigned int type, const void* pixels) {
    if (g_active.load(std::memory_order_relaxed) && level == 0 && width > 0 && height > 0) {
        const std::size_t bpp = gles_bpp(format, type);
        if (bpp > 0) {
            t_frame_texture_create_bytes +=
                static_cast<std::uint64_t>(width) *
                static_cast<std::uint64_t>(height) *
                static_cast<std::uint64_t>(bpp);
        }
        ++t_frame_texture_create_count;
    }
    if (g_orig_texImage2d != nullptr) {
        g_orig_texImage2d(target, level, internalformat, width, height, border, format, type, pixels);
    }
}

static void proxy_glTexSubImage2D(unsigned int target, int level, int xoffset, int yoffset,
                                   int width, int height, unsigned int format, unsigned int type,
                                   const void* pixels) {
    if (g_active.load(std::memory_order_relaxed) && width > 0 && height > 0) {
        const std::size_t bpp = gles_bpp(format, type);
        if (bpp > 0) {
            t_frame_texture_upload_bytes +=
                static_cast<std::uint64_t>(width) *
                static_cast<std::uint64_t>(height) *
                static_cast<std::uint64_t>(bpp);
        }
        ++t_frame_texture_update_count;
    }
    if (g_orig_texSubImage2d != nullptr) {
        g_orig_texSubImage2d(target, level, xoffset, yoffset, width, height, format, type, pixels);
    }
}

static void proxy_glBindTexture(unsigned int target, unsigned int texture) {
    if (g_active.load(std::memory_order_relaxed)) {
        ++t_frame_set_texture_count;
    }
    if (g_orig_bindTexture != nullptr) {
        g_orig_bindTexture(target, texture);
    }
}

static void proxy_glBindFramebuffer(unsigned int target, unsigned int framebuffer) {
    if (g_active.load(std::memory_order_relaxed)) {
        ++t_frame_render_target_changes;
    }
    if (g_orig_bindFramebuffer != nullptr) {
        g_orig_bindFramebuffer(target, framebuffer);
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

// hookOneAddr — resolve symbol via dlsym(RTLD_DEFAULT, ...) and patch the
// resulting address. Catches the actual implementation that the dynamic
// loader hands out, which on Adreno-backed Android devices may live in
// `libGLESv2_adreno.so` rather than `libGLESv2.so`. Adobe's GLES call sites
// cache function pointers from `dlsym` at boot, so patching the lib-exported
// stub via shadowhook_hook_sym_name doesn't intercept those calls (the
// cached pointer skips the patched prologue). Hooking the resolved address
// directly is the same path Adobe's calls go through.
static void* hookOneAddr(const char* sym, void* proxy, void** orig) {
    void* addr = dlsym(RTLD_DEFAULT, sym);
    if (addr == nullptr) {
        LOGE("dlsym(RTLD_DEFAULT, %s) returned null", sym);
        return nullptr;
    }
    void* stub = shadowhook_hook_func_addr(addr, proxy, orig);
    if (stub == nullptr) {
        LOGE("shadowhook_hook_func_addr(%s @ %p) failed: errno=%d %s",
             sym, addr, shadowhook_get_errno(),
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
    // Phase 6 v2 hooks use dlsym + hook_func_addr — see hookOneAddr comment.
    // Empirically: shadowhook_hook_sym_name on libGLESv2.so binds successfully
    // for these symbols but Adobe's render path resolves them via
    // dlsym(RTLD_DEFAULT, ...) at boot and caches the address (which on
    // Adreno-backed devices points into libGLESv2_adreno.so), so the cached
    // pointer bypasses our libGLESv2.so prologue patch. Hooking the actual
    // resolved address catches the calls regardless of which lib provides
    // the implementation.
    g_stub_clear = hookOneAddr("glClear",
                                reinterpret_cast<void*>(&proxy_glClear),
                                reinterpret_cast<void**>(&g_orig_clear));
    g_stub_texImage2d = hookOneAddr("glTexImage2D",
                                     reinterpret_cast<void*>(&proxy_glTexImage2D),
                                     reinterpret_cast<void**>(&g_orig_texImage2d));
    g_stub_texSubImage2d = hookOneAddr("glTexSubImage2D",
                                        reinterpret_cast<void*>(&proxy_glTexSubImage2D),
                                        reinterpret_cast<void**>(&g_orig_texSubImage2d));
    g_stub_bindTexture = hookOneAddr("glBindTexture",
                                      reinterpret_cast<void*>(&proxy_glBindTexture),
                                      reinterpret_cast<void**>(&g_orig_bindTexture));
    g_stub_bindFramebuffer = hookOneAddr("glBindFramebuffer",
                                          reinterpret_cast<void*>(&proxy_glBindFramebuffer),
                                          reinterpret_cast<void**>(&g_orig_bindFramebuffer));

    // Per-hook log — long combined LOGI was getting truncated mid-line on the
    // Cat S60 logger output. Splitting so each binding result is visible
    // independently for diagnostics (tells us "did shadowhook bind?" without
    // needing to disambiguate "logger truncated" vs "stub was null").
    LOGI("install swap=%p", g_stub_swap);
    LOGI("install drawArrays=%p drawElements=%p", g_stub_drawarrays, g_stub_drawelements);
    LOGI("install clear=%p", g_stub_clear);
    LOGI("install texImage2D=%p texSubImage2D=%p", g_stub_texImage2d, g_stub_texSubImage2d);
    LOGI("install bindTexture=%p bindFramebuffer=%p", g_stub_bindTexture, g_stub_bindFramebuffer);

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
    if (g_stub_clear)           { shadowhook_unhook(g_stub_clear);           g_stub_clear           = nullptr; }
    if (g_stub_texImage2d)      { shadowhook_unhook(g_stub_texImage2d);      g_stub_texImage2d      = nullptr; }
    if (g_stub_texSubImage2d)   { shadowhook_unhook(g_stub_texSubImage2d);   g_stub_texSubImage2d   = nullptr; }
    if (g_stub_bindTexture)     { shadowhook_unhook(g_stub_bindTexture);     g_stub_bindTexture     = nullptr; }
    if (g_stub_bindFramebuffer) { shadowhook_unhook(g_stub_bindFramebuffer); g_stub_bindFramebuffer = nullptr; }
    g_controller.store(nullptr, std::memory_order_release);
    installed_ = false;
}

std::uint64_t AndroidRenderHook::diagSwapCalls()  const { return g_diag_swap_calls.load(std::memory_order_relaxed); }
std::uint64_t AndroidRenderHook::diagDrawCalls()  const { return g_diag_draw_calls.load(std::memory_order_relaxed); }
std::uint64_t AndroidRenderHook::diagFramesEmit() const { return g_diag_frames_emit.load(std::memory_order_relaxed); }

} // namespace ane::profiler
