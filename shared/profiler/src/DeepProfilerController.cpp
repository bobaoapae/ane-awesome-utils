#include "DeepProfilerController.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstring>
#include <functional>
#include <thread>
#include <utility>

namespace ane::profiler {

namespace {
thread_local std::vector<std::uint32_t> t_method_stack;

std::string default_header_json() {
    return R"({"format":"aneprof","formatVersion":1,"backend":"deep-native","platform":"windows","airSdk":"51.1.3.10","wireProtocol":"aneprof-events"})";
}

template <typename T>
bool write_pod(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    return static_cast<bool>(out);
}

template <typename T>
std::vector<std::uint8_t> fixed_with_label_payload(const T& fixed, const std::string& label) {
    std::vector<std::uint8_t> payload(sizeof(T) + label.size());
    std::memcpy(payload.data(), &fixed, sizeof(T));
    if (!label.empty()) {
        std::memcpy(payload.data() + sizeof(T), label.data(), label.size());
    }
    return payload;
}
} // namespace

DeepProfilerController::DeepProfilerController() = default;

DeepProfilerController::~DeepProfilerController() {
    stop();
}

std::uint64_t DeepProfilerController::now_ns() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

std::uint32_t DeepProfilerController::thread_id() {
    thread_local const std::uint32_t cached_thread_id = [] {
        const auto h = std::hash<std::thread::id>{}(std::this_thread::get_id());
        return static_cast<std::uint32_t>(h & 0xffffffffu);
    }();
    return cached_thread_id;
}

std::size_t DeepProfilerController::allocation_shard_index(std::uintptr_t ptr) {
    return (ptr >> 4) & (kAllocationShardCount - 1);
}

void DeepProfilerController::clear_allocation_shards() {
    for (auto& shard : allocation_shards_) {
        std::lock_guard<std::mutex> alloc_lock(shard.mu);
        shard.entries.clear();
    }
}

void DeepProfilerController::reserve_allocation_shards() {
    for (auto& shard : allocation_shards_) {
        std::lock_guard<std::mutex> alloc_lock(shard.mu);
        shard.entries.max_load_factor(0.7f);
        shard.entries.reserve(kAllocationReservePerShard);
    }
}

std::pair<std::uint64_t, std::uint64_t> DeepProfilerController::live_allocation_totals() const {
    std::uint64_t live_count = 0;
    std::uint64_t live_bytes = 0;
    for (const auto& shard : allocation_shards_) {
        std::lock_guard<std::mutex> alloc_lock(shard.mu);
        live_count += shard.entries.size();
        for (const auto& kv : shard.entries) {
            live_bytes += kv.second.size;
        }
    }
    return {live_count, live_bytes};
}

bool DeepProfilerController::start(const Config& cfg) {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
    if (state_.load(std::memory_order_acquire) != State::Idle) return false;

    state_.store(State::Starting, std::memory_order_release);
    cfg_ = cfg;
    if (cfg_.header_json.empty()) {
        cfg_.header_json = default_header_json();
    }

    events_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
    payload_bytes_.store(0, std::memory_order_relaxed);
    total_allocations_.store(0, std::memory_order_relaxed);
    total_frees_.store(0, std::memory_order_relaxed);
    total_reallocations_.store(0, std::memory_order_relaxed);
    unknown_frees_.store(0, std::memory_order_relaxed);
    writer_events_written_.store(0, std::memory_order_relaxed);
    writer_bytes_written_.store(0, std::memory_order_relaxed);
    clear_allocation_shards();
    if (cfg_.memory_enabled) {
        reserve_allocation_shards();
    }
    writer_queue_ = std::make_unique<WriterQueueSlot[]>(kWriterQueueCapacity);
    for (std::size_t i = 0; i < kWriterQueueCapacity; ++i) {
        writer_queue_[i].sequence.store(i, std::memory_order_relaxed);
        writer_queue_[i].event = PendingEvent{};
    }
    writer_enqueue_pos_.store(0, std::memory_order_relaxed);
    writer_dequeue_pos_.store(0, std::memory_order_relaxed);
    writer_count_.store(0, std::memory_order_relaxed);
    writer_stop_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> snapshot_lock(snapshot_thread_mu_);
        snapshot_thread_stop_ = false;
    }

    file_buffer_.assign(kFileBufferBytes, 0);
    file_.rdbuf()->pubsetbuf(file_buffer_.data(),
                             static_cast<std::streamsize>(file_buffer_.size()));
    file_.open(cfg_.output_path, std::ios::binary | std::ios::trunc);
    if (!file_.is_open()) {
        state_.store(State::Error, std::memory_order_release);
        return false;
    }

