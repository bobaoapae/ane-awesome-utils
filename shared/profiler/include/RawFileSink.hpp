// Raw (uncompressed) file sink. Mostly for tests and for the case where
// we want to inspect the stream byte-for-byte without decompression.
//
// Implementation uses std::ofstream in binary mode with a small in-process
// buffer; writes are forwarded as-is.

#ifndef ANE_PROFILER_RAW_FILE_SINK_HPP
#define ANE_PROFILER_RAW_FILE_SINK_HPP

#include "IByteSink.hpp"

#include <atomic>
#include <fstream>
#include <string>

namespace ane::profiler {

class RawFileSink : public IByteSink {
public:
    // Opens `path` for writing, truncating any existing file. Throws
    // std::runtime_error on open failure.
    explicit RawFileSink(const std::string& path);

    ~RawFileSink() override;

    bool  write(const void* data, std::size_t n) override;
    void  flush() override;
    void  close() override;

    std::uint64_t bytes_in() const override  { return bytes_.load(std::memory_order_relaxed); }
    std::uint64_t bytes_out() const override { return bytes_.load(std::memory_order_relaxed); }

private:
    std::ofstream                  out_;
    std::atomic<std::uint64_t>     bytes_{0};
    bool                           open_;
};

} // namespace ane::profiler

#endif // ANE_PROFILER_RAW_FILE_SINK_HPP
