#include "profiler/WindowsRuntimeHook.hpp"

#include "CaptureController.hpp"
#include "PrologueBuffer.hpp"

// winsock2.h must come before windows.h to avoid the legacy winsock.h
// being pulled in transitively and clashing on SOCKET / WSA* definitions.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace ane::profiler {

// Globals shared between the three hook functions (which have no closure).
// Only one WindowsRuntimeHook instance at a time is supported — a process
// has exactly one AIR runtime.
static std::atomic<CaptureController*> g_controller{nullptr};
static std::atomic<void*>              g_original_send{nullptr};
static std::atomic<void*>              g_original_connect{nullptr};
static std::atomic<void*>              g_original_closesocket{nullptr};

// Target address for the Scout telemetry socket. The loopback filter
// accepts IPv4 127.0.0.1:<port>. Both stored in network byte order so we
// can compare with sockaddr_in fields directly.
static std::atomic<std::uint16_t>      g_target_port_be{0};
static constexpr std::uint32_t         kLoopbackIpv4Be = 0x0100007Fu; // 127.0.0.1

// Per-socket classification cache. Value = 1 means the socket is peered
// at 127.0.0.1:<telemetry_port> (capture); 0 means anything else (ignore).
// Populated from two sources:
//   (a) hook_connect — when we see connect() going to the target, we mark
//       the socket 1 before the real connect returns.
//   (b) hook_send    — for sockets that predate install() (post-patch AIR
//       has Adobe's SocketTransport wired at startup, so its connect
//       happened before the IAT was patched), we fall back to getpeername
//       on the first send and cache the result.
// hook_closesocket removes the entry.
//
// The cache is protected by a mutex, but the send path's critical section
// is just a map lookup; the getpeername fallback runs at most once per
// non-monitored socket we've actually seen.
static std::mutex                           g_monitored_mu;
static std::unordered_map<SOCKET, bool>     g_monitored;

// Cumulative diagnostic counters. Incremented from hot-path hooks.
static std::atomic<std::uint64_t>  g_diag_send_calls{0};
static std::atomic<std::uint64_t>  g_diag_send_captured{0};
static std::atomic<std::uint64_t>  g_diag_connect_calls{0};
static std::atomic<std::uint64_t>  g_diag_connect_matched{0};
static std::atomic<std::uint64_t>  g_diag_close_calls{0};

// -- IAT walker --------------------------------------------------------------
//
// Walks Adobe AIR.dll's Import Directory to find the IAT slot for a
// specific imported function. Works identically for PE32 (x86) and PE32+
// (x64) because PIMAGE_THUNK_DATA is typedef'd differently per target at
// compile time.

static void** find_iat_slot(HMODULE module,
                            const char* import_dll,
                            unsigned ordinal,
                            const char* name_fallback) {
    if (module == nullptr) return nullptr;

    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(module);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    const auto& dir = nt->OptionalHeader
                        .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return nullptr;

    auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        base + dir.VirtualAddress);

    for (; desc->Name != 0; ++desc) {
        const char* dll_name = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(dll_name, import_dll) != 0) continue;

        const DWORD origThunkRva = desc->OriginalFirstThunk
                                    ? desc->OriginalFirstThunk
                                    : desc->FirstThunk;
        auto* hint = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + origThunkRva);
        auto* iat  = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);

        for (; hint->u1.AddressOfData != 0; ++hint, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(hint->u1.Ordinal)) {
                if (static_cast<unsigned>(IMAGE_ORDINAL(hint->u1.Ordinal)) == ordinal) {
                    return reinterpret_cast<void**>(&iat->u1.Function);
                }
            } else if (name_fallback != nullptr) {
                auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                    base + hint->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(ibn->Name),
                                name_fallback) == 0) {
                    return reinterpret_cast<void**>(&iat->u1.Function);
                }
            }
        }
        // Module matched but the import wasn't there — stop, no point
        // walking further modules (they won't have this import either).
        return nullptr;
    }
    return nullptr;
}

// Atomically swap a single IAT slot to `new_fn`, capturing the old pointer
// in *out_original. Wraps the VirtualProtect dance. Returns false on any
// syscall failure, in which case the slot is left untouched.
static bool patch_iat_slot(void** slot, void* new_fn, void** out_original) {
    if (slot == nullptr) return false;
    DWORD old_prot = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_prot)) {
        return false;
    }
    *out_original = *slot;
    *slot = new_fn;
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), old_prot, &tmp);
    return true;
}

static void restore_iat_slot(void** slot, void* original) {
    if (slot == nullptr || original == nullptr) return;
    DWORD old_prot = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_prot)) {
        return;
    }
    *slot = original;
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), old_prot, &tmp);
}

// -- the hooks --------------------------------------------------------------

