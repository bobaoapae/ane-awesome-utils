#include "AndroidRuntimeHook.hpp"

#include "CaptureController.hpp"
#include "PrologueBuffer.hpp"

#include <shadowhook.h>
#include <android/log.h>
#include <unwind.h>
#include <dlfcn.h>
#include <link.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

#define LOG_TAG "AneRuntimeHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace ane::profiler {

namespace {

// Globals shared between hook proxies (mirrors Windows impl exactly).
static std::atomic<CaptureController*> g_controller{nullptr};

// shadowhook stub handles (returned by shadowhook_hook_sym_name).
static void* g_stub_send    = nullptr;
static void* g_stub_sendto  = nullptr;
static void* g_stub_connect = nullptr;
static void* g_stub_close   = nullptr;

// Original libc symbols populated by shadowhook on hook install.
typedef ssize_t (*send_t)(int, const void*, size_t, int);
typedef ssize_t (*sendto_t)(int, const void*, size_t, int, const sockaddr*, socklen_t);
typedef int     (*connect_t)(int, const sockaddr*, socklen_t);
typedef int     (*close_t)(int);

static send_t    g_orig_send    = nullptr;
static sendto_t  g_orig_sendto  = nullptr;
static connect_t g_orig_connect = nullptr;
static close_t   g_orig_close   = nullptr;

// Target address for telemetry filter. Stored in network byte order.
static std::atomic<std::uint16_t> g_target_port_be{0};
static constexpr std::uint32_t    kLoopbackIpv4Be = 0x0100007Fu; // 127.0.0.1

// Per-fd classification cache. Same logic as Windows: connect marks,
// send/sendto checks (with getpeername fallback for fds opened pre-install),
// close unmarks.
static std::mutex                       g_monitored_mu;
static std::unordered_map<int, bool>    g_monitored;

// libCore.so address range — populated on install via dl_iterate_phdr.
// Used to filter caller of send/connect; only AIR-runtime-originated calls
// are subject to telemetry capture.
static uintptr_t g_libcore_start = 0;
static uintptr_t g_libcore_end   = 0;

// Diagnostic counters.
static std::atomic<std::uint64_t> g_diag_send_calls{0};
static std::atomic<std::uint64_t> g_diag_send_captured{0};
static std::atomic<std::uint64_t> g_diag_connect_calls{0};
static std::atomic<std::uint64_t> g_diag_connect_matched{0};
static std::atomic<std::uint64_t> g_diag_close_calls{0};

// Re-entry guard to prevent recursion through hook→logger→hook.
static thread_local bool t_in_hook = false;

// ----- libCore.so range discovery -----

static int phdrCallback(struct dl_phdr_info* info, size_t, void* data) {
    const char* name = info->dlpi_name;
    if (!name) return 0;
    const char* slash = strrchr(name, '/');
    const char* base = slash ? slash + 1 : name;
    if (strcmp(base, "libCore.so") != 0) return 0;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const auto& p = info->dlpi_phdr[i];
        if (p.p_type != PT_LOAD) continue;
        uintptr_t a = info->dlpi_addr + p.p_vaddr;
        uintptr_t b = a + p.p_memsz;
        if (a < lo) lo = a;
        if (b > hi) hi = b;
    }
    if (lo < hi) {
        auto* range = static_cast<std::pair<uintptr_t, uintptr_t>*>(data);
        range->first = lo;
        range->second = hi;
        return 1;
    }
    return 0;
}

static bool discoverLibCoreRange() {
    std::pair<uintptr_t, uintptr_t> range{0, 0};
    dl_iterate_phdr(phdrCallback, &range);
    if (range.first == 0 || range.second == 0) {
        LOGE("libCore.so not loaded — caller filter cannot install");
        return false;
    }
    g_libcore_start = range.first;
    g_libcore_end   = range.second;
    LOGI("libCore.so range: 0x%lx-0x%lx (%lu bytes)",
         (unsigned long)g_libcore_start, (unsigned long)g_libcore_end,
         (unsigned long)(g_libcore_end - g_libcore_start));
    return true;
}

// ----- Caller filter -----

// Returns true if the immediate caller PC lies in libCore.so. Uses
// _Unwind_Backtrace to walk the stack a single frame above our proxy.
// Cost: ~100-200 ns per check. Cached per-fd via g_monitored to keep the
// hot send/sendto path at one map lookup once classified.

struct UnwindCtx {
    uintptr_t* frames;
    size_t     max;
    size_t     count;
};

