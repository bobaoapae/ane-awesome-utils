// Unit tests for FileFormat.hpp + RawFileSink round-trip.

#include "FileFormat.hpp"
#include "RawFileSink.hpp"
#include "TestHarness.hpp"

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace ff = ane::profiler::fileformat;

namespace {
std::string tmp_path(const char* name) {
    auto p = std::filesystem::temp_directory_path() / name;
    return p.string();
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
} // namespace

TEST("header struct bytes have FLMC magic and version 1") {
    auto h = ff::make_header_bytes(42);
    EXPECT(std::memcmp(h.data(), "FLMC", 4) == 0);
    std::uint16_t ver = 0;
    std::memcpy(&ver, h.data() + 4, 2);
    EXPECT_EQ(ver, static_cast<std::uint16_t>(1));
    std::uint32_t hl = 0;
    std::memcpy(&hl, h.data() + 8, 4);
    EXPECT_EQ(hl, 42u);
}

TEST("footer struct round-trips via parse_footer_bytes") {
    auto f = ff::make_footer_bytes(1234, 567, 89, 1, 1735000000ull, 0xDEADBEEFu);
    ff::Footer parsed{};
    EXPECT(ff::parse_footer_bytes(f.data(), &parsed));
    EXPECT_EQ(parsed.total_bytes_raw,        static_cast<std::uint64_t>(1234));
    EXPECT_EQ(parsed.total_bytes_compressed, static_cast<std::uint64_t>(567));
    EXPECT_EQ(parsed.record_count,           static_cast<std::uint64_t>(89));
    EXPECT_EQ(parsed.dropped_count,          static_cast<std::uint64_t>(1));
    EXPECT_EQ(parsed.ended_utc,              static_cast<std::uint64_t>(1735000000ull));
    EXPECT_EQ(parsed.crc32_stream,           0xDEADBEEFu);
    EXPECT(std::memcmp(parsed.magic,     "FMFT", 4) == 0);
    EXPECT(std::memcmp(parsed.end_magic, "END!", 4) == 0);
}

TEST("parse_footer rejects bad magic") {
    std::uint8_t bad[64] = {0};
    ff::Footer out{};
    EXPECT(!ff::parse_footer_bytes(bad, &out));
}

TEST("crc32 of canonical test vector '123456789' = 0xCBF43926") {
    // Standard CRC-32/ISO-HDLC reference vector.
    EXPECT_EQ(ff::crc32("123456789", 9), 0xCBF43926u);
}

TEST("crc32 of empty buffer is 0") {
    EXPECT_EQ(ff::crc32("", 0), 0u);
}

TEST("crc32 incremental matches one-shot") {
    const std::string s = "The quick brown fox jumps over the lazy dog";
    auto full = ff::crc32(s.data(), s.size());
    auto a = ff::crc32_update(0, s.data(), 10);
    auto b = ff::crc32_update(a, s.data() + 10, s.size() - 10);
    EXPECT_EQ(full, b);
}

TEST("raw file sink writes what it is given") {
    std::string path = tmp_path("ane_flmc_test_raw.bin");
    {
        ane::profiler::RawFileSink sink(path);
        std::string hello = "hello world";
        EXPECT(sink.write(hello.data(), hello.size()));
        EXPECT_EQ(sink.bytes_in(), static_cast<std::uint64_t>(hello.size()));
        EXPECT_EQ(sink.bytes_out(), static_cast<std::uint64_t>(hello.size()));
        sink.flush();
    }
    auto v = read_all(path);
    EXPECT_EQ(v.size(), static_cast<std::size_t>(11));
    EXPECT(std::memcmp(v.data(), "hello world", 11) == 0);
    std::filesystem::remove(path);
}

TEST("end-to-end: header + stream + footer in a .flmc file") {
    std::string path = tmp_path("ane_flmc_test_e2e.flmc");

    // Fabricate a tiny Scout-like stream.
    std::string header_json = R"({"platform":"windows-x64","air_version":"51.1.3.10",)"
                              R"("compression":"raw","wire_protocol":"scout-amf3"})";
    std::vector<std::uint8_t> stream(1024);
    for (std::size_t i = 0; i < stream.size(); ++i)
        stream[i] = static_cast<std::uint8_t>(i & 0xFF);

    std::uint32_t crc = ff::crc32(stream.data(), stream.size());

    {
        ane::profiler::RawFileSink sink(path);
        auto h = ff::make_header_bytes(static_cast<std::uint32_t>(header_json.size()));
        EXPECT(sink.write(h.data(), h.size()));
        EXPECT(sink.write(header_json.data(), header_json.size()));
        EXPECT(sink.write(stream.data(), stream.size()));
        auto f = ff::make_footer_bytes(stream.size(), stream.size(),
                                        1, 0, 1735000000ull, crc);
        EXPECT(sink.write(f.data(), f.size()));
    }

    auto bytes = read_all(path);
    const std::size_t expected_size = sizeof(ff::Header) + header_json.size()
                                      + stream.size() + sizeof(ff::Footer);
    EXPECT_EQ(bytes.size(), expected_size);

    // Parse header.
    ff::Header hdr{};
    EXPECT(ff::parse_header_bytes(bytes.data(), &hdr));
    EXPECT_EQ(hdr.version, static_cast<std::uint16_t>(1));
    EXPECT_EQ(hdr.header_len, static_cast<std::uint32_t>(header_json.size()));

    // Verify header JSON round-trip.
    std::string got_json(reinterpret_cast<const char*>(bytes.data() + sizeof(ff::Header)),
                         hdr.header_len);
    EXPECT(got_json == header_json);

    // Parse footer.
    ff::Footer ft{};
    EXPECT(ff::parse_footer_bytes(bytes.data() + bytes.size() - sizeof(ff::Footer), &ft));
    EXPECT_EQ(ft.total_bytes_raw,        stream.size());
    EXPECT_EQ(ft.total_bytes_compressed, stream.size());
    EXPECT_EQ(ft.record_count,           static_cast<std::uint64_t>(1));
    EXPECT_EQ(ft.crc32_stream,           crc);

    // Stream bytes recoverable between header+JSON and footer.
    const std::size_t stream_off = sizeof(ff::Header) + hdr.header_len;
    const std::size_t stream_len = bytes.size() - stream_off - sizeof(ff::Footer);
    EXPECT_EQ(stream_len, stream.size());
    EXPECT(std::memcmp(bytes.data() + stream_off, stream.data(), stream.size()) == 0);

    std::filesystem::remove(path);
}

int main() { return ane::profiler::test::run_all(); }
