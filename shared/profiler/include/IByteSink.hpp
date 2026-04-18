// Abstract sink for compressed/uncompressed byte output. The writer thread
// feeds bytes to the sink; the sink owns the underlying file handle. The
// interface is minimal on purpose so `RawFileSink` stays trivial and the
// future `ZlibFileSink` can be swapped in without touching higher layers.

#ifndef ANE_PROFILER_I_BYTE_SINK_HPP
#define ANE_PROFILER_I_BYTE_SINK_HPP

#include <cstddef>
#include <cstdint>

namespace ane::profiler {

class IByteSink {
public:
    virtual ~IByteSink() = default;

    // Append `n` bytes. Returns true on success.
    virtual bool write(const void* data, std::size_t n) = 0;

    // Force any buffered data out to the underlying file. Caller should
    // call this periodically to bound data loss on crash.
    virtual void flush() = 0;

    // Close the underlying file. After this, `write()` returns false.
    virtual void close() = 0;

    // Total payload bytes accepted (post-compression view: bytes_out).
    virtual std::uint64_t bytes_in() const = 0;

    // Bytes actually written to the file after compression. For raw sinks
    // this equals bytes_in.
    virtual std::uint64_t bytes_out() const = 0;
};

} // namespace ane::profiler

#endif // ANE_PROFILER_I_BYTE_SINK_HPP
