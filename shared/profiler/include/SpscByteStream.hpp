// Single-producer / single-consumer lock-free byte stream.
//
// Used as the main-thread-to-writer-thread handoff for the profiler:
//   Producer (AIR runtime main thread, inside our send() hook):
//     stream.try_push(buf, len)
//   Consumer (our WriterThread):
//     stream.try_pop(...) in a loop
//
// Layout on disk (wire order inside the circular storage):
//   record := u32 length  ||  u8 payload[length]
//
// Capacity must be a power of two. Wrap-around is handled transparently in
// both read and write paths. Never blocks; `try_push` returns false when the
// arena is full, so the producer can bump a dropped-counter without ever
// stalling the runtime.
//
// Thread-safety: exactly one producer thread and exactly one consumer
// thread. Concurrent producers or consumers require external serialization.

#ifndef ANE_PROFILER_SPSC_BYTE_STREAM_HPP
#define ANE_PROFILER_SPSC_BYTE_STREAM_HPP

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace ane::profiler {

class SpscByteStream {
public:
    // `capacity` must be a power of two and >= 64. Total RAM usage ~= capacity.
    explicit SpscByteStream(std::size_t capacity)
        : capacity_(capacity), mask_(capacity - 1),
          storage_(new std::uint8_t[capacity]) {
        assert(capacity >= 64);
        assert((capacity & (capacity - 1)) == 0 && "capacity must be power of 2");
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    SpscByteStream(const SpscByteStream&) = delete;
    SpscByteStream& operator=(const SpscByteStream&) = delete;

    std::size_t capacity() const noexcept { return capacity_; }

    // Number of payload+header bytes queued (not yet consumed).
    std::size_t used_bytes() const noexcept {
        const std::uint64_t h = head_.load(std::memory_order_acquire);
        const std::uint64_t t = tail_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(h - t);
    }

    // True if there are no pending records.
    bool empty() const noexcept { return used_bytes() == 0; }

    // Attempt to enqueue a record of `len` bytes from `data`.
    // Non-blocking. Returns false if insufficient free space.
    bool try_push(const void* data, std::uint32_t len) noexcept {
        const std::uint64_t h = head_.load(std::memory_order_relaxed);
        const std::uint64_t t = tail_.load(std::memory_order_acquire);
        const std::size_t needed = kHeaderSize + len;
        if (h + needed > t + capacity_) {
            return false; // would overflow
        }
        const std::uint32_t lenLe = to_le32(len);
        write_at(h, &lenLe, kHeaderSize);
        write_at(h + kHeaderSize, data, len);
        head_.store(h + needed, std::memory_order_release);
        return true;
    }

    // Pop one record into `out`, writing up to `max_out` bytes. Returns the
    // actual record length on success, 0 if the queue is empty, or
    // kTooLarge (= std::size_t(-1)) if `max_out` is smaller than the next
    // record. In the latter case nothing is consumed.
    static constexpr std::size_t kTooLarge = static_cast<std::size_t>(-1);

    std::size_t try_pop(void* out, std::size_t max_out) noexcept {
        const std::uint64_t t = tail_.load(std::memory_order_relaxed);
        const std::uint64_t h = head_.load(std::memory_order_acquire);
        if (t == h) return 0;
        assert(h - t >= kHeaderSize);

        std::uint32_t lenLe = 0;
        read_at(t, &lenLe, kHeaderSize);
        const std::uint32_t len = from_le32(lenLe);
        if (len > max_out) return kTooLarge;

        assert(h - t >= kHeaderSize + len);
        read_at(t + kHeaderSize, out, len);
        tail_.store(t + kHeaderSize + len, std::memory_order_release);
        return len;
    }

    // Peek at the next record's length without consuming. Returns 0 if empty.
    std::uint32_t peek_length() const noexcept {
        const std::uint64_t t = tail_.load(std::memory_order_relaxed);
        const std::uint64_t h = head_.load(std::memory_order_acquire);
        if (t == h) return 0;
        std::uint32_t lenLe = 0;
        read_at(t, &lenLe, kHeaderSize);
        return from_le32(lenLe);
    }

private:
    static constexpr std::size_t kHeaderSize = sizeof(std::uint32_t);

    // Host-endian serialization. We always store little-endian on the wire
    // so dumps are portable between host architectures (Windows x64 is LE,
    // Android arm64 is LE — both trivial).
    static std::uint32_t to_le32(std::uint32_t v) noexcept { return v; }
    static std::uint32_t from_le32(std::uint32_t v) noexcept { return v; }

    void write_at(std::uint64_t pos, const void* src, std::size_t n) noexcept {
        const std::size_t offset = static_cast<std::size_t>(pos) & mask_;
        const std::size_t first = (n < capacity_ - offset) ? n : (capacity_ - offset);
        std::memcpy(storage_.get() + offset, src, first);
        if (first < n) {
            std::memcpy(storage_.get(),
                        static_cast<const std::uint8_t*>(src) + first,
                        n - first);
        }
    }

    void read_at(std::uint64_t pos, void* dst, std::size_t n) const noexcept {
        const std::size_t offset = static_cast<std::size_t>(pos) & mask_;
        const std::size_t first = (n < capacity_ - offset) ? n : (capacity_ - offset);
        std::memcpy(dst, storage_.get() + offset, first);
        if (first < n) {
            std::memcpy(static_cast<std::uint8_t*>(dst) + first,
                        storage_.get(),
                        n - first);
        }
    }

    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<std::uint8_t[]> storage_;
    alignas(64) std::atomic<std::uint64_t> head_;
    alignas(64) std::atomic<std::uint64_t> tail_;
};

} // namespace ane::profiler

#endif // ANE_PROFILER_SPSC_BYTE_STREAM_HPP
