// ZlibFileSink round-trip test.
//
// We write a stream of bytes through the sink, then decompress the result
// using miniz directly and verify it matches the input. Also confirms the
// produced file starts with the zlib header (0x78, 0x01/0x5E/0x9C/0xDA).

#include "ZlibFileSink.hpp"
#include "TestHarness.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "miniz.h"

using ane::profiler::ZlibFileSink;

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

std::vector<std::uint8_t> inflate_zlib(const std::vector<std::uint8_t>& compressed,
                                       std::size_t expected) {
    std::vector<std::uint8_t> out(expected + 64);
    mz_stream z{};
    EXPECT(mz_inflateInit(&z) == MZ_OK);
    z.next_in  = compressed.data();
    z.avail_in = static_cast<unsigned int>(compressed.size());
    z.next_out = out.data();
    z.avail_out = static_cast<unsigned int>(out.size());
    int rc = mz_inflate(&z, MZ_FINISH);
    mz_inflateEnd(&z);
    EXPECT(rc == MZ_STREAM_END);
    out.resize(z.total_out);
    return out;
}
} // namespace

TEST("round-trip small buffer through zlib sink") {
    std::string path = tmp_path("ane_zlib_small.bin");
    std::string msg = "hello, zlib world";
    {
        ZlibFileSink sink(path);
        EXPECT(sink.write(msg.data(), msg.size()));
    }
    auto bytes = read_all(path);
    EXPECT(bytes.size() > 0);
    EXPECT(bytes[0] == 0x78);   // zlib magic
    auto decoded = inflate_zlib(bytes, msg.size());
    EXPECT(decoded.size() == msg.size());
    EXPECT(std::memcmp(decoded.data(), msg.data(), msg.size()) == 0);
    std::filesystem::remove(path);
}

TEST("round-trip 1MB random buffer") {
    std::string path = tmp_path("ane_zlib_big.bin");

    std::mt19937 rng(0xC0FFEE);
    std::vector<std::uint8_t> input(1 * 1024 * 1024);
    for (auto& b : input) b = static_cast<std::uint8_t>(rng());

    {
        ZlibFileSink sink(path, 1); // fast level — random data, little win
        EXPECT(sink.write(input.data(), input.size()));
    }

    auto compressed = read_all(path);
    EXPECT(compressed.size() > 0);
    // Random data shouldn't shrink much; allow any size <= 1.1x input.
    EXPECT(compressed.size() <= input.size() * 11 / 10);

    auto decoded = inflate_zlib(compressed, input.size());
    EXPECT(decoded.size() == input.size());
    EXPECT(std::memcmp(decoded.data(), input.data(), input.size()) == 0);
    std::filesystem::remove(path);
}

TEST("round-trip highly-compressible repetitive buffer") {
    std::string path = tmp_path("ane_zlib_repeat.bin");
    std::vector<std::uint8_t> input(256 * 1024, 'A');
    {
        ZlibFileSink sink(path, 9);
        EXPECT(sink.write(input.data(), input.size()));
    }
    auto compressed = read_all(path);
    EXPECT(compressed.size() > 0);
    EXPECT(compressed.size() < 1024); // 'AAAA…' better shrink a lot
    auto decoded = inflate_zlib(compressed, input.size());
    EXPECT(decoded.size() == input.size());
    for (auto b : decoded) EXPECT(b == 'A');
    std::filesystem::remove(path);
}

TEST("multi-chunk writes produce the same output as one big write") {
    std::mt19937 rng(0xBABE);
    std::vector<std::uint8_t> input(64 * 1024);
    for (auto& b : input) b = static_cast<std::uint8_t>(rng());

    std::string pBig = tmp_path("ane_zlib_big_single.bin");
    std::string pChk = tmp_path("ane_zlib_big_chunked.bin");
    {
        ZlibFileSink s(pBig);
        EXPECT(s.write(input.data(), input.size()));
    }
    {
        ZlibFileSink s(pChk);
        constexpr std::size_t kChunk = 4096;
        for (std::size_t i = 0; i < input.size(); i += kChunk) {
            std::size_t n = std::min(kChunk, input.size() - i);
            EXPECT(s.write(input.data() + i, n));
        }
    }
    // Both must round-trip to the same bytes, even if encodings differ.
    auto cBig = read_all(pBig);
    auto cChk = read_all(pChk);
    auto dBig = inflate_zlib(cBig, input.size());
    auto dChk = inflate_zlib(cChk, input.size());
    EXPECT(dBig.size() == input.size());
    EXPECT(dChk.size() == input.size());
    EXPECT(std::memcmp(dBig.data(), input.data(), input.size()) == 0);
    EXPECT(std::memcmp(dChk.data(), input.data(), input.size()) == 0);

    std::filesystem::remove(pBig);
    std::filesystem::remove(pChk);
}

int main() { return ane::profiler::test::run_all(); }