    const auto started_utc = static_cast<std::uint64_t>(std::time(nullptr));
    const auto header = aneprof::make_header_bytes(
        static_cast<std::uint32_t>(cfg_.header_json.size()), started_utc);
    file_.write(reinterpret_cast<const char*>(header.data()),
                static_cast<std::streamsize>(header.size()));
    file_.write(cfg_.header_json.data(), static_cast<std::streamsize>(cfg_.header_json.size()));
    if (!file_) {
        file_.close();
        state_.store(State::Error, std::memory_order_release);
        return false;
    }

    started_ns_ = now_ns();
    state_.store(State::Recording, std::memory_order_release);
    writer_thread_ = std::thread(&DeepProfilerController::writer_thread_main, this);

    write_event(aneprof::EventType::Start, nullptr, 0);
    if (cfg_.snapshots_enabled) {
        write_snapshot_events("initial", false);
    }
    if (cfg_.snapshots_enabled && cfg_.snapshot_interval_ms != 0) {
        snapshot_thread_ = std::thread(&DeepProfilerController::snapshot_thread_main, this);
    }
    return true;
}

bool DeepProfilerController::stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
    const State s = state_.load(std::memory_order_acquire);
    if (s == State::Idle) return true;
    if (s != State::Recording && s != State::Starting && s != State::Error) return false;

    state_.store(State::Stopping, std::memory_order_release);
    {
        std::lock_guard<std::mutex> snapshot_lock(snapshot_thread_mu_);
        snapshot_thread_stop_ = true;
    }
    snapshot_thread_cv_.notify_all();
    if (snapshot_thread_.joinable()) {
        snapshot_thread_.join();
    }

    if (cfg_.snapshots_enabled) {
        write_snapshot_events("final", true);
    }
    write_event(aneprof::EventType::Stop, nullptr, 0);
    writer_stop_.store(true, std::memory_order_release);
    writer_cv_.notify_all();
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }

    const auto [live_count, live_bytes] = live_allocation_totals();

    if (file_.is_open()) {
        const auto footer = aneprof::make_footer_bytes(
            events_.load(std::memory_order_relaxed),
            dropped_.load(std::memory_order_relaxed),
            payload_bytes_.load(std::memory_order_relaxed),
            static_cast<std::uint64_t>(std::time(nullptr)),
            live_count,
            live_bytes);
        file_.write(reinterpret_cast<const char*>(footer.data()),
                    static_cast<std::streamsize>(footer.size()));
        file_.flush();
        file_.close();
    }
    writer_queue_.reset();
    writer_enqueue_pos_.store(0, std::memory_order_relaxed);
    writer_dequeue_pos_.store(0, std::memory_order_relaxed);
    writer_count_.store(0, std::memory_order_relaxed);
    file_buffer_.clear();

    state_.store(State::Idle, std::memory_order_release);
    return true;
}

bool DeepProfilerController::snapshot(const std::string& label) {
    if (!cfg_.snapshots_enabled) return true;
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;
    return write_snapshot_events(label.empty() ? "manual" : label, true);
}

bool DeepProfilerController::marker(const std::string& name, const std::string& value_json) {
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;

    const std::uint32_t name_len = static_cast<std::uint32_t>(name.size());
    const std::uint32_t value_len = static_cast<std::uint32_t>(value_json.size());
    std::vector<std::uint8_t> payload(sizeof(std::uint32_t) * 2 + name_len + value_len);
    std::memcpy(payload.data(), &name_len, sizeof(name_len));
    std::memcpy(payload.data() + sizeof(name_len), &value_len, sizeof(value_len));
    if (name_len != 0) {
        std::memcpy(payload.data() + sizeof(std::uint32_t) * 2, name.data(), name_len);
    }
    if (value_len != 0) {
        std::memcpy(payload.data() + sizeof(std::uint32_t) * 2 + name_len,
                    value_json.data(),
                    value_len);
    }
    return write_event(aneprof::EventType::Marker,
                       payload.data(),
                       static_cast<std::uint32_t>(payload.size()));
}

bool DeepProfilerController::method_enter(std::uint32_t method_id) {
    if (!cfg_.timing_enabled) return true;
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;
    t_method_stack.push_back(method_id);
    aneprof::MethodEvent payload{method_id, static_cast<std::uint32_t>(t_method_stack.size())};
    return write_event(aneprof::EventType::MethodEnter, &payload, sizeof(payload));
}

bool DeepProfilerController::method_exit(std::uint32_t method_id) {
    if (!cfg_.timing_enabled) return true;
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;
    const std::uint32_t depth = static_cast<std::uint32_t>(t_method_stack.size());
    aneprof::MethodEvent payload{method_id, depth};
    if (!t_method_stack.empty()) {
        t_method_stack.pop_back();
    }
    return write_event(aneprof::EventType::MethodExit, &payload, sizeof(payload));
}

