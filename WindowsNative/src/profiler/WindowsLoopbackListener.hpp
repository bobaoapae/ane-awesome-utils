// Minimal localhost TCP listener to satisfy the AIR runtime's connect()
// when the profiler's force-enabled Telemetry opens its socket. Without
// somebody listening on 127.0.0.1:<port>, connect() fails, the socket is
// marked errored, and the runtime never calls send() — which means our
// IAT hook never fires and we capture nothing.
//
// This listener accepts the runtime's socket, then silently drains the
// incoming bytes (we already captured them on the producer side via the
// send() hook, so the bytes here are discarded).
//
// Lifecycle:
//   start(port)  : binds+listens on 127.0.0.1:port, spawns accept/recv thread
//   stop()       : closes listener + peer, joins thread. Idempotent.

#ifndef ANE_PROFILER_WINDOWS_LOOPBACK_LISTENER_HPP
#define ANE_PROFILER_WINDOWS_LOOPBACK_LISTENER_HPP

#include <atomic>
#include <cstdint>
#include <thread>

namespace ane::profiler {

class WindowsLoopbackListener {
public:
    WindowsLoopbackListener() = default;
    ~WindowsLoopbackListener() { stop(); }

    WindowsLoopbackListener(const WindowsLoopbackListener&) = delete;
    WindowsLoopbackListener& operator=(const WindowsLoopbackListener&) = delete;

    bool start(std::uint16_t port);
    void stop();

    bool running() const { return running_.load(std::memory_order_acquire); }

    // Stats (for diagnostics).
    std::uint64_t accepted_bytes() const { return accepted_bytes_.load(std::memory_order_relaxed); }

private:
    void thread_main();

    // We keep SOCKET as uintptr_t to avoid pulling winsock2.h into the header.
    std::atomic<std::uintptr_t>  listen_sock_{0};
    std::atomic<std::uintptr_t>  peer_sock_{0};
    std::atomic<bool>            running_{false};
    std::atomic<std::uint64_t>   accepted_bytes_{0};
    std::thread                  worker_;
};

} // namespace ane::profiler

#endif // ANE_PROFILER_WINDOWS_LOOPBACK_LISTENER_HPP