static _Unwind_Reason_Code unwindCallback(struct _Unwind_Context* ctx, void* arg) {
    UnwindCtx* uc = static_cast<UnwindCtx*>(arg);
    if (uc->count >= uc->max) return _URC_END_OF_STACK;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc == 0) return _URC_END_OF_STACK;
    uc->frames[uc->count++] = pc;
    return _URC_NO_REASON;
}

static bool callerIsLibCore() {
    uintptr_t frames[8];
    UnwindCtx uc{frames, 8, 0};
    _Unwind_Backtrace(unwindCallback, &uc);
    // Skip our own proxy frame (frames[0]); look for any frame in libCore.so.
    for (size_t i = 1; i < uc.count; i++) {
        if (frames[i] >= g_libcore_start && frames[i] < g_libcore_end) {
            return true;
        }
    }
    return false;
}

// ----- Hook proxies -----

static int proxy_connect(int fd, const sockaddr* name, socklen_t namelen) {
    int ret = g_orig_connect ? g_orig_connect(fd, name, namelen) : -1;
    int last_err = (ret == 0) ? 0 : errno;
    bool ok_or_pending = (ret == 0) || (last_err == EINPROGRESS);

    if (t_in_hook) { errno = last_err; return ret; }
    t_in_hook = true;

    g_diag_connect_calls.fetch_add(1, std::memory_order_relaxed);

    const std::uint16_t target_port = g_target_port_be.load(std::memory_order_acquire);
    if (ok_or_pending && target_port != 0 &&
        name && namelen >= (socklen_t)sizeof(sockaddr_in) &&
        name->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(name);
        bool match = (in->sin_port == target_port &&
                      in->sin_addr.s_addr == kLoopbackIpv4Be) &&
                     callerIsLibCore();
        if (match) g_diag_connect_matched.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_monitored_mu);
        g_monitored[fd] = match;
    }

    t_in_hook = false;
    if (ret != 0) errno = last_err;
    return ret;
}

static int proxy_close(int fd) {
    if (!t_in_hook) {
        t_in_hook = true;
        g_diag_close_calls.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(g_monitored_mu);
            g_monitored.erase(fd);
        }
        t_in_hook = false;
    }
    return g_orig_close ? g_orig_close(fd) : -1;
}

// Common send-path body — used by both send() and sendto() proxies.
static void capture_if_monitored(int fd, const void* buf, ssize_t sent) {
    if (sent <= 0 || !buf) return;
    if (t_in_hook) return;
    t_in_hook = true;

    g_diag_send_calls.fetch_add(1, std::memory_order_relaxed);

    const std::uint16_t target_port = g_target_port_be.load(std::memory_order_acquire);
    bool monitored = false;

    if (target_port == 0) {
        // Filter disabled — capture everything (legacy behavior).
        monitored = callerIsLibCore();
    } else {
        bool cached = false;
        {
            std::lock_guard<std::mutex> lk(g_monitored_mu);
            auto it = g_monitored.find(fd);
            if (it != g_monitored.end()) {
                monitored = it->second;
                cached = true;
            }
        }
        if (!cached) {
            // Pre-install fd. Classify via getpeername now and cache.
            sockaddr_storage ss{};
            socklen_t sslen = sizeof(ss);
            int gp = ::getpeername(fd, reinterpret_cast<sockaddr*>(&ss), &sslen);
            bool match = false;
            if (gp == 0 && ss.ss_family == AF_INET) {
                const auto* in = reinterpret_cast<const sockaddr_in*>(&ss);
                match = (in->sin_port == target_port &&
                         in->sin_addr.s_addr == kLoopbackIpv4Be) &&
                        callerIsLibCore();
            }
            std::lock_guard<std::mutex> lk(g_monitored_mu);
            g_monitored[fd] = match;
            monitored = match;
        }
    }

    if (monitored) {
        g_diag_send_captured.fetch_add(static_cast<std::uint64_t>(sent),
                                       std::memory_order_relaxed);
        PrologueBuffer::instance().append(buf, static_cast<std::size_t>(sent));
        CaptureController* ctrl = g_controller.load(std::memory_order_acquire);
        if (ctrl) {
            ctrl->push_bytes(buf, static_cast<std::uint32_t>(sent));
        }
    }

    t_in_hook = false;
}

static ssize_t proxy_send(int fd, const void* buf, size_t len, int flags) {
    ssize_t sent = g_orig_send ? g_orig_send(fd, buf, len, flags) : -1;
    capture_if_monitored(fd, buf, sent);
    return sent;
}

