#include "AneprofFormat.hpp"
#include "DeepProfilerController.hpp"
#include "TestHarness.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace ap = ane::profiler::aneprof;
using ane::profiler::DeepProfilerController;

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

std::uint32_t count_events(const std::vector<std::uint8_t>& bytes) {
    ap::FileHeader header{};
    EXPECT(ap::parse_header_bytes(bytes.data(), &header));
    std::size_t off = sizeof(ap::FileHeader) + header.header_json_len;
    const std::size_t end = bytes.size() - sizeof(ap::FileFooter);
    std::uint32_t count = 0;
    while (off < end) {
        ap::EventHeader eh{};
        EXPECT(ap::parse_event_header_bytes(bytes.data() + off, &eh));
        off += sizeof(ap::EventHeader) + eh.payload_size;
        ++count;
    }
    EXPECT_EQ(off, end);
    return count;
}
} // namespace

TEST("aneprof header and footer helpers round-trip") {
    auto hb = ap::make_header_bytes(17, 123);
    ap::FileHeader h{};
    EXPECT(ap::parse_header_bytes(hb.data(), &h));
    EXPECT_EQ(h.header_json_len, 17u);
    EXPECT_EQ(h.started_utc, static_cast<std::uint64_t>(123));

    auto fb = ap::make_footer_bytes(5, 1, 99, 456, 2, 64);
    ap::FileFooter f{};
    EXPECT(ap::parse_footer_bytes(fb.data(), &f));
    EXPECT_EQ(f.event_count, static_cast<std::uint64_t>(5));
    EXPECT_EQ(f.dropped_count, static_cast<std::uint64_t>(1));
    EXPECT_EQ(f.payload_bytes, static_cast<std::uint64_t>(99));
    EXPECT_EQ(f.live_allocations, static_cast<std::uint64_t>(2));
    EXPECT_EQ(f.live_bytes, static_cast<std::uint64_t>(64));
}

TEST("controller writes marker, timing, memory and snapshots") {
    const std::string path = tmp_path("ane_deep_profiler_test.aneprof");
    DeepProfilerController cc;
    DeepProfilerController::Config cfg;
    cfg.output_path = path;
    cfg.header_json = R"({"format":"aneprof","test":"controller"})";
    cfg.timing_enabled = true;
    cfg.memory_enabled = true;
    cfg.snapshots_enabled = true;
    EXPECT(cc.start(cfg));

    EXPECT(cc.marker("battle.start", R"({"turn":1})"));
    EXPECT(cc.method_enter(42));
    EXPECT(cc.record_alloc(reinterpret_cast<void*>(0x1000), 64));
    EXPECT(cc.record_free_if_tracked(reinterpret_cast<void*>(0xfeed)));
    EXPECT(cc.record_realloc(reinterpret_cast<void*>(0x1000), reinterpret_cast<void*>(0x2000), 96));
    EXPECT(cc.record_realloc_if_tracked(reinterpret_cast<void*>(0xbeef),
                                        reinterpret_cast<void*>(0x3000),
                                        128));
    EXPECT(cc.record_free(reinterpret_cast<void*>(0x2000)));
    EXPECT(cc.method_exit(42));
    EXPECT(cc.snapshot("manual"));

    auto status = cc.status();
    EXPECT_EQ(status.live_allocations, static_cast<std::uint64_t>(0));
    EXPECT_EQ(status.total_allocations, static_cast<std::uint64_t>(1));
    EXPECT_EQ(status.total_frees, static_cast<std::uint64_t>(1));
    EXPECT_EQ(status.total_reallocations, static_cast<std::uint64_t>(1));
    EXPECT(cc.stop());

    const auto bytes = read_all(path);
    EXPECT(bytes.size() > sizeof(ap::FileHeader) + sizeof(ap::FileFooter));
    ap::FileFooter footer{};
    EXPECT(ap::parse_footer_bytes(bytes.data() + bytes.size() - sizeof(ap::FileFooter), &footer));
    EXPECT_EQ(footer.live_allocations, static_cast<std::uint64_t>(0));
    EXPECT(footer.event_count >= 10);
    EXPECT_EQ(count_events(bytes), static_cast<std::uint32_t>(footer.event_count));
    std::filesystem::remove(path);
}

TEST("controller writes AS3 reference payloads") {
    const std::string path = tmp_path("ane_deep_profiler_as3_ref.aneprof");
    DeepProfilerController cc;
    DeepProfilerController::Config cfg;
    cfg.output_path = path;
    cfg.header_json = R"({"format":"aneprof","test":"as3-ref"})";
    cfg.memory_enabled = true;
    EXPECT(cc.start(cfg));
    EXPECT(cc.record_as3_reference(0x11112222u, 0x33334444u));
    EXPECT(cc.stop());

    const auto bytes = read_all(path);
    ap::FileHeader header{};
    EXPECT(ap::parse_header_bytes(bytes.data(), &header));
    std::size_t off = sizeof(ap::FileHeader) + header.header_json_len;
    const std::size_t end = bytes.size() - sizeof(ap::FileFooter);
    bool found = false;
    while (off < end) {
        ap::EventHeader eh{};
        EXPECT(ap::parse_event_header_bytes(bytes.data() + off, &eh));
        off += sizeof(ap::EventHeader);
        if (eh.type == static_cast<std::uint16_t>(ap::EventType::As3Reference)) {
            ap::As3ReferenceEvent payload{};
            EXPECT_EQ(eh.payload_size, static_cast<std::uint32_t>(sizeof(payload)));
            std::memcpy(&payload, bytes.data() + off, sizeof(payload));
            EXPECT_EQ(payload.owner_id, static_cast<std::uint64_t>(0x11112222u));
            EXPECT_EQ(payload.dependent_id, static_cast<std::uint64_t>(0x33334444u));
            found = true;
        }
        off += eh.payload_size;
    }
    EXPECT(found);
    std::filesystem::remove(path);
}

TEST("periodic snapshots add events while recording") {
    const std::string path = tmp_path("ane_deep_profiler_periodic.aneprof");
    DeepProfilerController cc;
    DeepProfilerController::Config cfg;
    cfg.output_path = path;
    cfg.snapshots_enabled = true;
    cfg.snapshot_interval_ms = 10;
    EXPECT(cc.start(cfg));
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    EXPECT(cc.stop());

    const auto bytes = read_all(path);
    ap::FileFooter footer{};
    EXPECT(ap::parse_footer_bytes(bytes.data() + bytes.size() - sizeof(ap::FileFooter), &footer));
    EXPECT(footer.event_count >= 5);
    std::filesystem::remove(path);
}

int main() { return ane::profiler::test::run_all(); }
