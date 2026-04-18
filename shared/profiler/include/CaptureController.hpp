// CaptureController — top-level entry point for the profiler core.
//
// Owns:
//   - An SpscByteStream that the AIR-runtime-side hook pushes Scout bytes into
//   - The output .flmc file (header + zlib-compressed stream + footer)
//   - A dedicated writer thread that drains the ring and compresses into
//     the file in real time
//   - Usage counters (bytes in / out, dropped, elapsed)
//
// Thread model:
//   - start() and stop() run on the caller's thread (AS3/ANE bridge thread)
//   - push_bytes() runs on the hook thread (main AIR thread), lock-free
//   - The writer thread is spawned internally by start() and joined by stop()
//
// File shape (see FileFormat.hpp for exact bytes):
//   [Header 12B][Header JSON][compressed deflate stream][Footer 64B]

#ifndef ANE_PROFILER_CAPTURE_CONTROLLER_HPP
#define ANE_PROFILER_CAPTURE_CONTROLLER_HPP

#include "SpscByteStream.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ane::profiler {

class CaptureController {
public:
    struct Config {
        // Absolute path where the .flmc file will be written. Truncated on start.
        std::string output_path;

        // Arbitrary JSON blob that goes into the file header. Caller decides
        // the schema; `flmc-validate` expects at least {"compression":...,
        // "wire_protocol":"scout-amf3"} — provide those.
        std::string header_json;

        // Ring capacity (bytes). Must be power of two. Default 4 MiB is
        // enough for ~8 s of peak Scout traffic at 500 KB/s.
        std::size_t ring_capacity = 4 * 1024 * 1024;

        // Stop automatically once this many bytes have been written to the
        // .flmc file. 0 = no cap. Default 900 MB (under WhatsApp's 1 GB limit).
        std::uint64_t max_bytes_out = 900ull * 1024ull * 1024ull;

        // Deflate level 1..9. 6 is zlib default.
        int compression_level = 6;

        // Writer thread sleep when ring is empty. 2 ms keeps latency low
        // without pegging the core. Don't raise this above ~10 ms without
        // thinking about ring capacity.
        std::chrono::milliseconds writer_idle_sleep{2};
    };

    enum class State : std::uint32_t {
        Idle      = 0,
        Starting  = 1,
        Recording = 2,
        Stopping  = 3,
        Error     = 4,
    };

    struct Status {
        State         state;
        std::uint64_t bytes_in;          // bytes handed to push_bytes()
        std::uint64_t bytes_accepted;    // bytes actually enqueued (push ok)
        std::uint64_t bytes_out;         // bytes written to file (compressed)
        std::uint64_t record_count;      // records enqueued
        std::uint64_t drop_count;        // pushes rejected due to backpressure
        std::uint64_t drop_bytes;        // bytes dropped (size of rejected pushes)
        std::uint64_t elapsed_ms;
    };

    CaptureController();
    ~CaptureController();

    CaptureController(const CaptureController&) = delete;
    CaptureController& operator=(const CaptureController&) = delete;

    // Begin a capture. Idempotent: returns false if state != Idle.
    bool start(const Config& cfg);

    // End the capture gracefully. Writes the footer and closes the file.
    // Safe to call from any thread. Returns only when everything is flushed.
    void stop();

    State  state()  const noexcept { return state_.load(std::memory_order_acquire); }
    Status status() const;

    // Enqueue `n` bytes observed at the hook. Non-blocking; on ring-full
    // returns false and increments the drop counter. Caller should always
    // return back to the runtime regardless of success.
    bool push_bytes(const void* data, std::uint32_t n);

private:
    void writer_main();
    bool deflate_chunk(const void* data, std::size_t n, int flush_mode);

    Config                            cfg_;
    std::atomic<State>                state_{State::Idle};

    std::unique_ptr<SpscByteStream>   stream_;
    std::ofstream                     file_;
    // miniz's `mz_stream` is kept as void* so the header doesn't leak miniz.h
    void*                             zstream_ = nullptr;
    std::unique_ptr<std::uint8_t[]>   scratch_in_;
    std::vector<std::uint8_t>         scratch_out_;

    std::thread                       writer_;
    std::atomic<bool>                 writer_stop_{false};

    // Counters (relaxed is fine; we never need strict happens-before for them).
    std::atomic<std::uint64_t>        bytes_in_{0};
    std::atomic<std::uint64_t>        bytes_accepted_{0};
    std::atomic<std::uint64_t>        bytes_out_{0};
    std::atomic<std::uint64_t>        record_count_{0};
    std::atomic<std::uint64_t>        drop_count_{0};
    std::atomic<std::uint64_t>        drop_bytes_{0};
    std::chrono::steady_clock::time_point started_at_{};

    // Guards start()/stop() so they can't race on state transitions.
    std::mutex                        lifecycle_mutex_;
};

} // namespace ane::profiler

#endif // ANE_PROFILER_CAPTURE_CONTROLLER_HPP
