// End-to-end test: CaptureController produces a valid .flmc file whose
// compressed payload round-trips back to the exact bytes pushed through
// push_bytes(), and whose footer records the correct totals.

#include "CaptureController.hpp"
#include "FileFormat.hpp"
#include "TestHarness.hpp"

#include "miniz.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace ff = ane::profiler::fileformat;
using ane::profiler::CaptureController;

namespace {
std::string tmp_path(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

std::vector<std::uint8_t> read_all(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    in.seekg(0, std::ios::end);
    auto sz = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    std::vector<std::uint8_t> v(sz);
    in.read(reinterpret_cast<char*>(v.data()), sz);
    return v;
}

std::vector<std::uint8_t> inflate_zlib(const std::uint8_t* src, std::size_t srcN,
                                       std::size_t hint) {
    std::vector<std::uint8_t> out(hint + 4096);
    mz_stream z{};
    EXPECT(mz_inflateInit(&z) == MZ_OK);
    z.next_in   = src;
    z.avail_in  = static_cast<unsigned int>(srcN);
    std::size_t written = 0;
    while (true) {
        if (written + 4096 > out.size()) out.resize(out.size() * 2);
        z.next_out  = out.data() + written;
        z.avail_out = static_cast<unsigned int>(out.size() - written);
        int rc = mz_inflate(&z, MZ_SYNC_FLUSH);
        written = out.size() - z.avail_out;
        if (rc == MZ_STREAM_END) break;
        if (rc != MZ_OK) { mz_inflateEnd(&z); EXPECT(false); }
        if (z.avail_in == 0 && z.avail_out != 0) break;
    }
    mz_inflateEnd(&z);
    out.resize(written);
    return out;
}
} // namespace

TEST("start/stop idle -> idle without writes") {
    std::string path = tmp_path("ane_cap_empty.flmc");
    CaptureController cc;
    CaptureController::Config cfg;
    cfg.output_path = path;
    cfg.header_json = R"({"test":"empty"})";
    cfg.compression_level = 6;
    EXPECT(cc.start(cfg));
    cc.stop();

    auto bytes = read_all(path);
    ff::Header h{};
    EXPECT(ff::parse_header_bytes(bytes.data(), &h));
    ff::Footer f{};
    EXPECT(ff::parse_footer_bytes(bytes.data() + bytes.size() - sizeof(ff::Footer), &f));
    EXPECT_EQ(f.record_count, static_cast<std::uint64_t>(0));
    EXPECT_EQ(f.total_bytes_raw, static_cast<std::uint64_t>(0));

    std::filesystem::remove(path);
}

TEST("push bytes, stop, verify round-trip match") {
    std::string path = tmp_path("ane_cap_roundtrip.flmc");

    CaptureController cc;
    CaptureController::Config cfg;
    cfg.output_path       = path;
    cfg.header_json       = R"({"test":"roundtrip","compression":"zlib"})";
    cfg.ring_capacity     = 256 * 1024;  // 256 KB
    cfg.compression_level = 6;
    EXPECT(cc.start(cfg));

    // Feed 200 records of varying size, concatenated input.
    std::mt19937 rng(0xC0DE);
    std::vector<std::uint8_t> expected;
    expected.reserve(200 * 512);
    for (int i = 0; i < 200; ++i) {
        std::uint32_t len = 16 + (rng() % 1024);
        std::vector<std::uint8_t> buf(len);
        for (std::size_t k = 0; k < len; ++k) buf[k] = static_cast<std::uint8_t>(rng());
        while (!cc.push_bytes(buf.data(), len)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expected.insert(expected.end(), buf.begin(), buf.end());
    }

    cc.stop();

    // Parse the .flmc container.
    auto bytes = read_all(path);
    ff::Header h{};
    EXPECT(ff::parse_header_bytes(bytes.data(), &h));
    const std::size_t stream_off = sizeof(ff::Header) + h.header_len;
    EXPECT(bytes.size() > stream_off + sizeof(ff::Footer));
    const std::size_t stream_len = bytes.size() - stream_off - sizeof(ff::Footer);

    auto decoded = inflate_zlib(bytes.data() + stream_off, stream_len, expected.size());
    EXPECT_EQ(decoded.size(), expected.size());
    EXPECT(std::memcmp(decoded.data(), expected.data(), expected.size()) == 0);

    ff::Footer f{};
    EXPECT(ff::parse_footer_bytes(bytes.data() + bytes.size() - sizeof(ff::Footer), &f));
    EXPECT_EQ(f.record_count,    static_cast<std::uint64_t>(200));
    EXPECT_EQ(f.total_bytes_raw, static_cast<std::uint64_t>(expected.size()));
    EXPECT(f.total_bytes_compressed > 0);

    std::filesystem::remove(path);
}

TEST("backpressure: push returns false when ring is full, drop counters advance") {
    std::string path = tmp_path("ane_cap_backpressure.flmc");
    CaptureController cc;
    CaptureController::Config cfg;
    cfg.output_path       = path;
    cfg.header_json       = R"({"test":"backpressure"})";
    cfg.ring_capacity     = 128;    // tiny on purpose — forces drops
    cfg.compression_level = 9;
    // Make the writer sleep a lot so it can't drain behind us.
    cfg.writer_idle_sleep = std::chrono::milliseconds(50);
    EXPECT(cc.start(cfg));

    std::uint8_t big[120] = {0};
    // First push fits (120 + 4 = 124 <= 128). Second must fail (no room).
    EXPECT(cc.push_bytes(big, sizeof(big)));
    for (int i = 0; i < 100; ++i) {
        // Push fast enough to outrun the writer. Some should fail.
        cc.push_bytes(big, sizeof(big));
    }
    auto s1 = cc.status();
    EXPECT(s1.drop_count > 0);
    EXPECT(s1.drop_bytes > 0);

    cc.stop();
    std::filesystem::remove(path);
}

TEST("push after stop is rejected") {
    CaptureController cc;
    std::string path = tmp_path("ane_cap_rejected.flmc");
    CaptureController::Config cfg;
    cfg.output_path = path;
    cfg.header_json = R"({"test":"rejected"})";
    EXPECT(cc.start(cfg));
    cc.stop();

    std::uint8_t buf[4] = {1,2,3,4};
    EXPECT(!cc.push_bytes(buf, sizeof(buf)));

    std::filesystem::remove(path);
}

int main() { return ane::profiler::test::run_all(); }
