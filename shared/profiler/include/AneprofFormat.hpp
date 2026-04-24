// Native .aneprof container used by the deep profiler.
//
// The payload is an append-only binary event log. It intentionally does not
// embed Scout/AMF telemetry bytes; the ANE owns the schema and can evolve it
// independently from Adobe's telemetry transport.

#ifndef ANE_PROFILER_ANEPROF_FORMAT_HPP
#define ANE_PROFILER_ANEPROF_FORMAT_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ane::profiler::aneprof {

inline constexpr char kHeaderMagic[8] = {'A', 'N', 'E', 'P', 'R', 'O', 'F', 0};
inline constexpr char kFooterMagic[8] = {'A', 'N', 'E', 'P', 'E', 'N', 'D', 0};
inline constexpr std::uint16_t kFormatVersion = 1;

enum class EventType : std::uint16_t {
    Start         = 1,
    Stop          = 2,
    Marker        = 3,
    Snapshot      = 4,
    MethodEnter   = 5,
    MethodExit    = 6,
    Alloc         = 7,
    Free          = 8,
    Realloc       = 9,
    LiveAllocation = 10,
    MethodTable   = 11,
    As3Alloc      = 12,
    As3Free       = 13,
    As3Reference  = 14,
    As3ReferenceEx = 15,
    As3Root       = 16,
    As3Payload    = 17,
    Frame         = 18,
    GcCycle       = 19,
};

enum class As3ReferenceKind : std::uint16_t {
    Unknown       = 0,
    Slot          = 1,
    Array         = 2,
    Dictionary    = 3,
    DisplayChild  = 4,
    EventListener = 5,
    TimerCallback = 6,
    StaticField   = 7,
    NativePayload = 8,
};

enum class As3RootKind : std::uint16_t {
    Unknown         = 0,
    Stage           = 1,
    DisplayList     = 2,
    Static          = 3,
    Timer           = 4,
    EventDispatcher = 5,
    Loader          = 6,
    Native          = 7,
};

enum class As3PayloadKind : std::uint16_t {
    Unknown    = 0,
    BitmapData = 1,
    ByteArray  = 2,
    Texture    = 3,
    Vector     = 4,
    NativePeer = 5,
};

enum class GcCycleKind : std::uint16_t {
    Unknown         = 0,
    NativeRequested = 1,
    NativeObserved  = 2,
    Runtime         = 3,
};

enum EventFlags : std::uint16_t {
    EventFlagInferred = 1u << 0,
    EventFlagRequested = 1u << 1,
    EventFlagBeforeUnknown = 1u << 2,
    EventFlagAfterUnknown = 1u << 3,
};

#pragma pack(push, 1)
struct FileHeader {
    char          magic[8];          // "ANEPROF\0"
    std::uint16_t version;           // = 1
    std::uint16_t reserved;          // = 0
    std::uint32_t header_json_len;   // bytes following this header
    std::uint64_t started_utc;       // unix seconds
};

struct EventHeader {
    std::uint16_t type;              // EventType
    std::uint16_t flags;             // type-specific, currently 0
    std::uint32_t payload_size;      // bytes immediately following
    std::uint64_t timestamp_ns;      // QueryPerformanceCounter converted to ns
    std::uint32_t thread_id;         // OS thread id
    std::uint32_t reserved;          // = 0
};

struct FileFooter {
    char          magic[8];          // "ANEPEND\0"
    std::uint16_t version;           // = 1
    std::uint16_t reserved;          // = 0
    std::uint32_t footer_size;       // sizeof(FileFooter)
    std::uint64_t event_count;
    std::uint64_t dropped_count;
    std::uint64_t payload_bytes;
    std::uint64_t ended_utc;
    std::uint64_t live_allocations;
    std::uint64_t live_bytes;
    std::uint32_t crc32_events;      // event region CRC32, 0 when not computed
    std::uint32_t reserved2;
};

struct MethodEvent {
    std::uint32_t method_id;
    std::uint32_t depth;
};

struct AllocationEvent {
    std::uint64_t ptr;
    std::uint64_t size;
    std::uint64_t old_ptr;
    std::uint64_t old_size;
    std::uint32_t method_id;
    std::uint32_t stack_id;
};

struct SnapshotEvent {
    std::uint64_t live_allocations;
    std::uint64_t live_bytes;
    std::uint64_t total_allocations;
    std::uint64_t total_frees;
    std::uint64_t total_reallocations;
    std::uint64_t unknown_frees;
    std::uint32_t label_len;
    std::uint32_t reserved;
};

struct LiveAllocationEvent {
    std::uint64_t ptr;
    std::uint64_t size;
    std::uint64_t timestamp_ns;
    std::uint32_t thread_id;
    std::uint32_t method_id;
};

struct As3ObjectEvent {
    std::uint64_t sample_id;
    std::uint64_t size;
    std::uint32_t type_name_len;
    std::uint32_t stack_len;
};

struct As3ReferenceEvent {
    std::uint64_t owner_id;
    std::uint64_t dependent_id;
};

struct As3ReferenceExEvent {
    std::uint64_t owner_id;
    std::uint64_t dependent_id;
    std::uint16_t kind;
    std::uint16_t reserved;
    std::uint32_t label_len;
};

struct As3RootEvent {
    std::uint64_t object_id;
    std::uint16_t kind;
    std::uint16_t reserved;
    std::uint32_t label_len;
};