// ws2_32!connect signature: int WSAAPI connect(SOCKET, const sockaddr*, int)
//
// We forward first and only mark the socket as monitored after a successful
// (or pending, for non-blocking sockets) call. The AIR telemetry transport
// is blocking in Mode B; treating WSAEWOULDBLOCK as "in flight" is just
// defensive against future runtime changes.
static int WSAAPI hook_connect(SOCKET s, const sockaddr* name, int namelen) {
    using ConnectFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);
    auto* real = reinterpret_cast<ConnectFn>(
        g_original_connect.load(std::memory_order_acquire));
    if (real == nullptr) return SOCKET_ERROR;

    g_diag_connect_calls.fetch_add(1, std::memory_order_relaxed);

    const int ret = real(s, name, namelen);
    const int last_err = (ret == 0) ? 0 : WSAGetLastError();
    const bool ok_or_pending = (ret == 0) || (last_err == WSAEWOULDBLOCK);

    const std::uint16_t target_port = g_target_port_be.load(std::memory_order_acquire);
    if (ok_or_pending && target_port != 0 &&
        name != nullptr && namelen >= static_cast<int>(sizeof(sockaddr_in)) &&
        name->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(name);
        const bool match = (in->sin_port == target_port &&
                            in->sin_addr.s_addr == kLoopbackIpv4Be);
        if (match) g_diag_connect_matched.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_monitored_mu);
        g_monitored[s] = match;
    }

    if (ret != 0) WSASetLastError(last_err);
    return ret;
}

// ws2_32!closesocket signature: int WSAAPI closesocket(SOCKET)
//
// Always remove from the set *before* calling the real closesocket so a
// racing send() on another thread can't capture bytes for a handle the
// kernel is actively reaping. (Winsock lets other threads write until
// closesocket returns; a stale entry could briefly mis-capture.)
static int WSAAPI hook_closesocket(SOCKET s) {
    using CloseFn = int(WSAAPI*)(SOCKET);
    auto* real = reinterpret_cast<CloseFn>(
        g_original_closesocket.load(std::memory_order_acquire));
    if (real == nullptr) return SOCKET_ERROR;

    g_diag_close_calls.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(g_monitored_mu);
        g_monitored.erase(s);
    }
    return real(s);
}

// ws2_32!send signature: int WSAAPI send(SOCKET, const char*, int, int)
// On x86 WSAAPI is __stdcall; on x64 the ABI is implicit. Both work by
// declaring the same WSAAPI macro as the real import.
static int WSAAPI hook_send(SOCKET s, const char* buf, int len, int flags) {
    using SendFn = int(WSAAPI*)(SOCKET, const char*, int, int);
    auto* real = reinterpret_cast<SendFn>(g_original_send.load(std::memory_order_acquire));

    // Defensive: if the hook is somehow invoked without original bound,
    // make a best effort to behave like send() succeeded.
    if (real == nullptr) return len;

    g_diag_send_calls.fetch_add(1, std::memory_order_relaxed);

    const int sent = real(s, buf, len, flags);

    if (sent > 0 && buf != nullptr) {
        const std::uint16_t target_port = g_target_port_be.load(std::memory_order_acquire);
        bool monitored = false;

        if (target_port == 0) {
            // Filter disabled — capture everything (legacy behavior).
            monitored = true;
        } else {
            bool cached = false;
            {
                std::lock_guard<std::mutex> lk(g_monitored_mu);
                auto it = g_monitored.find(s);
                if (it != g_monitored.end()) {
                    monitored = it->second;
                    cached = true;
                }
            }
            if (!cached) {
                // First time we've seen this socket. It could have been
                // created before install() (post-patch AIR wires Adobe's
                // SocketTransport at startup, so its connect predates the
                // hook). Classify now via getpeername and cache the result.
                sockaddr_storage ss{};
                int sslen = static_cast<int>(sizeof(ss));
                const int gp = ::getpeername(s, reinterpret_cast<sockaddr*>(&ss), &sslen);
                bool match = false;
                if (gp == 0 && ss.ss_family == AF_INET) {
                    const auto* in = reinterpret_cast<const sockaddr_in*>(&ss);
                    match = (in->sin_port == target_port &&
                             in->sin_addr.s_addr == kLoopbackIpv4Be);
                }
                std::lock_guard<std::mutex> lk(g_monitored_mu);
                g_monitored[s] = match;
                monitored = match;
            }
        }

        if (monitored) {
            g_diag_send_captured.fetch_add(static_cast<std::uint64_t>(sent),
                                           std::memory_order_relaxed);

            // Snapshot the session prologue once per process. On post-patch
            // AIR the telemetry session is single and process-wide, so we
            // can only observe the setup bytes at the head of the very
            // first stream; subsequent .flmc replay them so every file is
            // a self-contained capture.
            PrologueBuffer::instance().append(buf, static_cast<std::size_t>(sent));

            CaptureController* ctrl = g_controller.load(std::memory_order_acquire);
            if (ctrl != nullptr) {
                // push_bytes checks state internally; when Idle it just bumps
                // a drop counter — near-zero cost for the typical case.
                ctrl->push_bytes(buf, static_cast<std::uint32_t>(sent));
            }
        }
    }
    return sent;
}

// -- public interface -------------------------------------------------------

