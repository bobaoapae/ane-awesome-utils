// PrologueBuffer — process-wide cache of the AMF3 session prologue so every
// .flmc we emit is a self-contained, replayable capture.
//
// Problem it solves:
//   Scout's wire protocol is a single long-running AMF3 session with internal
//   string and trait tables that grow monotonically over the process. The
//   first N bytes out of the AIR runtime set up those tables (`.tlm.version`,
//   `.tlm.meta`, `.tlm.date`, `.player.*` schema, common trait definitions).
//   Everything sent afterwards references those tables by index. If a .flmc
//   captured mid-session starts with refs, no parser can resolve them unless
//   it first saw the prologue.
//
//   On post-patch AIR we can't tear down Adobe's SocketTransport between
//   profilerStart/Stop cycles (doing so errors the socket and no bytes ever
//   flow again). So every .flmc after the first one starts mid-session.
//
// Strategy:
//   The send() hook copies the first `kMaxBytes` bytes it ever sees on the
//   telemetry socket into this buffer, once per process. CaptureController
//   then `replay()`s those bytes at the start of every new .flmc (after the
//   file header, before live deflate of the current-session stream). The
//   parser sees every file open with the same tables being (re-)defined, so
//   all subsequent refs resolve.
//
//   The same definitions appearing at the head of every file is not wrong
//   AMF3 — inline string/trait markers just overwrite (or no-op-redefine)
//   the existing table entries. Cost is a fixed prefix per .flmc.
//
// Threading:
//   - `append()` is called from the IAT hook thread (possibly many threads
//     in parallel if AIR has multiple telemetry emitters — in practice it
//     is a single send pump, but we lock anyway).
//   - `replay()` is called once per CaptureController::start() on the ANE
//     bridge thread while no writer thread is live yet.
//   - Both are guarded by a mutex; the hot path (`append`) short-circuits
//     with a relaxed load once the buffer is full.

#ifndef ANE_PROFILER_PROLOGUE_BUFFER_HPP
#define ANE_PROFILER_PROLOGUE_BUFFER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace ane::profiler {

class PrologueBuffer {
public:
    // How many bytes of prologue we keep. 512 KiB captures the full
    // Scout setup phase — `.tlm.*` meta, `.player.*`, all `.span.*`,
    // `.gc.*`, `.rend.*`, `.mem.*`, `.memory.*`, `.sampler.sample`,
    // `.tlm.doplay`, `.enter` trait definitions plus the early
    // method-name-map chunk. Measured empirically: 64 KiB was too small
    // (sampler.sample and many `.rend.*` / `.tlm.doplay` emitted inline
    // past 64 KiB left files N>=1 unable to resolve those refs). At 512
    // KiB every tag observed in run A also appears as an inline
    // definition in runs B and C.
    //
    // Cost: 512 KiB prepended raw to every .flmc; at the observed ~0.2
    // zlib ratio that is ~100 KiB on disk per file — negligible vs the
    // MB-scale live stream.
    static constexpr std::size_t kMaxBytes = 512 * 1024;

    static PrologueBuffer& instance() {
        static PrologueBuffer inst;
        return inst;
    }

    // Copy up to (kMaxBytes - already-buffered) bytes from `data` into the
    // cache. Once the cache is full, subsequent calls short-circuit to a
    // relaxed load — near-zero hot-path cost.
    void append(const void* data, std::size_t n) noexcept {
        if (full_.load(std::memory_order_relaxed)) return;
        if (n == 0 || data == nullptr) return;
        std::lock_guard<std::mutex> lk(mu_);
        if (buf_.size() >= kMaxBytes) {
            full_.store(true, std::memory_order_relaxed);
            return;
        }
        const std::size_t room = kMaxBytes - buf_.size();
        const std::size_t take = (n < room) ? n : room;
        buf_.insert(buf_.end(),
                    static_cast<const std::uint8_t*>(data),
                    static_cast<const std::uint8_t*>(data) + take);
        if (buf_.size() >= kMaxBytes) {
            full_.store(true, std::memory_order_relaxed);
        }
    }

    // Copy the cached bytes into `out` (resets `out` first). Safe to call
    // concurrently with append(); the snapshot may lag a call or two, which
    // is fine — replay only needs a consistent prefix of the definitions.
    void snapshot(std::vector<std::uint8_t>& out) const {
        std::lock_guard<std::mutex> lk(mu_);
        out.assign(buf_.begin(), buf_.end());
    }

    std::size_t size() const noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        return buf_.size();
    }

    // Reset the cache. Meant for testing and for a future "profilerStart
    // with fresh AMF3 session" path on pre-patch AIR; not called on the
    // post-patch attach path (the AIR session is single and process-wide).
    void clear() noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        buf_.clear();
        full_.store(false, std::memory_order_relaxed);
    }

private:
    PrologueBuffer() { buf_.reserve(kMaxBytes); }
    PrologueBuffer(const PrologueBuffer&) = delete;
    PrologueBuffer& operator=(const PrologueBuffer&) = delete;

    mutable std::mutex            mu_;
    std::vector<std::uint8_t>     buf_;
    std::atomic<bool>             full_{false};
};

} // namespace ane::profiler

#endif // ANE_PROFILER_PROLOGUE_BUFFER_HPP
