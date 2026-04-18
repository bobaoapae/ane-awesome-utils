// .flmc container format for on-device profiling captures.
//
// Layout:
//   offset 0       "FLMC" magic (4B)
//   offset 4       version u16 LE (= 1)
//   offset 6       reserved u16 (= 0)
//   offset 8       header_len u32 LE
//   offset 12      header JSON UTF-8 (header_len bytes). Freeform but
//                  at minimum expected to contain:
//                      { "platform": "windows-x64", "air_version": "...",
//                        "started_utc": <unix_s>, "compression": "raw" | "zlib",
//                        "wire_protocol": "scout-amf3",
//                        "wire_stream_offset": <byte offset of compressed data> }
//   offset 12+H    compressed / raw stream of AMF3 messages. Decompressed
//                  result is byte-identical to the TCP stream Scout would
//                  have received in a live session.
//   last 64 B      fixed binary footer (see Footer struct below)
//
// Reader is expected to seek-end to read the footer, then parse header JSON
// to know how to decompress the stream.
//
// All integer fields are little-endian on disk.

#ifndef ANE_PROFILER_FILE_FORMAT_HPP
#define ANE_PROFILER_FILE_FORMAT_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ane::profiler::fileformat {

inline constexpr char kMagic[4]        = {'F', 'L', 'M', 'C'};
inline constexpr char kFooterMagic[4]  = {'F', 'M', 'F', 'T'};
inline constexpr char kFooterEndMagic[4] = {'E', 'N', 'D', '!'};

inline constexpr std::uint16_t kFormatVersion = 1;

// The first 12 bytes of the file.
#pragma pack(push, 1)
struct Header {
    char            magic[4];      // "FLMC"
    std::uint16_t   version;       // = 1
    std::uint16_t   reserved;      // = 0
    std::uint32_t   header_len;    // bytes of JSON that immediately follow
};
#pragma pack(pop)

static_assert(sizeof(Header) == 12, "Header must be 12 bytes");

// The last 64 bytes of the file. Fixed layout so readers can seek the end
// and grab the totals without streaming the body.
#pragma pack(push, 1)
struct Footer {
    char            magic[4];            // "FMFT"               (+0)
    std::uint16_t   version;             // = 1                  (+4)
    std::uint16_t   reserved;            // = 0                  (+6)
    std::uint64_t   total_bytes_raw;     // uncompressed         (+8)
    std::uint64_t   total_bytes_compressed;                   // (+16)
    std::uint64_t   record_count;        //                     (+24)
    std::uint64_t   dropped_count;       // bytes dropped under backpressure (+32)
    std::uint64_t   ended_utc;           // unix seconds        (+40)
    std::uint32_t   crc32_stream;        // of the compressed stream (+48)
    std::uint32_t   reserved2;           // = 0                 (+52)
    char            end_magic[4];        // "END!"              (+56)
    char            pad[4];              // = 0                 (+60)
};
#pragma pack(pop)

static_assert(sizeof(Footer) == 64, "Footer must be exactly 64 bytes");

// Helper builders ----------------------------------------------------------

// Build the binary Header prefix (12 bytes). Caller writes this followed
// by `header_json` of length `header_json.size()`.
inline std::array<std::uint8_t, sizeof(Header)> make_header_bytes(std::uint32_t header_json_len) {
    Header h{};
    std::memcpy(h.magic, kMagic, 4);
    h.version    = kFormatVersion;
    h.reserved   = 0;
    h.header_len = header_json_len;
    std::array<std::uint8_t, sizeof(Header)> out;
    std::memcpy(out.data(), &h, sizeof(h));
    return out;
}

// Build the binary Footer (64 bytes).
inline std::array<std::uint8_t, sizeof(Footer)>
make_footer_bytes(std::uint64_t total_bytes_raw,
                  std::uint64_t total_bytes_compressed,
                  std::uint64_t record_count,
                  std::uint64_t dropped_count,
                  std::uint64_t ended_utc,
                  std::uint32_t crc32_stream) {
    Footer f{};
    std::memcpy(f.magic, kFooterMagic, 4);
    f.version                = kFormatVersion;
    f.reserved               = 0;
    f.total_bytes_raw        = total_bytes_raw;
    f.total_bytes_compressed = total_bytes_compressed;
    f.record_count           = record_count;
    f.dropped_count          = dropped_count;
    f.ended_utc              = ended_utc;
    f.crc32_stream           = crc32_stream;
    f.reserved2              = 0;
    std::memcpy(f.end_magic, kFooterEndMagic, 4);
    std::memset(f.pad, 0, 4);
    std::array<std::uint8_t, sizeof(Footer)> out;
    std::memcpy(out.data(), &f, sizeof(f));
    return out;
}

// Parse the binary Footer from a 64-byte buffer. Returns false on bad magic.
inline bool parse_footer_bytes(const void* src, Footer* out) noexcept {
    if (src == nullptr || out == nullptr) return false;
    std::memcpy(out, src, sizeof(Footer));
    if (std::memcmp(out->magic, kFooterMagic, 4) != 0) return false;
    if (std::memcmp(out->end_magic, kFooterEndMagic, 4) != 0) return false;
    return true;
}

// Parse the Header prefix (first 12 bytes). Returns false on bad magic.
inline bool parse_header_bytes(const void* src, Header* out) noexcept {
    if (src == nullptr || out == nullptr) return false;
    std::memcpy(out, src, sizeof(Header));
    return std::memcmp(out->magic, kMagic, 4) == 0;
}

// CRC32/IEEE — identical to what zlib uses. Implemented inline so we can
// compute it over the compressed stream without depending on zlib.
// Bitwise slow path; the writer thread is not bottlenecked by CRC.
inline std::uint32_t crc32_update(std::uint32_t crc, const void* buf, std::size_t n) noexcept {
    static constexpr std::uint32_t kPoly = 0xEDB88320u;
    const std::uint8_t* p = static_cast<const std::uint8_t*>(buf);
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (kPoly & -static_cast<std::int32_t>(crc & 1));
        }
    }
    return ~crc;
}

inline std::uint32_t crc32(const void* buf, std::size_t n) noexcept {
    return crc32_update(0, buf, n);
}

} // namespace ane::profiler::fileformat

#endif // ANE_PROFILER_FILE_FORMAT_HPP