static ssize_t proxy_sendto(int fd, const void* buf, size_t len, int flags,
                            const sockaddr* dest, socklen_t addrlen) {
    ssize_t sent = g_orig_sendto ? g_orig_sendto(fd, buf, len, flags, dest, addrlen) : -1;
    capture_if_monitored(fd, buf, sent);
    return sent;
}

// ----- shadowhook plumbing -----

static void* hookSym(const char* sym, void* proxy, void** orig) {
    // shadowhook 2.x rejects lib_name=NULL with SHADOWHOOK_ERRNO_INVALID_ARG (3).
    // bionic socket symbols live in libc.so on all Android arches we ship.
    void* stub = shadowhook_hook_sym_name("libc.so", sym, proxy, orig);
    if (!stub) {
        int err = shadowhook_get_errno();
        LOGE("shadowhook_hook_sym_name(libc.so, %s) failed: errno=%d %s",
             sym, err, shadowhook_to_errmsg(err));
    }
    return stub;
}

} // namespace

// ----- Public API -----

std::uint64_t AndroidRuntimeHook::diagSendCalls()      const { return g_diag_send_calls.load(); }
std::uint64_t AndroidRuntimeHook::diagSendCaptured()   const { return g_diag_send_captured.load(); }
std::uint64_t AndroidRuntimeHook::diagConnectCalls()   const { return g_diag_connect_calls.load(); }
std::uint64_t AndroidRuntimeHook::diagConnectMatched() const { return g_diag_connect_matched.load(); }
std::uint64_t AndroidRuntimeHook::diagCloseCalls()     const { return g_diag_close_calls.load(); }

AndroidRuntimeHook::~AndroidRuntimeHook() {
    uninstall();
}

bool AndroidRuntimeHook::install(CaptureController* controller,
                                 std::uint16_t telemetry_port) {
    if (installed_) {
        // Already installed — re-bind controller + port (matches Windows).
        g_controller.store(controller, std::memory_order_release);
        g_target_port_be.store(htons(telemetry_port), std::memory_order_release);
        return true;
    }
    if (!controller) return false;
    if (!discoverLibCoreRange()) return false;

    g_controller.store(controller, std::memory_order_release);
    g_target_port_be.store(htons(telemetry_port), std::memory_order_release);

    // Hook order matches Windows: connect first (so subsequent connects
    // are properly classified), then close, then send/sendto.
    g_stub_connect = hookSym("connect", (void*)proxy_connect,
                             reinterpret_cast<void**>(&g_orig_connect));
    g_stub_close   = hookSym("close",   (void*)proxy_close,
                             reinterpret_cast<void**>(&g_orig_close));
    g_stub_send    = hookSym("send",    (void*)proxy_send,
                             reinterpret_cast<void**>(&g_orig_send));
    g_stub_sendto  = hookSym("sendto",  (void*)proxy_sendto,
                             reinterpret_cast<void**>(&g_orig_sendto));

    if (!g_stub_send) {
        // send is the critical hook. If it fails, abort and rollback.
        uninstall();
        return false;
    }
    LOGI("hooks installed: connect=%p close=%p send=%p sendto=%p target_port=%u",
         g_stub_connect, g_stub_close, g_stub_send, g_stub_sendto, telemetry_port);
    installed_ = true;
    return true;
}

void AndroidRuntimeHook::uninstall() {
    if (!installed_ && !g_stub_send && !g_stub_connect && !g_stub_close && !g_stub_sendto) {
        return;
    }
    if (g_stub_send)    { shadowhook_unhook(g_stub_send);    g_stub_send    = nullptr; }
    if (g_stub_sendto)  { shadowhook_unhook(g_stub_sendto);  g_stub_sendto  = nullptr; }
    if (g_stub_close)   { shadowhook_unhook(g_stub_close);   g_stub_close   = nullptr; }
    if (g_stub_connect) { shadowhook_unhook(g_stub_connect); g_stub_connect = nullptr; }
    {
        std::lock_guard<std::mutex> lk(g_monitored_mu);
        g_monitored.clear();
    }
    g_orig_send    = nullptr;
    g_orig_sendto  = nullptr;
    g_orig_connect = nullptr;
    g_orig_close   = nullptr;
    g_controller.store(nullptr, std::memory_order_release);
    g_target_port_be.store(0,   std::memory_order_release);
    installed_ = false;
}

// Factory for IRuntimeHook::create() on Android.
std::unique_ptr<IRuntimeHook> IRuntimeHook::create() {
    return std::unique_ptr<IRuntimeHook>(new AndroidRuntimeHook());
}

} // namespace ane::profiler