bool DeepProfilerController::register_method_table(const void* data, std::uint32_t size) {
    if (data == nullptr || size == 0) return false;
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;
    return write_event(aneprof::EventType::MethodTable, data, size);
}

bool DeepProfilerController::record_alloc(void* ptr, std::uint64_t size) {
    if (!cfg_.memory_enabled) return true;
    if (ptr == nullptr) return false;
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;

    const auto ts = now_ns();
    const auto tid = thread_id();
    const auto method = current_method_id();

    aneprof::AllocationEvent payload{};
    payload.ptr = reinterpret_cast<std::uintptr_t>(ptr);
    payload.size = size;
    payload.method_id = method;
    return enqueue_allocation_event(aneprof::EventType::Alloc,
                                    payload,
                                    ts,
                                    tid,
                                    PendingWritePolicy::AllocTrack);
}

bool DeepProfilerController::record_alloc_if_untracked(void* ptr, std::uint64_t size) {
    if (!cfg_.memory_enabled) return true;
    if (ptr == nullptr) return false;
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;

    const auto ts = now_ns();
    const auto tid = thread_id();
    const auto method = current_method_id();

    aneprof::AllocationEvent payload{};
    payload.ptr = reinterpret_cast<std::uintptr_t>(ptr);
    payload.size = size;
    payload.method_id = method;
    return enqueue_allocation_event(aneprof::EventType::Alloc,
                                    payload,
                                    ts,
                                    tid,
                                    PendingWritePolicy::AllocIfUntracked);
}

bool DeepProfilerController::record_free(void* ptr) {
    if (!cfg_.memory_enabled) return true;
    if (ptr == nullptr) return false;
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;

    aneprof::AllocationEvent payload{};
    payload.ptr = reinterpret_cast<std::uintptr_t>(ptr);
    payload.method_id = current_method_id();
    return enqueue_allocation_event(aneprof::EventType::Free,
                                    payload,
                                    now_ns(),
                                    thread_id(),
                                    PendingWritePolicy::FreeTrack);
}

bool DeepProfilerController::record_free_if_tracked(void* ptr) {
    if (!cfg_.memory_enabled) return true;
    if (ptr == nullptr) return false;
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;

    aneprof::AllocationEvent payload{};
    payload.ptr = reinterpret_cast<std::uintptr_t>(ptr);
    payload.method_id = current_method_id();
    return enqueue_allocation_event(aneprof::EventType::Free,
                                    payload,
                                    now_ns(),
                                    thread_id(),
                                    PendingWritePolicy::FreeIfTracked);
}

bool DeepProfilerController::record_realloc(void* old_ptr, void* new_ptr, std::uint64_t new_size) {
    if (!cfg_.memory_enabled) return true;
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;

    if (old_ptr == nullptr) return record_alloc(new_ptr, new_size);
    if (new_ptr == nullptr) return record_free(old_ptr);

    const auto ts = now_ns();
    const auto tid = thread_id();
    const auto method = current_method_id();

    aneprof::AllocationEvent payload{};
    payload.ptr = reinterpret_cast<std::uintptr_t>(new_ptr);
    payload.size = new_size;
    payload.old_ptr = reinterpret_cast<std::uintptr_t>(old_ptr);
    payload.method_id = method;
    return enqueue_allocation_event(aneprof::EventType::Realloc,
                                    payload,
                                    ts,
                                    tid,
                                    PendingWritePolicy::ReallocTrack);
}

bool DeepProfilerController::record_realloc_if_tracked(void* old_ptr,
                                                       void* new_ptr,
                                                       std::uint64_t new_size) {
    if (!cfg_.memory_enabled) return true;
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;

    if (old_ptr == nullptr) return false;
    if (new_ptr == nullptr) return record_free_if_tracked(old_ptr);

    const auto ts = now_ns();
    const auto tid = thread_id();
    const auto method = current_method_id();

    aneprof::AllocationEvent payload{};
    payload.ptr = reinterpret_cast<std::uintptr_t>(new_ptr);
    payload.size = new_size;
    payload.old_ptr = reinterpret_cast<std::uintptr_t>(old_ptr);
    payload.method_id = method;
    return enqueue_allocation_event(aneprof::EventType::Realloc,
                                    payload,
                                    ts,
                                    tid,
                                    PendingWritePolicy::ReallocIfTracked);
}

std::uint64_t DeepProfilerController::tracked_allocation_size(void* ptr) const {
    if (ptr == nullptr) return 0;
    const auto key = reinterpret_cast<std::uintptr_t>(ptr);
    const auto& shard = allocation_shards_[allocation_shard_index(key)];
    std::lock_guard<std::mutex> alloc_lock(shard.mu);
    auto it = shard.entries.find(key);
    return it == shard.entries.end() ? 0 : it->second.size;
}

