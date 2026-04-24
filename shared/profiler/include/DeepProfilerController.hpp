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
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ane::profiler {

class DeepProfilerController {
public:
    struct Config {
        std::string output_path;
        std::string header_json;
        bool timing_enabled = true;
        bool memory_enabled = false;
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
        bool timing_enabled = false;
        bool memory_enabled = false;
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
    bool record_as3_free(std::uint64_t sample_id,
                         const std::string& type_name,
                         std::uint64_t size);
    bool record_as3_reference(std::uint64_t owner_id, std::uint64_t dependent_id);
    bool record_as3_reference_ex(std::uint64_t owner_id,
                                 std::uint64_t dependent_id,
                                 aneprof::As3ReferenceKind kind,
                                 const std::string& label = std::string(),
                                 bool inferred = false);
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
    struct AllocationMeta {
        std::uint64_t size = 0;
        std::uint64_t timestamp_ns = 0;
        std::uint32_t thread_id = 0;
        std::uint32_t method_id = 0;
    };

    bool write_event(aneprof::EventType type, const void* payload, std::uint32_t size);
    bool write_event_locked(aneprof::EventType type,
                            const void* payload,
                            std::uint32_t size,
                            std::uint64_t timestamp_ns,
                            std::uint32_t thread_id,
                            std::uint16_t flags = 0);
    bool write_snapshot_events(const std::string& label, bool include_live_entries);
    void snapshot_thread_main();

    static std::uint64_t now_ns();
    static std::uint32_t thread_id();

    Config cfg_;
    std::atomic<State> state_{State::Idle};
    std::ofstream file_;
    mutable std::mutex lifecycle_mu_;
    mutable std::mutex file_mu_;
    mutable std::mutex alloc_mu_;
    std::mutex snapshot_thread_mu_;
    std::condition_variable snapshot_thread_cv_;
    std::thread snapshot_thread_;
    bool snapshot_thread_stop_ = false;

    std::unordered_map<std::uintptr_t, AllocationMeta> allocations_;

    std::atomic<std::uint64_t> events_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> payload_bytes_{0};
    std::atomic<std::uint64_t> total_allocations_{0};
    std::atomic<std::uint64_t> total_frees_{0};
    std::atomic<std::uint64_t> total_reallocations_{0};
    std::atomic<std::uint64_t> unknown_frees_{0};
    std::uint64_t started_ns_ = 0;
};

} // namespace ane::profiler

#endif // ANE_PROFILER_DEEP_PROFILER_CONTROLLER_HPP