struct As3PayloadEvent {
    std::uint64_t owner_id;
    std::uint64_t payload_id;
    std::uint64_t logical_bytes;
    std::uint64_t native_bytes;
    std::uint16_t kind;
    std::uint16_t reserved;
    std::uint32_t label_len;
};

struct FrameEvent {
    std::uint64_t frame_index;
    std::uint64_t duration_ns;
    std::uint64_t allocation_bytes;
    std::uint32_t allocation_count;
    std::uint32_t label_len;
};

struct GcCycleEvent {
    std::uint64_t gc_id;
    std::uint64_t before_live_bytes;
    std::uint64_t after_live_bytes;
    std::uint64_t before_live_count;
    std::uint64_t after_live_count;
    std::uint16_t kind;
    std::uint16_t reserved;
    std::uint32_t label_len;
};
#pragma pack(pop)

static_assert(sizeof(FileHeader) == 24, "FileHeader size drift");
static_assert(sizeof(EventHeader) == 24, "EventHeader size drift");
static_assert(sizeof(FileFooter) == 72, "FileFooter size drift");
static_assert(sizeof(As3ObjectEvent) == 24, "As3ObjectEvent size drift");
static_assert(sizeof(As3ReferenceEvent) == 16, "As3ReferenceEvent size drift");
static_assert(sizeof(As3ReferenceExEvent) == 24, "As3ReferenceExEvent size drift");
static_assert(sizeof(As3RootEvent) == 16, "As3RootEvent size drift");
static_assert(sizeof(As3PayloadEvent) == 40, "As3PayloadEvent size drift");
static_assert(sizeof(FrameEvent) == 32, "FrameEvent size drift");
static_assert(sizeof(GcCycleEvent) == 48, "GcCycleEvent size drift");

inline std::array<std::uint8_t, sizeof(FileHeader)>
make_header_bytes(std::uint32_t header_json_len, std::uint64_t started_utc) {
    FileHeader h{};
    std::memcpy(h.magic, kHeaderMagic, sizeof(h.magic));
    h.version = kFormatVersion;
    h.reserved = 0;
    h.header_json_len = header_json_len;
    h.started_utc = started_utc;
    std::array<std::uint8_t, sizeof(FileHeader)> out{};
    std::memcpy(out.data(), &h, sizeof(h));
    return out;
}

inline std::array<std::uint8_t, sizeof(EventHeader)>
make_event_header_bytes(EventType type,
                        std::uint32_t payload_size,
                        std::uint64_t timestamp_ns,
                        std::uint32_t thread_id,
                        std::uint16_t flags = 0) {
    EventHeader h{};
    h.type = static_cast<std::uint16_t>(type);
    h.flags = flags;
    h.payload_size = payload_size;
    h.timestamp_ns = timestamp_ns;
    h.thread_id = thread_id;
    h.reserved = 0;
    std::array<std::uint8_t, sizeof(EventHeader)> out{};
    std::memcpy(out.data(), &h, sizeof(h));
    return out;
}

inline std::array<std::uint8_t, sizeof(FileFooter)>
make_footer_bytes(std::uint64_t event_count,
                  std::uint64_t dropped_count,
                  std::uint64_t payload_bytes,
                  std::uint64_t ended_utc,
                  std::uint64_t live_allocations,
                  std::uint64_t live_bytes,
                  std::uint32_t crc32_events = 0) {
    FileFooter f{};
    std::memcpy(f.magic, kFooterMagic, sizeof(f.magic));
    f.version = kFormatVersion;
    f.reserved = 0;
    f.footer_size = static_cast<std::uint32_t>(sizeof(FileFooter));
    f.event_count = event_count;
    f.dropped_count = dropped_count;
    f.payload_bytes = payload_bytes;
    f.ended_utc = ended_utc;
    f.live_allocations = live_allocations;
    f.live_bytes = live_bytes;
    f.crc32_events = crc32_events;
    f.reserved2 = 0;
    std::array<std::uint8_t, sizeof(FileFooter)> out{};
    std::memcpy(out.data(), &f, sizeof(f));
    return out;
}

inline bool parse_header_bytes(const void* src, FileHeader* out) noexcept {
    if (src == nullptr || out == nullptr) return false;
    std::memcpy(out, src, sizeof(FileHeader));
    return std::memcmp(out->magic, kHeaderMagic, sizeof(out->magic)) == 0 &&
           out->version == kFormatVersion;
}

inline bool parse_event_header_bytes(const void* src, EventHeader* out) noexcept {
    if (src == nullptr || out == nullptr) return false;
    std::memcpy(out, src, sizeof(EventHeader));
    return out->type >= static_cast<std::uint16_t>(EventType::Start) &&
           out->type <= static_cast<std::uint16_t>(EventType::GcCycle);
}

inline bool parse_footer_bytes(const void* src, FileFooter* out) noexcept {
    if (src == nullptr || out == nullptr) return false;
    std::memcpy(out, src, sizeof(FileFooter));
    return std::memcmp(out->magic, kFooterMagic, sizeof(out->magic)) == 0 &&
           out->version == kFormatVersion &&
           out->footer_size == sizeof(FileFooter);
}

} // namespace ane::profiler::aneprof

#endif // ANE_PROFILER_ANEPROF_FORMAT_HPP
