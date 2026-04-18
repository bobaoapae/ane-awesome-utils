#include "profiler/WindowsLoopbackListener.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

namespace ane::profiler {

bool WindowsLoopbackListener::start(std::uint16_t port) {
    if (running_.load(std::memory_order_acquire)) return false;

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);  // OK to re-call; ref-counted.

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    int yes = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        ::closesocket(s);
        return false;
    }
    if (::listen(s, 1) == SOCKET_ERROR) {
        ::closesocket(s);
        return false;
    }

    listen_sock_.store(static_cast<std::uintptr_t>(s), std::memory_order_release);
    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&WindowsLoopbackListener::thread_main, this);
    return true;
}

void WindowsLoopbackListener::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;

    // Close both sockets to unblock any accept() / recv() in the worker.
    auto ls = static_cast<SOCKET>(listen_sock_.exchange(0, std::memory_order_acq_rel));
    auto ps = static_cast<SOCKET>(peer_sock_.exchange(0, std::memory_order_acq_rel));
    if (ls != 0 && ls != INVALID_SOCKET) ::closesocket(ls);
    if (ps != 0 && ps != INVALID_SOCKET) ::closesocket(ps);

    if (worker_.joinable()) worker_.join();
}

void WindowsLoopbackListener::thread_main() {
    auto ls = static_cast<SOCKET>(listen_sock_.load(std::memory_order_acquire));
    if (ls == 0 || ls == INVALID_SOCKET) return;

    while (running_.load(std::memory_order_acquire)) {
        sockaddr_in peer{};
        int peer_len = sizeof(peer);
        SOCKET ps = ::accept(ls, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (ps == INVALID_SOCKET) {
            // Either stop() closed us, or a transient error — either way bail.
            break;
        }
        peer_sock_.store(static_cast<std::uintptr_t>(ps), std::memory_order_release);

        // Drain until EOF / error / stop. Small buffer — these are just
        // bytes we've already captured upstream.
        char buf[8192];
        for (;;) {
            int n = ::recv(ps, buf, sizeof(buf), 0);
            if (n <= 0) break;
            accepted_bytes_.fetch_add(static_cast<std::uint64_t>(n),
                                      std::memory_order_relaxed);
        }
        ::closesocket(ps);
        peer_sock_.store(0, std::memory_order_release);

        // Go back to accepting more connections (runtime may reconnect).
    }
}

} // namespace ane::profiler
