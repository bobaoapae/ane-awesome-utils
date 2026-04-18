#include "CaptureController.hpp"

#include "FileFormat.hpp"
#include "miniz.h"

#include <algorithm>
#include <ctime>

namespace ane::profiler {

namespace {
// We pull records out of the ring in bursts up to this many bytes. Trade-off:
// larger = fewer loops / better compression ratio; smaller = lower latency.
constexpr std::size_t kScratchInBytes = 64 * 1024;
// Miniz output buffer; deflate emits up to this many bytes per produce pass.
constexpr std::size_t kScratchOutBytes = 64 * 1024;
} // namespace

CaptureController::CaptureController() = default;

CaptureController::~CaptureController() {
    stop();
}

bool CaptureController::start(const Config& cfg) {
    std::lock_guard<std::mutex> g(lifecycle_mutex_);
    if (state_.load(std::memory_order_acquire) != State::Idle) return false;

    state_.store(State::Starting, std::memory_order_release);
    cfg_ = cfg;

    // Reset counters.
    bytes_in_.store(0);
    bytes_accepted_.store(0);
    bytes_out_.store(0);
    record_count_.store(0);
    drop_count_.store(0);
    drop_bytes_.store(0);
    writer_stop_.store(false);

    // Allocate the ring.
    try {
        stream_ = std::make_unique<SpscByteStream>(cfg.ring_capacity);
    } catch (...) {
        state_.store(State::Error, std::memory_order_release);
        return false;
    }

    // Open the output file.
    file_.open(cfg.output_path, std::ios::binary | std::ios::trunc);
    if (!file_.is_open()) {
        state_.store(State::Error, std::memory_order_release);
        stream_.reset();
        return false;
    }

    // Write header (magic + version + header_len + JSON).
    const auto& hj = cfg.header_json;
    auto hbytes = fileformat::make_header_bytes(static_cast<std::uint32_t>(hj.size()));
    file_.write(reinterpret_cast<const char*>(hbytes.data()),
                static_cast<std::streamsize>(hbytes.size()));
    if (!hj.empty()) {
        file_.write(hj.data(), static_cast<std::streamsize>(hj.size()));
    }
    if (!file_) {
        state_.store(State::Error, std::memory_order_release);
        file_.close();
        stream_.reset();
        return false;
    }

    // Initialize deflate state.
    auto* z = new mz_stream();
    *z = mz_stream{};
    int rc = mz_deflateInit(z, cfg.compression_level);
    if (rc != MZ_OK) {
        delete z;
        state_.store(State::Error, std::memory_order_release);
        file_.close();
        stream_.reset();
        return false;
    }
    zstream_ = z;

    scratch_in_  = std::unique_ptr<std::uint8_t[]>(new std::uint8_t[kScratchInBytes]);
    scratch_out_.assign(kScratchOutBytes, 0);

    started_at_ = std::chrono::steady_clock::now();

    // Launch writer thread.
    writer_ = std::thread(&CaptureController::writer_main, this);
    state_.store(State::Recording, std::memory_order_release);
    return true;
}

void CaptureController::stop() {
    std::lock_guard<std::mutex> g(lifecycle_mutex_);
    const State s = state_.load(std::memory_order_acquire);
    if (s == State::Idle) return;
    if (s != State::Recording && s != State::Error && s != State::Starting) {
        // Already Stopping in another thread, or in an unexpected state.
        return;
    }
    state_.store(State::Stopping, std::memory_order_release);

    writer_stop_.store(true, std::memory_order_release);
    if (writer_.joinable()) {
        writer_.join();
    }

    // Drain any bytes the writer missed. With a clean stop, the writer
    // has already looped once after seeing stop and drained the ring —
    // but do it defensively here too.
    if (stream_ && zstream_ != nullptr) {
        std::uint8_t buf[4096];
        while (!stream_->empty()) {
            std::size_t got = stream_->try_pop(buf, sizeof(buf));
            if (got == 0 || got == SpscByteStream::kTooLarge) break;
            deflate_chunk(buf, got, MZ_NO_FLUSH);
        }
    }

    // Flush deflate final block and close the zlib stream.
    if (zstream_ != nullptr) {
        deflate_chunk(nullptr, 0, MZ_FINISH);
        auto* z = static_cast<mz_stream*>(zstream_);
        mz_deflateEnd(z);
        delete z;
        zstream_ = nullptr;
    }

    // Write footer.
    if (file_.is_open()) {
        // Compute CRC over the file's compressed region. For simplicity we
        // record 0 here — the footer CRC field is advisory. A future
        // improvement: tee CRC during deflate to avoid a re-read.
        std::uint64_t ended_utc = static_cast<std::uint64_t>(std::time(nullptr));
        auto footer = fileformat::make_footer_bytes(
            bytes_accepted_.load(std::memory_order_acquire),
            bytes_out_.load(std::memory_order_acquire),
            record_count_.load(std::memory_order_acquire),
            drop_count_.load(std::memory_order_acquire),
            ended_utc,
            0u);
        file_.write(reinterpret_cast<const char*>(footer.data()),
                    static_cast<std::streamsize>(footer.size()));
        file_.flush();
        file_.close();
    }

    stream_.reset();
    scratch_in_.reset();
    scratch_out_.clear();
    scratch_out_.shrink_to_fit();

    state_.store(State::Idle, std::memory_order_release);
}

bool CaptureController::push_bytes(const void* data, std::uint32_t n) {
    bytes_in_.fetch_add(n, std::memory_order_relaxed);
    if (state_.load(std::memory_order_acquire) != State::Recording) {
        drop_count_.fetch_add(1, std::memory_order_relaxed);
        drop_bytes_.fetch_add(n, std::memory_order_relaxed);
        return false;
    }
    if (stream_ && stream_->try_push(data, n)) {
        bytes_accepted_.fetch_add(n, std::memory_order_relaxed);
        record_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    drop_count_.fetch_add(1, std::memory_order_relaxed);
    drop_bytes_.fetch_add(n, std::memory_order_relaxed);
    return false;
}

CaptureController::Status CaptureController::status() const {
    Status s{};
    s.state          = state_.load(std::memory_order_acquire);
    s.bytes_in       = bytes_in_.load(std::memory_order_relaxed);
    s.bytes_accepted = bytes_accepted_.load(std::memory_order_relaxed);
    s.bytes_out      = bytes_out_.load(std::memory_order_relaxed);
    s.record_count   = record_count_.load(std::memory_order_relaxed);
    s.drop_count     = drop_count_.load(std::memory_order_relaxed);
    s.drop_bytes     = drop_bytes_.load(std::memory_order_relaxed);
    if (s.state == State::Recording || s.state == State::Stopping) {
        auto now = std::chrono::steady_clock::now();
        s.elapsed_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started_at_).count());
    } else {
        s.elapsed_ms = 0;
    }
    return s;
}

