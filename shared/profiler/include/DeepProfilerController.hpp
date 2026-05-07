// DeepProfilerController — native .aneprof event writer.
//
// This controller is independent of Adobe Scout telemetry. Compiler probes,
// native runtime hooks, snapshots and user markers all flow into a compact
// binary event log with the .aneprof format.

#ifndef ANE_PROFILER_DEEP_PROFILER_CONTROLLER_HPP
#define ANE_PROFILER_DEEP_PROFILER_CONTROLLER_HPP

#include "AneprofFormat.hpp"

#include <atomic>
#include <condition_variable>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ane::profiler {

class DeepProfilerController {
public:
    struct Config {
        std::string output_path;
        std::string header_json;
        bool timing_enabled = true;
        bool memory_enabled = false;
        bool render_enabled = false;
        bool snapshots_enabled = true;
        std::uint32_t max_live_allocations_per_snapshot = 4096;
        std::uint32_t snapshot_interval_ms = 0;
    };

    enum class State : std::uint32_t {
        Idle      = 0,
        Starting  = 1,
        Recording = 2,
        Stopping  = 3,
        Error     = 4,
    };

    struct Status {
        State state = State::Idle;
        std::uint64_t events = 0;
        std::uint64_t dropped = 0;
        std::uint64_t payload_bytes = 0;
        std::uint64_t elapsed_ms = 0;
        std::uint64_t live_allocations = 0;
        std::uint64_t live_bytes = 0;
        std::uint64_t total_allocations = 0;
        std::uint64_t total_frees = 0;
        std::uint64_t total_reallocations = 0;
        std::uint64_t unknown_frees = 0;
        std::uint64_t writer_queue_depth = 0;
        std::uint64_t writer_queue_capacity = 0;
        std::uint64_t writer_events_written = 0;
        std::uint64_t writer_bytes_written = 0;
        std::uint64_t writer_overflow_depth = 0;
        std::uint64_t writer_overflow_peak = 0;
        std::uint64_t writer_overflow_events = 0;
        bool timing_enabled = false;
        bool memory_enabled = false;
        bool render_enabled = false;
        bool snapshots_enabled = false;
    };

    DeepProfilerController();
    ~DeepProfilerController();

    DeepProfilerController(const DeepProfilerController&) = delete;
    DeepProfilerController& operator=(const DeepProfilerController&) = delete;

    bool start(const Config& cfg);
    bool stop();
    bool snapshot(const std::string& label);
    bool marker(const std::string& name, const std::string& value_json);

    bool method_enter(std::uint32_t method_id);
    bool method_exit(std::uint32_t method_id);
    bool register_method_table(const void* data, std::uint32_t size);

