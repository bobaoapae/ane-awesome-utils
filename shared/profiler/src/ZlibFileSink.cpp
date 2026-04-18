#include "ZlibFileSink.hpp"

#include "miniz.h"

#include <ios>
#include <stdexcept>

namespace ane::profiler {

namespace {
constexpr std::size_t kChunkBytes = 64 * 1024; // 64 KB compressed-out chunk
} // namespace

ZlibFileSink::ZlibFileSink(const std::string& path, int level)
    : out_(path, std::ios::binary | std::ios::trunc) {
    if (!out_.is_open()) {
        throw std::runtime_error("ZlibFileSink: failed to open '" + path + "'");
    }
    outbuf_.resize(kChunkBytes);

    auto* z = new mz_stream();
    *z = mz_stream{};
    int rc = mz_deflateInit(z, level);
    if (rc != MZ_OK) {
        delete z;
        throw std::runtime_error("ZlibFileSink: mz_deflateInit failed");
    }
    zstream_ = z;
    open_ = true;
}

ZlibFileSink::~ZlibFileSink() {
    try { close(); } catch (...) {}
    if (zstream_ != nullptr) {
        auto* z = static_cast<mz_stream*>(zstream_);
        mz_deflateEnd(z);
        delete z;
        zstream_ = nullptr;
    }
}

bool ZlibFileSink::write_deflated(const void* data, std::size_t n, int flush_mode) {
    if (!open_ || zstream_ == nullptr) return false;
    auto* z = static_cast<mz_stream*>(zstream_);

    z->next_in  = static_cast<const unsigned char*>(data);
    z->avail_in = static_cast<unsigned int>(n);

    for (;;) {
        z->next_out  = outbuf_.data();
        z->avail_out = static_cast<unsigned int>(outbuf_.size());

        int rc = mz_deflate(z, flush_mode);
        std::size_t produced = outbuf_.size() - z->avail_out;
        if (produced > 0) {
            out_.write(reinterpret_cast<const char*>(outbuf_.data()),
                       static_cast<std::streamsize>(produced));
            if (!out_) { open_ = false; return false; }
            bytes_out_.fetch_add(produced, std::memory_order_relaxed);
        }

        if (rc == MZ_STREAM_END) {
            finished_ = true;
            return true;
        }
        if (rc == MZ_BUF_ERROR && z->avail_in == 0 && flush_mode == MZ_NO_FLUSH) {
            // Nothing more to feed, deflator has nothing to emit. Done.
            return true;
        }
        if (rc != MZ_OK) {
            // Any other code is a failure. (MZ_OK keeps us going.)
            return false;
        }
        // If we still have input or the deflator had to break out because
        // the output buffer filled up, keep looping.
        if (z->avail_in == 0 && z->avail_out != 0) {
            return true;
        }
    }
}

bool ZlibFileSink::write(const void* data, std::size_t n) {
    if (n == 0) return true;
    if (!open_ || finished_) return false;
    bytes_in_.fetch_add(n, std::memory_order_relaxed);
    return write_deflated(data, n, MZ_NO_FLUSH);
}

void ZlibFileSink::flush() {
    if (!open_ || finished_) return;
    write_deflated(nullptr, 0, MZ_SYNC_FLUSH);
    out_.flush();
}

void ZlibFileSink::close() {
    if (!open_) return;
    if (!finished_) {
        write_deflated(nullptr, 0, MZ_FINISH);
    }
    out_.flush();
    out_.close();
    open_ = false;
}

} // namespace ane::profiler