std::uint64_t WindowsRuntimeHook::diagSendCalls() const {
    return g_diag_send_calls.load(std::memory_order_relaxed);
}
std::uint64_t WindowsRuntimeHook::diagSendCaptured() const {
    return g_diag_send_captured.load(std::memory_order_relaxed);
}
std::uint64_t WindowsRuntimeHook::diagConnectCalls() const {
    return g_diag_connect_calls.load(std::memory_order_relaxed);
}
std::uint64_t WindowsRuntimeHook::diagConnectMatched() const {
    return g_diag_connect_matched.load(std::memory_order_relaxed);
}
std::uint64_t WindowsRuntimeHook::diagCloseCalls() const {
    return g_diag_close_calls.load(std::memory_order_relaxed);
}

WindowsRuntimeHook::~WindowsRuntimeHook() {
    uninstall();
}

bool WindowsRuntimeHook::install(CaptureController* controller,
                                 std::uint16_t telemetry_port) {
    if (installed_) {
        // Already installed — just re-point the controller + port. The set
        // of monitored sockets persists (the runtime's telemetry socket
        // stays connected across profiler_start/stop cycles).
        g_controller.store(controller, std::memory_order_release);
        g_target_port_be.store(htons(telemetry_port), std::memory_order_release);
        return true;
    }
    if (controller == nullptr) return false;

    HMODULE air = GetModuleHandleW(L"Adobe AIR.dll");
    if (air == nullptr) return false;

    void** send_slot        = find_iat_slot(air, "ws2_32.dll", 19, "send");
    void** connect_slot     = find_iat_slot(air, "ws2_32.dll",  4, "connect");
    void** closesocket_slot = find_iat_slot(air, "ws2_32.dll",  3, "closesocket");
    if (send_slot == nullptr || connect_slot == nullptr || closesocket_slot == nullptr) {
        return false;
    }

    g_controller.store(controller, std::memory_order_release);
    g_target_port_be.store(htons(telemetry_port), std::memory_order_release);

    // Patch connect + closesocket before send: a stray send between the
    // first and last patch on the target socket would otherwise be missed
    // (if connect fires in between) or, worse, captured unfiltered.
    void* orig_connect = nullptr;
    if (!patch_iat_slot(connect_slot, reinterpret_cast<void*>(&hook_connect),
                        &orig_connect)) {
        g_controller.store(nullptr, std::memory_order_release);
        g_target_port_be.store(0, std::memory_order_release);
        return false;
    }
    g_original_connect.store(orig_connect, std::memory_order_release);

    void* orig_closesocket = nullptr;
    if (!patch_iat_slot(closesocket_slot, reinterpret_cast<void*>(&hook_closesocket),
                        &orig_closesocket)) {
        restore_iat_slot(connect_slot, orig_connect);
        g_original_connect.store(nullptr, std::memory_order_release);
        g_controller.store(nullptr, std::memory_order_release);
        g_target_port_be.store(0, std::memory_order_release);
        return false;
    }
    g_original_closesocket.store(orig_closesocket, std::memory_order_release);

    void* orig_send = nullptr;
    if (!patch_iat_slot(send_slot, reinterpret_cast<void*>(&hook_send),
                        &orig_send)) {
        restore_iat_slot(closesocket_slot, orig_closesocket);
        restore_iat_slot(connect_slot,     orig_connect);
        g_original_closesocket.store(nullptr, std::memory_order_release);
        g_original_connect.store(nullptr, std::memory_order_release);
        g_controller.store(nullptr, std::memory_order_release);
        g_target_port_be.store(0, std::memory_order_release);
        return false;
    }
    g_original_send.store(orig_send, std::memory_order_release);

    send_slot_            = send_slot;
    connect_slot_         = connect_slot;
    closesocket_slot_     = closesocket_slot;
    original_send_        = orig_send;
    original_connect_     = orig_connect;
    original_closesocket_ = orig_closesocket;

    installed_ = true;
    return true;
}

void WindowsRuntimeHook::uninstall() {
    if (!installed_) return;

    // Reverse of install: restore send first so new sends go straight
    // through the real function while we unhook the bookkeeping pair.
    restore_iat_slot(send_slot_,        original_send_);
    restore_iat_slot(closesocket_slot_, original_closesocket_);
    restore_iat_slot(connect_slot_,     original_connect_);

    {
        std::lock_guard<std::mutex> lk(g_monitored_mu);
        g_monitored.clear();
    }

    g_controller.store(nullptr,           std::memory_order_release);
    g_original_send.store(nullptr,        std::memory_order_release);
    g_original_connect.store(nullptr,     std::memory_order_release);
    g_original_closesocket.store(nullptr, std::memory_order_release);
    g_target_port_be.store(0,             std::memory_order_release);

    send_slot_            = nullptr;
    connect_slot_         = nullptr;
    closesocket_slot_     = nullptr;
    original_send_        = nullptr;
    original_connect_     = nullptr;
    original_closesocket_ = nullptr;
    installed_            = false;
}

} // namespace ane::profiler

// Factory for IRuntimeHook::create(). On Windows we return a
// WindowsRuntimeHook. Other platforms provide their own create().
namespace ane::profiler {
std::unique_ptr<IRuntimeHook> IRuntimeHook::create() {
    return std::unique_ptr<IRuntimeHook>(new WindowsRuntimeHook());
}
} // namespace ane::profiler