    bool record_alloc(void* ptr, std::uint64_t size);
    bool record_alloc_if_untracked(void* ptr, std::uint64_t size);
    bool record_free(void* ptr);
    bool record_realloc(void* old_ptr, void* new_ptr, std::uint64_t new_size);
    bool record_free_if_tracked(void* ptr);
    bool record_realloc_if_tracked(void* old_ptr, void* new_ptr, std::uint64_t new_size);
    std::uint64_t tracked_allocation_size(void* ptr) const;
    bool record_as3_alloc(std::uint64_t sample_id,
                          const std::string& type_name,
                          std::uint64_t size,
                          const std::string& stack);
    // Hot-path variant: takes raw pointers + lengths to avoid two std::string
    // constructions on the per-event path (~150-300 ns/event saved on the
    // Phase 4a sampler proxy where 12k+ allocs/30s are typical).
    bool record_as3_alloc_raw(std::uint64_t sample_id,
                              const char* type_name, std::size_t type_name_len,
                              std::uint64_t size,
                              const char* stack, std::size_t stack_len);
    bool record_as3_free(std::uint64_t sample_id,
                         const std::string& type_name,
                         std::uint64_t size);
    bool record_as3_reference(std::uint64_t owner_id, std::uint64_t dependent_id);
    bool record_as3_reference_ex(std::uint64_t owner_id,
                                 std::uint64_t dependent_id,
                                 aneprof::As3ReferenceKind kind,
                                 const std::string& label = std::string(),
                                 bool inferred = false);
    bool record_as3_reference_remove(std::uint64_t owner_id,
                                     std::uint64_t dependent_id,
                                     aneprof::As3ReferenceKind kind,
                                     const std::string& label = std::string());
    bool record_as3_root(std::uint64_t object_id,
                         aneprof::As3RootKind kind,
                         const std::string& label = std::string(),
                         bool inferred = false);
    bool record_as3_payload(std::uint64_t owner_id,
                            std::uint64_t payload_id,
                            aneprof::As3PayloadKind kind,
                            std::uint64_t logical_bytes,
                            std::uint64_t native_bytes,
                            const std::string& label = std::string(),
                            bool inferred = false);
    bool record_frame(std::uint64_t frame_index,
                      std::uint64_t duration_ns,
                      std::uint32_t allocation_count,
                      std::uint64_t allocation_bytes,
                      const std::string& label = std::string());
    bool record_render_frame(std::uint64_t frame_index,
                             std::uint64_t interval_ns,
                             std::uint64_t cpu_between_presents_ns,
                             std::uint64_t present_ns,
                             std::uint64_t draw_calls,
                             std::uint64_t primitive_count,
                             std::uint64_t texture_upload_bytes,
                             std::uint64_t texture_create_bytes,
                             std::uint32_t texture_create_count,
                             std::uint32_t texture_update_count,
                             std::uint32_t set_texture_count,
                             std::uint32_t render_target_change_count,
                             std::uint32_t clear_count,
                             std::uint32_t present_result,
                             const std::string& label = std::string());
    bool record_gc_cycle(std::uint64_t gc_id,
                         aneprof::GcCycleKind kind,
                         std::uint64_t before_live_count,
                         std::uint64_t before_live_bytes,
                         std::uint64_t after_live_count,
                         std::uint64_t after_live_bytes,
                         const std::string& label = std::string(),
                         std::uint16_t flags = 0);

    State state() const noexcept { return state_.load(std::memory_order_acquire); }
    Status status() const;
    std::uint32_t current_method_id() const;

private:
    // 256 bytes inline buffer fits all common event records without heap
    // allocation. Markers (esp. as3_alloc_sampler with name + JSON payload)
    // are typically 110-150 bytes; staying inline avoids vector::resize
    // calling libc malloc, which is hooked by alloc_tracer and triggers
    // re-entrancy that has caused 2-byte file misalignments under load.
    static constexpr std::size_t kInlineRecordBytes = 256;
    static constexpr std::size_t kWriterQueueCapacity = 262144;
    static constexpr std::size_t kFileBufferBytes = 8 * 1024 * 1024;
    static constexpr std::size_t kAllocationShardCount = 256;
    static constexpr std::size_t kAllocationReservePerShard = 4096;

    struct AllocationMeta {
        std::uint64_t size = 0;
        std::uint64_t timestamp_ns = 0;
        std::uint32_t thread_id = 0;
        std::uint32_t method_id = 0;
    };

    enum class PendingWritePolicy : std::uint8_t {
        Always = 0,
        AllocTrack,
        AllocIfUntracked,
        FreeTrack,
        FreeIfTracked,
        ReallocTrack,
        ReallocIfTracked,
    };

    struct PendingEvent {
        std::uint64_t sequence = 0;
        std::uint32_t record_size = 0;
        std::uint32_t payload_size = 0;
        bool heap_backed = false;
        aneprof::EventType event_type = aneprof::EventType::Start;
        PendingWritePolicy write_policy = PendingWritePolicy::Always;
        std::array<std::uint8_t, kInlineRecordBytes> inline_record{};
        std::vector<std::uint8_t> heap_record;

        const std::uint8_t* data() const {
            return heap_backed ? heap_record.data() : inline_record.data();
        }

        std::uint8_t* mutable_data() {
            return heap_backed ? heap_record.data() : inline_record.data();
        }
    };

    struct WriterQueueSlot {
        std::atomic<std::size_t> sequence{0};
        PendingEvent event;
    };

    struct AllocationShard {
        mutable std::mutex mu;
        std::unordered_map<std::uintptr_t, AllocationMeta> entries;
    };

