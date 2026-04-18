// Streaming zlib-format file sink.
//
// Uses miniz (bundled under third_party/) as the deflate implementation.
// Output on disk is the bytes you'd get from `zlib.compress(data)` in
// Python — i.e. the zlib-wrapped deflate stream. flash-profiler's Rust
// reader uses `flate2::read::ZlibDecoder` on the stream bytes, so this
// is bit-compatible out of the box.
//
// The sink owns its miniz deflator and the underlying file handle. Each
// `write()` call feeds into the deflator; deflated bytes are written out
// to disk as they are produced. Call `flush()` periodically to bound the
// amount of data that can be lost on crash; call `close()` at the end to
// finalize the zlib trailer.

#ifndef ANE_PROFILER_ZLIB_FILE_SINK_HPP
#define ANE_PROFILER_ZLIB_FILE_SINK_HPP

#include "IByteSink.hpp"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace ane::profiler {

class ZlibFileSink : public IByteSink {
public:
    // Level: 1 (fastest) .. 9 (smallest). Default 6 matches zlib's default.
    explicit ZlibFileSink(const std::string& path, int level = 6);
    ~ZlibFileSink() override;

    bool  write(const void* data, std::size_t n) override;
    void  flush() override;   // forces deflator flush + fstream flush
    void  close() override;   // finalizes zlib trailer + closes file

    std::uint64_t bytes_in()  const override { return bytes_in_.load(std::memory_order_relaxed); }
    std::uint64_t bytes_out() const override { return bytes_out_.load(std::memory_order_relaxed); }

private:
    bool write_deflated(const void* data, std::size_t n, int flush_mode);

    std::ofstream out_;
    // Opaque pointer to miniz's z_stream — kept as void* here so miniz.h
    // doesn't leak into translation units that only need the interface.
    void*                           zstream_ = nullptr;
    std::vector<std::uint8_t>       outbuf_;
    std::atomic<std::uint64_t>      bytes_in_{0};
    std::atomic<std::uint64_t>      bytes_out_{0};
    bool                            open_   = false;
    bool                            finished_ = false;
};

} // namespace ane::profiler

#endif // ANE_PROFILER_ZLIB_FILE_SINK_HPP