bool DeepProfilerController::record_as3_alloc(std::uint64_t sample_id,
                                              const std::string& type_name,
                                              std::uint64_t size,
                                              const std::string& stack) {
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;

    aneprof::As3ObjectEvent fixed{};
    fixed.sample_id = sample_id;
    fixed.size = size;
    fixed.type_name_len = static_cast<std::uint32_t>(type_name.size());
    fixed.stack_len = static_cast<std::uint32_t>(stack.size());

    std::vector<std::uint8_t> payload(sizeof(fixed) + type_name.size() + stack.size());
    std::memcpy(payload.data(), &fixed, sizeof(fixed));
    if (!type_name.empty()) {
        std::memcpy(payload.data() + sizeof(fixed), type_name.data(), type_name.size());
    }
    if (!stack.empty()) {
        std::memcpy(payload.data() + sizeof(fixed) + type_name.size(), stack.data(), stack.size());
    }
    return write_event(aneprof::EventType::As3Alloc,
                       payload.data(),
                       static_cast<std::uint32_t>(payload.size()));
}

bool DeepProfilerController::record_as3_free(std::uint64_t sample_id,
                                             const std::string& type_name,
                                             std::uint64_t size) {
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;

    aneprof::As3ObjectEvent fixed{};
    fixed.sample_id = sample_id;
    fixed.size = size;
    fixed.type_name_len = static_cast<std::uint32_t>(type_name.size());
    fixed.stack_len = 0;

    std::vector<std::uint8_t> payload(sizeof(fixed) + type_name.size());
    std::memcpy(payload.data(), &fixed, sizeof(fixed));
    if (!type_name.empty()) {
        std::memcpy(payload.data() + sizeof(fixed), type_name.data(), type_name.size());
    }
    return write_event(aneprof::EventType::As3Free,
                       payload.data(),
                       static_cast<std::uint32_t>(payload.size()));
}

bool DeepProfilerController::record_as3_reference(std::uint64_t owner_id,
                                                  std::uint64_t dependent_id) {
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;
    if (owner_id == 0 || dependent_id == 0 || owner_id == dependent_id) return true;

    aneprof::As3ReferenceEvent payload{};
    payload.owner_id = owner_id;
    payload.dependent_id = dependent_id;
    return write_event(aneprof::EventType::As3Reference, &payload, sizeof(payload));
}

bool DeepProfilerController::record_as3_reference_ex(std::uint64_t owner_id,
                                                     std::uint64_t dependent_id,
                                                     aneprof::As3ReferenceKind kind,
                                                     const std::string& label,
                                                     bool inferred) {
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;
    if (owner_id == 0 || dependent_id == 0 || owner_id == dependent_id) return true;

    aneprof::As3ReferenceExEvent fixed{};
    fixed.owner_id = owner_id;
    fixed.dependent_id = dependent_id;
    fixed.kind = static_cast<std::uint16_t>(kind);
    fixed.label_len = static_cast<std::uint32_t>(label.size());
    auto payload = fixed_with_label_payload(fixed, label);
    const std::uint16_t flags = inferred ? aneprof::EventFlagInferred : 0;
    return write_event_locked(aneprof::EventType::As3ReferenceEx,
                              payload.data(),
                              static_cast<std::uint32_t>(payload.size()),
                              now_ns(),
                              thread_id(),
                              flags);
}

bool DeepProfilerController::record_as3_reference_remove(std::uint64_t owner_id,
                                                         std::uint64_t dependent_id,
                                                         aneprof::As3ReferenceKind kind,
                                                         const std::string& label) {
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;
    if (owner_id == 0 || dependent_id == 0 || owner_id == dependent_id) return true;

    aneprof::As3ReferenceExEvent fixed{};
    fixed.owner_id = owner_id;
    fixed.dependent_id = dependent_id;
    fixed.kind = static_cast<std::uint16_t>(kind);
    fixed.label_len = static_cast<std::uint32_t>(label.size());
    auto payload = fixed_with_label_payload(fixed, label);
    return write_event_locked(aneprof::EventType::As3ReferenceRemove,
                              payload.data(),
                              static_cast<std::uint32_t>(payload.size()),
                              now_ns(),
                              thread_id(),
                              0);
}

