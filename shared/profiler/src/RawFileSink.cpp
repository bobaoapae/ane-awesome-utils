#include "RawFileSink.hpp"

#include <ios>
#include <stdexcept>

namespace ane::profiler {

RawFileSink::RawFileSink(const std::string& path)
    : out_(path, std::ios::binary | std::ios::trunc), open_(false) {
    if (!out_.is_open()) {
        throw std::runtime_error("RawFileSink: failed to open '" + path + "'");
    }
    open_ = true;
}

RawFileSink::~RawFileSink() {
    try { close(); } catch (...) {}
}

bool RawFileSink::write(const void* data, std::size_t n) {
    if (!open_) return false;
    if (n == 0) return true;
    out_.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    if (!out_) { open_ = false; return false; }
    bytes_.fetch_add(n, std::memory_order_relaxed);
    return true;
}

void RawFileSink::flush() {
    if (open_) out_.flush();
}

void RawFileSink::close() {
    if (open_) {
        out_.flush();
        out_.close();
        open_ = false;
    }
}

} // namespace ane::profiler