    bool write_event(aneprof::EventType type, const void* payload, std::uint32_t size);
    bool write_event_locked(aneprof::EventType type,
                            const void* payload,
                            std::uint32_t size,
                            std::uint64_t timestamp_ns,
                            std::uint32_t thread_id,
                            std::uint16_t flags = 0,
                            PendingWritePolicy write_policy = PendingWritePolicy::Always);
    bool enqueue_allocation_event(aneprof::EventType type,
                                  const aneprof::AllocationEvent& payload,
                                  std::uint64_t timestamp_ns,
                                  std::uint32_t thread_id,
                                  PendingWritePolicy write_policy);
    bool write_snapshot_events(const std::string& label, bool include_live_entries);
    bool enqueue_event(PendingEvent&& event);
    bool dequeue_event(PendingEvent& event);
    bool dequeue_next_event(PendingEvent& event);
    bool peek_ring_event_sequence(std::uint64_t& sequence) const;
    bool enqueue_overflow_event(PendingEvent&& event);
    bool dequeue_overflow_event(PendingEvent& event, std::uint64_t before_sequence);
    bool prepare_event_for_write(PendingEvent& event);
    bool update_allocation_tracking(aneprof::EventType type,
                                    PendingWritePolicy policy,
                                    aneprof::AllocationEvent& payload,
                                    std::uint64_t timestamp_ns,
                                    std::uint32_t thread_id);
    void wait_for_writer_idle() const;
    void writer_thread_main();
    void snapshot_thread_main();
    void clear_allocation_shards();
    void reserve_allocation_shards();
    std::pair<std::uint64_t, std::uint64_t> live_allocation_totals() const;

    static std::uint64_t now_ns();
    static std::uint32_t thread_id();
    static std::size_t allocation_shard_index(std::uintptr_t ptr);

    Config cfg_;
    std::atomic<State> state_{State::Idle};
    // Native FILE* avoids libc++ basic_filebuf + pubsetbuf, which produced
    // unaccounted stray bytes in event captures (see PROGRESS.md gap notes).
    // setvbuf with our own buffer keeps syscall count low without delegating
    // buffer management to filebuf.
    std::FILE* file_ = nullptr;
    std::vector<char> file_buffer_;
    mutable std::mutex lifecycle_mu_;
    mutable std::mutex file_mu_;
    mutable std::mutex writer_wait_mu_;
    std::condition_variable writer_cv_;
    std::unique_ptr<WriterQueueSlot[]> writer_queue_;
    std::thread writer_thread_;
    std::mutex writer_overflow_mu_;
    std::map<std::uint64_t, PendingEvent> writer_overflow_;
    std::atomic<std::uint64_t> writer_event_sequence_{0};
    std::atomic<std::size_t> writer_enqueue_pos_{0};
    std::atomic<std::size_t> writer_dequeue_pos_{0};
    std::atomic<std::size_t> writer_count_{0};
    std::atomic<std::uint64_t> writer_overflow_count_{0};
    std::atomic<std::uint64_t> writer_overflow_peak_{0};
    std::atomic<std::uint64_t> writer_overflow_events_{0};
    std::atomic<bool> writer_stop_{false};
    std::mutex snapshot_thread_mu_;
    std::condition_variable snapshot_thread_cv_;
    std::thread snapshot_thread_;
    bool snapshot_thread_stop_ = false;

    std::array<AllocationShard, kAllocationShardCount> allocation_shards_;

    std::atomic<std::uint64_t> events_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> payload_bytes_{0};
    std::atomic<std::uint64_t> total_allocations_{0};
    std::atomic<std::uint64_t> total_frees_{0};
    std::atomic<std::uint64_t> total_reallocations_{0};
    std::atomic<std::uint64_t> unknown_frees_{0};
    std::atomic<std::uint64_t> writer_events_written_{0};
    std::atomic<std::uint64_t> writer_bytes_written_{0};
    std::uint64_t started_ns_ = 0;
};

} // namespace ane::profiler

#endif // ANE_PROFILER_DEEP_PROFILER_CONTROLLER_HPP