bool DeepProfilerController::record_as3_root(std::uint64_t object_id,
                                             aneprof::As3RootKind kind,
                                             const std::string& label,
                                             bool inferred) {
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;
    if (object_id == 0) return true;

    aneprof::As3RootEvent fixed{};
    fixed.object_id = object_id;
    fixed.kind = static_cast<std::uint16_t>(kind);
    fixed.label_len = static_cast<std::uint32_t>(label.size());
    auto payload = fixed_with_label_payload(fixed, label);
    const std::uint16_t flags = inferred ? aneprof::EventFlagInferred : 0;
    return write_event_locked(aneprof::EventType::As3Root,
                              payload.data(),
                              static_cast<std::uint32_t>(payload.size()),
                              now_ns(),
                              thread_id(),
                              flags);
}

bool DeepProfilerController::record_as3_payload(std::uint64_t owner_id,
                                                std::uint64_t payload_id,
                                                aneprof::As3PayloadKind kind,
                                                std::uint64_t logical_bytes,
                                                std::uint64_t native_bytes,
                                                const std::string& label,
                                                bool inferred) {
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;
    if (owner_id == 0 && payload_id == 0) return true;

    aneprof::As3PayloadEvent fixed{};
    fixed.owner_id = owner_id;
    fixed.payload_id = payload_id;
    fixed.logical_bytes = logical_bytes;
    fixed.native_bytes = native_bytes;
    fixed.kind = static_cast<std::uint16_t>(kind);
    fixed.label_len = static_cast<std::uint32_t>(label.size());
    auto payload = fixed_with_label_payload(fixed, label);
    const std::uint16_t flags = inferred ? aneprof::EventFlagInferred : 0;
    return write_event_locked(aneprof::EventType::As3Payload,
                              payload.data(),
                              static_cast<std::uint32_t>(payload.size()),
                              now_ns(),
                              thread_id(),
                              flags);
}

bool DeepProfilerController::record_frame(std::uint64_t frame_index,
                                          std::uint64_t duration_ns,
                                          std::uint32_t allocation_count,
                                          std::uint64_t allocation_bytes,
                                          const std::string& label) {
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;

    aneprof::FrameEvent fixed{};
    fixed.frame_index = frame_index;
    fixed.duration_ns = duration_ns;
    fixed.allocation_bytes = allocation_bytes;
    fixed.allocation_count = allocation_count;
    fixed.label_len = static_cast<std::uint32_t>(label.size());
    auto payload = fixed_with_label_payload(fixed, label);
    return write_event(aneprof::EventType::Frame,
                       payload.data(),
                       static_cast<std::uint32_t>(payload.size()));
}

bool DeepProfilerController::record_render_frame(std::uint64_t frame_index,
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
                                                 const std::string& label) {
    if (!cfg_.render_enabled) return true;
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;

    aneprof::RenderFrameEvent fixed{};
    fixed.frame_index = frame_index;
    fixed.interval_ns = interval_ns;
    fixed.cpu_between_presents_ns = cpu_between_presents_ns;
    fixed.present_ns = present_ns;
    fixed.draw_calls = draw_calls;
    fixed.primitive_count = primitive_count;
    fixed.texture_upload_bytes = texture_upload_bytes;
    fixed.texture_create_bytes = texture_create_bytes;
    fixed.texture_create_count = texture_create_count;
    fixed.texture_update_count = texture_update_count;
    fixed.set_texture_count = set_texture_count;
    fixed.render_target_change_count = render_target_change_count;
    fixed.clear_count = clear_count;
    fixed.present_result = present_result;
    fixed.label_len = static_cast<std::uint32_t>(label.size());
    auto payload = fixed_with_label_payload(fixed, label);
    return write_event(aneprof::EventType::RenderFrame,
                       payload.data(),
                       static_cast<std::uint32_t>(payload.size()));
}

bool DeepProfilerController::record_gc_cycle(std::uint64_t gc_id,
                                             aneprof::GcCycleKind kind,
                                             std::uint64_t before_live_count,
                                             std::uint64_t before_live_bytes,
                                             std::uint64_t after_live_count,
                                             std::uint64_t after_live_bytes,
                                             const std::string& label,
                                             std::uint16_t flags) {
    if (state_.load(std::memory_order_acquire) != State::Recording) return false;

    aneprof::GcCycleEvent fixed{};
    fixed.gc_id = gc_id;
    fixed.before_live_bytes = before_live_bytes;
    fixed.after_live_bytes = after_live_bytes;
    fixed.before_live_count = before_live_count;
    fixed.after_live_count = after_live_count;
    fixed.kind = static_cast<std::uint16_t>(kind);
    fixed.label_len = static_cast<std::uint32_t>(label.size());
    auto payload = fixed_with_label_payload(fixed, label);
    return write_event_locked(aneprof::EventType::GcCycle,
                              payload.data(),
                              static_cast<std::uint32_t>(payload.size()),
                              now_ns(),
                              thread_id(),
                              flags);
}

