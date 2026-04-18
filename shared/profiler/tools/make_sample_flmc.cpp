// Generates a synthetic .flmc file for validation harnesses.
//
//   make_sample_flmc <out_path> [payload_bytes=4096]
//
// Pushes `payload_bytes` of pseudo-random bytes through a CaptureController,
// stops, and leaves the file on disk. The flmc_validate.py CLI is expected
// to accept the result without errors.

#include "CaptureController.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <out_path> [payload_bytes=4096]\n", argv[0]);
        return 1;
    }
    const std::string out = argv[1];
    std::size_t payload_bytes = 4096;
    if (argc >= 3) {
        payload_bytes = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
    }

    ane::profiler::CaptureController cc;
    ane::profiler::CaptureController::Config cfg;
    cfg.output_path = out;
    cfg.header_json = R"({"platform":"test","air_version":"51.1.3.10",)"
                      R"("compression":"zlib","wire_protocol":"scout-amf3"})";
    cfg.compression_level = 6;

    if (!cc.start(cfg)) {
        std::fprintf(stderr, "start failed\n");
        return 2;
    }

    // Push payload in ~1 KB records mixing patterns zlib can and cannot squeeze.
    std::vector<std::uint8_t> buf(1024);
    std::size_t remaining = payload_bytes;
    std::size_t seq = 0;
    while (remaining > 0) {
        std::size_t n = (remaining < buf.size()) ? remaining : buf.size();
        for (std::size_t i = 0; i < n; ++i) {
            buf[i] = static_cast<std::uint8_t>((seq * 0x9E3779B1u + i) & 0xFFu);
        }
        while (!cc.push_bytes(buf.data(), static_cast<std::uint32_t>(n))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        remaining -= n;
        ++seq;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cc.stop();

    auto s = cc.status();
    std::printf("wrote %s  records=%llu  bytes_in=%llu  bytes_out=%llu\n",
                out.c_str(),
                static_cast<unsigned long long>(s.record_count),
                static_cast<unsigned long long>(s.bytes_in),
                static_cast<unsigned long long>(s.bytes_out));
    return 0;
}
