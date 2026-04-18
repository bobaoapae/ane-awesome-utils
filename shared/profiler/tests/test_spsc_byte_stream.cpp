// Unit tests for SpscByteStream.

#include "SpscByteStream.hpp"
#include "TestHarness.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

using ane::profiler::SpscByteStream;

TEST("push and pop a single small record") {
    SpscByteStream s(1024);
    EXPECT(s.empty());

    const char msg[] = "hello";
    EXPECT(s.try_push(msg, sizeof(msg)));
    EXPECT(!s.empty());
    EXPECT_EQ(s.used_bytes(), static_cast<std::size_t>(sizeof(std::uint32_t) + sizeof(msg)));

    char out[32] = {0};
    std::size_t got = s.try_pop(out, sizeof(out));
    EXPECT_EQ(got, static_cast<std::size_t>(sizeof(msg)));
    EXPECT(std::memcmp(out, msg, sizeof(msg)) == 0);
    EXPECT(s.empty());
}

TEST("pop on empty returns 0") {
    SpscByteStream s(64);
    char out[8];
    EXPECT_EQ(s.try_pop(out, sizeof(out)), 0u);
}

TEST("push returns false when full") {
    // cap=64, header=4 → each 12-byte record consumes 16 bytes.
    SpscByteStream s(64);
    std::uint8_t payload[12] = {0};
    std::size_t pushed = 0;
    while (s.try_push(payload, sizeof(payload))) ++pushed;
    EXPECT_EQ(pushed, static_cast<std::size_t>(64 / 16));
}

TEST("pop with too-small output buffer returns kTooLarge and does not consume") {
    SpscByteStream s(256);
    std::uint8_t payload[100] = {0};
    for (std::size_t i = 0; i < 100; ++i) payload[i] = static_cast<std::uint8_t>(i);
    EXPECT(s.try_push(payload, sizeof(payload)));

    std::uint8_t out_small[10];
    EXPECT_EQ(s.try_pop(out_small, sizeof(out_small)), SpscByteStream::kTooLarge);

    // Still consumable with a larger buffer.
    std::uint8_t out_big[128];
    EXPECT_EQ(s.try_pop(out_big, sizeof(out_big)), static_cast<std::size_t>(100));
    for (std::size_t i = 0; i < 100; ++i) EXPECT_EQ(out_big[i], static_cast<std::uint8_t>(i));
}

TEST("wraparound: push then pop beyond capacity boundary") {
    // cap=64, 4B header + 12B payload = 16B per record. Capacity holds 4.
    SpscByteStream s(64);

    std::uint8_t p1[12], p2[12], p3[12], p4[12];
    for (std::size_t i = 0; i < 12; ++i) {
        p1[i] = static_cast<std::uint8_t>(0xA0 + i);
        p2[i] = static_cast<std::uint8_t>(0xB0 + i);
        p3[i] = static_cast<std::uint8_t>(0xC0 + i);
        p4[i] = static_cast<std::uint8_t>(0xD0 + i);
    }
    EXPECT(s.try_push(p1, sizeof(p1)));
    EXPECT(s.try_push(p2, sizeof(p2)));
    EXPECT(s.try_push(p3, sizeof(p3)));
    EXPECT(s.try_push(p4, sizeof(p4)));       // fills exactly to capacity
    std::uint8_t p5[12] = {0};
    EXPECT(!s.try_push(p5, sizeof(p5)));      // one more would overflow

    // Consume first, then push one more that writes starting at offset 0
    // (physical wraparound in the arena).
    std::uint8_t out[16];
    EXPECT_EQ(s.try_pop(out, sizeof(out)), 12u);
    for (std::size_t i = 0; i < 12; ++i) EXPECT_EQ(out[i], static_cast<std::uint8_t>(0xA0 + i));

    std::uint8_t p6[12];
    for (std::size_t i = 0; i < 12; ++i) p6[i] = static_cast<std::uint8_t>(0xE0 + i);
    EXPECT(s.try_push(p6, sizeof(p6)));  // wraps

    EXPECT_EQ(s.try_pop(out, sizeof(out)), 12u);
    for (std::size_t i = 0; i < 12; ++i) EXPECT_EQ(out[i], static_cast<std::uint8_t>(0xB0 + i));
    EXPECT_EQ(s.try_pop(out, sizeof(out)), 12u);
    for (std::size_t i = 0; i < 12; ++i) EXPECT_EQ(out[i], static_cast<std::uint8_t>(0xC0 + i));
    EXPECT_EQ(s.try_pop(out, sizeof(out)), 12u);
    for (std::size_t i = 0; i < 12; ++i) EXPECT_EQ(out[i], static_cast<std::uint8_t>(0xD0 + i));
    EXPECT_EQ(s.try_pop(out, sizeof(out)), 12u);
    for (std::size_t i = 0; i < 12; ++i) EXPECT_EQ(out[i], static_cast<std::uint8_t>(0xE0 + i));
    EXPECT(s.empty());
}

TEST("large record that crosses arena midpoint is read correctly") {
    SpscByteStream s(128);
    // Push 64 + 4 bytes, pop it, then push 80 + 4 (wraps).
    std::vector<std::uint8_t> a(64, 0xAA);
    EXPECT(s.try_push(a.data(), static_cast<std::uint32_t>(a.size())));
    std::vector<std::uint8_t> out(128);
    EXPECT_EQ(s.try_pop(out.data(), out.size()), a.size());

    std::vector<std::uint8_t> b(80);
    for (std::size_t i = 0; i < b.size(); ++i) b[i] = static_cast<std::uint8_t>(i);
    EXPECT(s.try_push(b.data(), static_cast<std::uint32_t>(b.size())));
    EXPECT_EQ(s.try_pop(out.data(), out.size()), b.size());
    for (std::size_t i = 0; i < b.size(); ++i) EXPECT_EQ(out[i], static_cast<std::uint8_t>(i));
}

TEST("mt stress: single producer single consumer high volume") {
    constexpr std::size_t kRuns = 20000;
    SpscByteStream s(1 << 14);  // 16 KB
    std::atomic<bool> stop{false};

    std::thread producer([&]() {
        std::mt19937 rng(0xC0FFEE);
        for (std::size_t i = 0; i < kRuns; ++i) {
            std::uint32_t len = 1 + (rng() % 256);
            std::vector<std::uint8_t> buf(len);
            for (auto& b : buf) b = static_cast<std::uint8_t>(rng());
            // Mix in the sequence number so the consumer can verify order.
            if (len >= 4) std::memcpy(buf.data(), &i, 4);
            while (!s.try_push(buf.data(), len)) {
                std::this_thread::yield();
            }
        }
        stop.store(true);
    });

    std::size_t expected = 0;
    std::vector<std::uint8_t> out(4096);
    std::thread consumer([&]() {
        while (!(stop.load() && s.empty())) {
            std::size_t got = s.try_pop(out.data(), out.size());
            if (got == 0) { std::this_thread::yield(); continue; }
            if (got >= 4) {
                std::size_t seq = 0;
                std::memcpy(&seq, out.data(), 4);
                EXPECT_EQ(seq & 0xFFFF, expected & 0xFFFF);
            }
            ++expected;
        }
    });

    producer.join();
    consumer.join();
    EXPECT_EQ(expected, kRuns);
}

int main() { return ane::profiler::test::run_all(); }