DeepProfilerController::Status DeepProfilerController::status() const {
    Status s{};
    s.state = state_.load(std::memory_order_acquire);
    s.events = events_.load(std::memory_order_relaxed);
    s.dropped = dropped_.load(std::memory_order_relaxed);
    s.payload_bytes = payload_bytes_.load(std::memory_order_relaxed);
    s.total_allocations = total_allocations_.load(std::memory_order_relaxed);
    s.total_frees = total_frees_.load(std::memory_order_relaxed);
    s.total_reallocations = total_reallocations_.load(std::memory_order_relaxed);
    s.unknown_frees = unknown_frees_.load(std::memory_order_relaxed);
    s.writer_queue_capacity = kWriterQueueCapacity;
    s.writer_queue_depth = writer_count_.load(std::memory_order_relaxed);
    s.writer_events_written = writer_events_written_.load(std::memory_order_relaxed);
    s.writer_bytes_written = writer_bytes_written_.load(std::memory_order_relaxed);
    s.timing_enabled = cfg_.timing_enabled;
    s.memory_enabled = cfg_.memory_enabled;
    s.render_enabled = cfg_.render_enabled;
    s.snapshots_enabled = cfg_.snapshots_enabled;
    if (s.state == State::Recording || s.state == State::Stopping) {
        s.elapsed_ms = (now_ns() - started_ns_) / 1000000ull;
    }
    const auto [live_allocations, live_bytes] = live_allocation_totals();
    s.live_allocations = live_allocations;
    s.live_bytes = live_bytes;
    return s;
}

std::uint32_t DeepProfilerController::current_method_id() const {
    if (t_method_stack.empty()) return 0;
    return t_method_stack.back();
}

bool DeepProfilerController::write_event(aneprof::EventType type,
                                         const void* payload,
                                         std::uint32_t size) {
    return write_event_locked(type, payload, size, now_ns(), thread_id());
}