void CaptureController::writer_main() {
    // Periodic fsync cadence — bound the amount of data we lose on crash /
    // force-kill. Every ~500 ms we flush the ofstream's internal buffer to
    // the OS.
    auto last_flush = std::chrono::steady_clock::now();
    constexpr auto kFlushEvery = std::chrono::milliseconds(500);

    while (true) {
        const bool should_stop = writer_stop_.load(std::memory_order_acquire);

        std::uint8_t* in = scratch_in_.get();
        // Drain up to kScratchInBytes worth of records per loop.
        std::size_t used = 0;
        while (used < kScratchInBytes) {
            std::size_t avail = kScratchInBytes - used;
            std::size_t got = stream_->try_pop(in + used, avail);
            if (got == 0) break;
            if (got == SpscByteStream::kTooLarge) {
                // Next record is bigger than our scratch room — flush what we
                // have, then grow a one-shot buffer and handle it.
                break;
            }
            used += got;
        }

        if (used > 0) {
            deflate_chunk(in, used, MZ_NO_FLUSH);
        }

        // Handle a too-large record: peek then allocate a single-purpose buf.
        std::uint32_t peek = stream_->peek_length();
        if (peek > kScratchInBytes) {
            std::vector<std::uint8_t> big(peek);
            std::size_t got = stream_->try_pop(big.data(), big.size());
            if (got == big.size()) {
                deflate_chunk(big.data(), big.size(), MZ_NO_FLUSH);
            }
        }

        // Hard byte-out cap.
        if (cfg_.max_bytes_out != 0 &&
            bytes_out_.load(std::memory_order_relaxed) >= cfg_.max_bytes_out) {
            writer_stop_.store(true, std::memory_order_release);
        }

        // Periodic flush — survives a hard kill by getting data out of the
        // ofstream's internal buffer and into the OS/disk cache.
        const auto now = std::chrono::steady_clock::now();
        if (now - last_flush >= kFlushEvery) {
            // Also emit a deflate sync-flush so the zlib stream is
            // decodable up to this point even without a proper finish.
            deflate_chunk(nullptr, 0, MZ_SYNC_FLUSH);
            file_.flush();
            last_flush = now;
        }

        if (should_stop && used == 0 && stream_->empty()) {
            break;
        }
        if (used == 0) {
            std::this_thread::sleep_for(cfg_.writer_idle_sleep);
        }
    }
}

bool CaptureController::deflate_chunk(const void* data, std::size_t n, int flush_mode) {
    if (zstream_ == nullptr) return false;
    auto* z = static_cast<mz_stream*>(zstream_);

    z->next_in  = static_cast<const unsigned char*>(data);
    z->avail_in = static_cast<unsigned int>(n);

    for (;;) {
        z->next_out  = scratch_out_.data();
        z->avail_out = static_cast<unsigned int>(scratch_out_.size());

        int rc = mz_deflate(z, flush_mode);
        const std::size_t produced = scratch_out_.size() - z->avail_out;
        if (produced > 0) {
            file_.write(reinterpret_cast<const char*>(scratch_out_.data()),
                        static_cast<std::streamsize>(produced));
            if (!file_) return false;
            bytes_out_.fetch_add(produced, std::memory_order_relaxed);
        }

        if (rc == MZ_STREAM_END) return true;
        if (rc == MZ_BUF_ERROR && z->avail_in == 0 && flush_mode == MZ_NO_FLUSH) return true;
        if (rc != MZ_OK) return false;
        if (z->avail_in == 0 && z->avail_out != 0) return true;
    }
}

} // namespace ane::profiler