bool DeepProfilerController::write_event_locked(aneprof::EventType type,
                                                const void* payload,
                                                std::uint32_t size,
                                                std::uint64_t timestamp_ns,
                                                std::uint32_t tid,
                                                std::uint16_t flags,
                                                PendingWritePolicy write_policy) {
    if (state_.load(std::memory_order_acquire) == State::Idle) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (payload == nullptr && size != 0) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const std::uint32_t record_size =
        static_cast<std::uint32_t>(sizeof(aneprof::EventHeader) + size);
    PendingEvent event{};
    event.record_size = record_size;
    event.payload_size = size;
    event.heap_backed = record_size > kInlineRecordBytes;
    event.event_type = type;
    event.write_policy = write_policy;

    std::uint8_t* dst = nullptr;
    if (event.heap_backed) {
        event.heap_record.resize(record_size);
        dst = event.heap_record.data();
    } else {
        dst = event.inline_record.data();
    }

    const auto header = aneprof::make_event_header_bytes(type, size, timestamp_ns, tid, flags);
    std::memcpy(dst, header.data(), header.size());
    if (size != 0) {
        std::memcpy(dst + sizeof(aneprof::EventHeader), payload, size);
    }

    if (!enqueue_event(std::move(event))) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool DeepProfilerController::enqueue_allocation_event(aneprof::EventType type,
                                                      const aneprof::AllocationEvent& payload,
                                                      std::uint64_t timestamp_ns,
                                                      std::uint32_t tid,
                                                      PendingWritePolicy write_policy) {
    return write_event_locked(type,
                              &payload,
                              static_cast<std::uint32_t>(sizeof(payload)),
                              timestamp_ns,
                              tid,
                              0,
                              write_policy);
}

bool DeepProfilerController::enqueue_event(PendingEvent&& event) {
    if (writer_queue_ == nullptr) {
        return false;
    }

    WriterQueueSlot* slot = nullptr;
    std::size_t pos = writer_enqueue_pos_.load(std::memory_order_relaxed);
    for (;;) {
        slot = &writer_queue_[pos % kWriterQueueCapacity];
        const std::size_t seq = slot->sequence.load(std::memory_order_acquire);
        const auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
        if (diff == 0) {
            if (writer_enqueue_pos_.compare_exchange_weak(pos,
                                                          pos + 1,
                                                          std::memory_order_relaxed,
                                                          std::memory_order_relaxed)) {
                break;
            }
            continue;
        }
        if (diff < 0) {
            std::this_thread::yield();
            pos = writer_enqueue_pos_.load(std::memory_order_relaxed);
            continue;
        }
        pos = writer_enqueue_pos_.load(std::memory_order_relaxed);
    }

    slot->event = std::move(event);
    slot->sequence.store(pos + 1, std::memory_order_release);
    const auto previous_count = writer_count_.fetch_add(1, std::memory_order_relaxed);
    if (previous_count == 0) {
        writer_cv_.notify_one();
    }
    return true;
}

bool DeepProfilerController::dequeue_event(PendingEvent& event) {
    if (writer_queue_ == nullptr) return false;

    WriterQueueSlot* slot = nullptr;
    std::size_t pos = writer_dequeue_pos_.load(std::memory_order_relaxed);
    for (;;) {
        slot = &writer_queue_[pos % kWriterQueueCapacity];
        const std::size_t seq = slot->sequence.load(std::memory_order_acquire);
        const auto diff =
            static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
        if (diff == 0) {
            if (writer_dequeue_pos_.compare_exchange_weak(pos,
                                                          pos + 1,
                                                          std::memory_order_relaxed,
                                                          std::memory_order_relaxed)) {
                break;
            }
            continue;
        }
        if (diff < 0) return false;
        pos = writer_dequeue_pos_.load(std::memory_order_relaxed);
    }

    event = std::move(slot->event);
    slot->event = PendingEvent{};
    slot->sequence.store(pos + kWriterQueueCapacity, std::memory_order_release);
    writer_count_.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

bool DeepProfilerController::prepare_event_for_write(PendingEvent& event) {
    if (event.write_policy == PendingWritePolicy::Always) return true;
    if ((event.event_type != aneprof::EventType::Alloc &&
         event.event_type != aneprof::EventType::Free &&
         event.event_type != aneprof::EventType::Realloc) ||
        event.payload_size != sizeof(aneprof::AllocationEvent) ||
        event.record_size < sizeof(aneprof::EventHeader) + sizeof(aneprof::AllocationEvent)) {
        return true;
    }

    auto* payload = reinterpret_cast<aneprof::AllocationEvent*>(
        event.mutable_data() + sizeof(aneprof::EventHeader));
    const auto* header = reinterpret_cast<const aneprof::EventHeader*>(event.data());
    return update_allocation_tracking(event.event_type,
                                      event.write_policy,
                                      *payload,
                                      header->timestamp_ns,
                                      header->thread_id);
}

bool DeepProfilerController::update_allocation_tracking(aneprof::EventType type,
                                                        PendingWritePolicy policy,
                                                        aneprof::AllocationEvent& payload,
                                                        std::uint64_t timestamp_ns,
                                                        std::uint32_t tid) {
    if (type == aneprof::EventType::Alloc) {
        const auto key = static_cast<std::uintptr_t>(payload.ptr);
        auto& shard = allocation_shards_[allocation_shard_index(key)];
        std::lock_guard<std::mutex> alloc_lock(shard.mu);
        if (policy == PendingWritePolicy::AllocIfUntracked &&
            shard.entries.find(key) != shard.entries.end()) {
            return false;
        }
        shard.entries[key] = AllocationMeta{
            payload.size, timestamp_ns, tid, payload.method_id
        };
        total_allocations_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (type == aneprof::EventType::Free) {
        const auto key = static_cast<std::uintptr_t>(payload.ptr);
        auto& shard = allocation_shards_[allocation_shard_index(key)];
        std::lock_guard<std::mutex> alloc_lock(shard.mu);
        auto it = shard.entries.find(key);
        if (it == shard.entries.end()) {
            if (policy == PendingWritePolicy::FreeIfTracked) return false;
            unknown_frees_.fetch_add(1, std::memory_order_relaxed);
        } else {
            payload.old_size = it->second.size;
            shard.entries.erase(it);
        }
        total_frees_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (type == aneprof::EventType::Realloc) {
        const auto old_key = static_cast<std::uintptr_t>(payload.old_ptr);
        const auto new_key = static_cast<std::uintptr_t>(payload.ptr);
        auto& old_shard = allocation_shards_[allocation_shard_index(old_key)];
        auto& new_shard = allocation_shards_[allocation_shard_index(new_key)];

        auto update_locked = [&]() -> bool {
            auto old_it = old_shard.entries.find(old_key);
            if (old_it == old_shard.entries.end()) {
                if (policy == PendingWritePolicy::ReallocIfTracked) return false;
                unknown_frees_.fetch_add(1, std::memory_order_relaxed);
            } else {
                payload.old_size = old_it->second.size;
                old_shard.entries.erase(old_it);
            }
            new_shard.entries[new_key] = AllocationMeta{
                payload.size, timestamp_ns, tid, payload.method_id
            };
            total_reallocations_.fetch_add(1, std::memory_order_relaxed);
            return true;
        };

        if (&old_shard == &new_shard) {
            std::lock_guard<std::mutex> alloc_lock(old_shard.mu);
            return update_locked();
        }

        AllocationShard* first = &old_shard < &new_shard ? &old_shard : &new_shard;
        AllocationShard* second = &old_shard < &new_shard ? &new_shard : &old_shard;
        std::scoped_lock lock(first->mu, second->mu);
        return update_locked();
    }

    return true;
}

void DeepProfilerController::wait_for_writer_idle() const {
    while (writer_count_.load(std::memory_order_acquire) != 0 ||
           writer_dequeue_pos_.load(std::memory_order_acquire) <
               writer_enqueue_pos_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void DeepProfilerController::writer_thread_main() {
    for (;;) {
        PendingEvent event{};
        if (!dequeue_event(event)) {
            const auto enqueue_pos = writer_enqueue_pos_.load(std::memory_order_acquire);
            const auto dequeue_pos = writer_dequeue_pos_.load(std::memory_order_acquire);
            if (writer_stop_.load(std::memory_order_acquire) &&
                writer_count_.load(std::memory_order_acquire) == 0 &&
                dequeue_pos >= enqueue_pos) {
                break;
            }
            std::unique_lock<std::mutex> writer_wait(writer_wait_mu_);
            writer_cv_.wait_for(writer_wait, std::chrono::milliseconds(1), [&] {
                return writer_stop_.load(std::memory_order_acquire) ||
                       writer_count_.load(std::memory_order_acquire) != 0;
            });
            continue;
        }

        if (event.record_size == 0) continue;
        if (!prepare_event_for_write(event)) continue;
        file_.write(reinterpret_cast<const char*>(event.data()),
                    static_cast<std::streamsize>(event.record_size));
        if (!file_) {
            state_.store(State::Error, std::memory_order_release);
            dropped_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        events_.fetch_add(1, std::memory_order_relaxed);
        payload_bytes_.fetch_add(event.payload_size, std::memory_order_relaxed);
        writer_events_written_.fetch_add(1, std::memory_order_relaxed);
        writer_bytes_written_.fetch_add(event.record_size, std::memory_order_relaxed);
    }
}

bool DeepProfilerController::write_snapshot_events(const std::string& label,
                                                   bool include_live_entries) {
    if (cfg_.memory_enabled) {
        wait_for_writer_idle();
    }

    std::vector<std::pair<std::uintptr_t, AllocationMeta>> live;
    std::uint64_t live_count = 0;
    std::uint64_t live_bytes = 0;
    live.reserve(cfg_.max_live_allocations_per_snapshot);
    for (const auto& shard : allocation_shards_) {
        std::lock_guard<std::mutex> alloc_lock(shard.mu);
        live_count += shard.entries.size();
        for (const auto& kv : shard.entries) {
            live_bytes += kv.second.size;
            if (include_live_entries &&
                live.size() < cfg_.max_live_allocations_per_snapshot) {
                live.emplace_back(kv.first, kv.second);
            }
        }
    }

    aneprof::SnapshotEvent fixed{};
    fixed.live_allocations = live_count;
    fixed.live_bytes = live_bytes;
    fixed.total_allocations = total_allocations_.load(std::memory_order_relaxed);
    fixed.total_frees = total_frees_.load(std::memory_order_relaxed);
    fixed.total_reallocations = total_reallocations_.load(std::memory_order_relaxed);
    fixed.unknown_frees = unknown_frees_.load(std::memory_order_relaxed);
    fixed.label_len = static_cast<std::uint32_t>(label.size());

    std::vector<std::uint8_t> payload(sizeof(fixed) + label.size());
    std::memcpy(payload.data(), &fixed, sizeof(fixed));
    if (!label.empty()) {
        std::memcpy(payload.data() + sizeof(fixed), label.data(), label.size());
    }
    if (!write_event(aneprof::EventType::Snapshot,
                     payload.data(),
                     static_cast<std::uint32_t>(payload.size()))) {
        return false;
    }

    for (const auto& kv : live) {
        aneprof::LiveAllocationEvent ev{};
        ev.ptr = kv.first;
        ev.size = kv.second.size;
        ev.timestamp_ns = kv.second.timestamp_ns;
        ev.thread_id = kv.second.thread_id;
        ev.method_id = kv.second.method_id;
        if (!write_event(aneprof::EventType::LiveAllocation, &ev, sizeof(ev))) {
            return false;
        }
    }
    return true;
}

void DeepProfilerController::snapshot_thread_main() {
    const auto interval = std::chrono::milliseconds(cfg_.snapshot_interval_ms);
    std::unique_lock<std::mutex> lock(snapshot_thread_mu_);
    while (!snapshot_thread_stop_) {
        if (snapshot_thread_cv_.wait_for(lock, interval, [&] { return snapshot_thread_stop_; })) {
            break;
        }
        lock.unlock();
        if (state_.load(std::memory_order_acquire) == State::Recording) {
            write_snapshot_events("periodic", true);
        }
        lock.lock();
    }
}

} // namespace ane::profiler
