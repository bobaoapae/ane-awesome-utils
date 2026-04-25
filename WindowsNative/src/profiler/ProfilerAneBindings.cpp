#include "ProfilerAneBindings.hpp"

#include "DeepProfilerController.hpp"
#include "IDiskMonitor.hpp"
#include "profiler/WindowsAirRuntime.hpp"
#include "profiler/WindowsAs3ObjectHook.hpp"
#include "profiler/WindowsDeepMemoryHook.hpp"
#include "profiler/WindowsRenderHook.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace ane::profiler::bindings {

namespace {
std::mutex g_mu;
std::unique_ptr<DeepProfilerController> g_ctrl;
std::unique_ptr<IDiskMonitor> g_disk;
std::unique_ptr<WindowsDeepMemoryHook> g_memory_hook;
std::unique_ptr<WindowsAs3ObjectHook> g_as3_object_hook;
std::unique_ptr<WindowsRenderHook> g_render_hook;
std::unique_ptr<WindowsAirRuntime> g_air_runtime;

FREObject make_bool(bool v) {
    FREObject o = nullptr;
    FRENewObjectFromBool(v ? 1u : 0u, &o);
    return o;
}

FREObject make_u32(std::uint32_t v) {
    FREObject o = nullptr;
    FRENewObjectFromUint32(v, &o);
    return o;
}

FREObject make_f64(double v) {
    FREObject o = nullptr;
    FRENewObjectFromDouble(v, &o);
    return o;
}

FREObject make_string(const char* s) {
    FREObject o = nullptr;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(s ? s : "");
    FRENewObjectFromUTF8(static_cast<std::uint32_t>(std::strlen(reinterpret_cast<const char*>(bytes))),
                         bytes,
                         &o);
    return o;
}

std::string hex_ptr(std::uintptr_t value) {
    char buf[2 + sizeof(std::uintptr_t) * 2 + 1] = {};
#if defined(_M_X64) || defined(__x86_64__)
    std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(value));
#else
    std::snprintf(buf, sizeof(buf), "0x%08lx", static_cast<unsigned long>(value));
#endif
    return std::string(buf);
}

bool read_string(FREObject o, std::string& out) {
    std::uint32_t len = 0;
    const std::uint8_t* data = nullptr;
    if (FREGetObjectAsUTF8(o, &len, &data) != FRE_OK || data == nullptr) return false;
    out.assign(reinterpret_cast<const char*>(data), len);
    return true;
}

bool read_u32(FREObject o, std::uint32_t& out) {
    return FREGetObjectAsUint32(o, &out) == FRE_OK;
}

bool read_u64_number(FREObject o, std::uint64_t& out) {
    std::uint32_t u32 = 0;
    if (FREGetObjectAsUint32(o, &u32) == FRE_OK) {
        out = u32;
        return true;
    }
    double d = 0;
    if (FREGetObjectAsDouble(o, &d) != FRE_OK || d < 0) return false;
    out = static_cast<std::uint64_t>(d);
    return true;
}

bool read_bool(FREObject o, bool& out) {
    std::uint32_t v = 0;
    if (FREGetObjectAsBool(o, &v) != FRE_OK) return false;
    out = (v != 0);
    return true;
}

void set_prop_u32(FREObject obj, const char* name, std::uint32_t v) {
    FREObject tmp = make_u32(v);
    FRESetObjectProperty(obj, reinterpret_cast<const std::uint8_t*>(name), tmp, nullptr);
}

void set_prop_f64(FREObject obj, const char* name, double v) {
    FREObject tmp = make_f64(v);
    FRESetObjectProperty(obj, reinterpret_cast<const std::uint8_t*>(name), tmp, nullptr);
}

void set_prop_bool(FREObject obj, const char* name, bool v) {
    FREObject tmp = make_bool(v);
    FRESetObjectProperty(obj, reinterpret_cast<const std::uint8_t*>(name), tmp, nullptr);
}

void set_prop_string(FREObject obj, const char* name, const char* v) {
    FREObject tmp = make_string(v);
    FRESetObjectProperty(obj, reinterpret_cast<const std::uint8_t*>(name), tmp, nullptr);
}

bool ensure_controller() {
    if (!g_ctrl) g_ctrl = std::make_unique<DeepProfilerController>();
    if (!g_disk) g_disk = IDiskMonitor::create();
    if (!g_memory_hook) g_memory_hook = std::make_unique<WindowsDeepMemoryHook>();
    if (!g_as3_object_hook) g_as3_object_hook = std::make_unique<WindowsAs3ObjectHook>();
    if (!g_render_hook) g_render_hook = std::make_unique<WindowsRenderHook>();
    return static_cast<bool>(g_ctrl);
}

WindowsAirRuntime* ensure_air_runtime() {
    if (!g_air_runtime) g_air_runtime = std::make_unique<WindowsAirRuntime>();
    g_air_runtime->initialize();
    return g_air_runtime.get();
}

} // namespace

// profilerStart(outputPath:String,
//               headerJson:String,
//               timing:Boolean,
//               memory:Boolean,
//               snapshots:Boolean,
//               maxLive:uint,
//               snapshotIntervalMs:uint,
//               render:Boolean,
//               as3ObjectSampling:Boolean,
//               as3SamplerForwarding:Boolean,
//               as3RealDisplayEdges:Boolean,
//               as3RealEventEdges:Boolean): Boolean
FREObject profiler_start(FREContext, void*, std::uint32_t argc, FREObject* argv) {
    std::lock_guard<std::mutex> g(g_mu);
    if (argc < 1) return make_bool(false);
    if (!ensure_controller()) return make_bool(false);

    std::string output_path;
    if (!read_string(argv[0], output_path) || output_path.empty()) {
        return make_bool(false);
    }
    if (g_ctrl->state() != DeepProfilerController::State::Idle) {
        return make_bool(false);
    }

    std::string header_json;
    bool timing = true;
    bool memory = false;
    bool render = false;
    bool as3_object_sampling = true;
    bool as3_sampler_forwarding = false;
    bool as3_real_display_edges = true;
    bool as3_real_event_edges = true;
    bool snapshots = true;
    std::uint32_t max_live = 4096;
    std::uint32_t snapshot_interval_ms = 0;
    if (argc >= 2) read_string(argv[1], header_json);
    if (argc >= 3) read_bool(argv[2], timing);
    if (argc >= 4) read_bool(argv[3], memory);
    if (argc >= 5) read_bool(argv[4], snapshots);
    if (argc >= 6) read_u32(argv[5], max_live);
    if (argc >= 7) read_u32(argv[6], snapshot_interval_ms);
    if (argc >= 8) read_bool(argv[7], render);
    if (argc >= 9) read_bool(argv[8], as3_object_sampling);
    if (argc >= 10) read_bool(argv[9], as3_sampler_forwarding);
    if (argc >= 11) read_bool(argv[10], as3_real_display_edges);
    if (argc >= 12) read_bool(argv[11], as3_real_event_edges);

    if (g_disk) {
        constexpr std::uint64_t kMinFree = 200ull * 1024ull * 1024ull;
        const std::uint64_t free = g_disk->free_bytes(output_path);
        if (free != UINT64_MAX && free < kMinFree) return make_bool(false);
    }

    DeepProfilerController::Config cfg;
    cfg.output_path = std::move(output_path);
    cfg.header_json = std::move(header_json);
    cfg.timing_enabled = timing;
    cfg.memory_enabled = memory;
    cfg.render_enabled = render;
    cfg.snapshots_enabled = snapshots;
    cfg.max_live_allocations_per_snapshot = max_live == 0 ? 4096 : max_live;
    cfg.snapshot_interval_ms = snapshot_interval_ms;

    if (!g_ctrl->start(cfg)) return make_bool(false);
    if (memory) {
        if (!g_memory_hook->install(g_ctrl.get())) {
            g_ctrl->stop();
            return make_bool(false);
        }
        if (as3_object_sampling &&
            !g_as3_object_hook->install(g_ctrl.get(),
                                        as3_real_display_edges,
                                        as3_real_event_edges,
                                        as3_sampler_forwarding)) {
            // AS3 object sampling is useful but not required for a valid capture.
            // AIR/Scout advanced telemetry or flash.sampler can already own the
            // single IMemorySampler slot; keep the native allocation stream and
            // expose the AS3 hook failure through profilerGetStatus().
        }
    }
    if (render) {
        // Render metrics are optional enrichment. A D3D/DXGI hook failure should
        // not invalidate memory/timing captures; profilerGetStatus exposes it.
        g_render_hook->install(g_ctrl.get());
    }
    return make_bool(true);
}

FREObject profiler_stop(FREContext, void*, std::uint32_t, FREObject*) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_ctrl) return make_bool(true);
    if (g_render_hook) g_render_hook->pause();
    if (g_as3_object_hook) g_as3_object_hook->uninstall();
    if (g_memory_hook) g_memory_hook->uninstall();
    return make_bool(g_ctrl->stop());
}

FREObject profiler_snapshot(FREContext, void*, std::uint32_t argc, FREObject* argv) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_ctrl) return make_bool(false);
    std::string label;
    if (argc >= 1 && argv[0] != nullptr) read_string(argv[0], label);
    return make_bool(g_ctrl->snapshot(label));
}

FREObject profiler_marker(FREContext, void*, std::uint32_t argc, FREObject* argv) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_ctrl || argc < 1) return make_bool(false);
    std::string name;
    std::string value_json = "null";
    if (!read_string(argv[0], name) || name.empty()) return make_bool(false);
    if (argc >= 2 && argv[1] != nullptr) read_string(argv[1], value_json);
    return make_bool(g_ctrl->marker(name, value_json));
}

FREObject profiler_request_gc(FREContext ctx, void*, std::uint32_t, FREObject*) {
    std::lock_guard<std::mutex> g(g_mu);
    DeepProfilerController::Status before{};
    if (g_ctrl) before = g_ctrl->status();
    WindowsAirRuntime* runtime = ensure_air_runtime();
    const bool ok = runtime != nullptr && runtime->requestNativeGc(ctx);
    if (g_ctrl) {
        const DeepProfilerController::Status after = g_ctrl->status();
        const std::uint64_t gc_id = runtime ? runtime->nativeGcRequestCount() : 0;
        std::uint16_t flags = aneprof::EventFlagRequested;
        if (!ok) flags |= aneprof::EventFlagAfterUnknown;
        g_ctrl->record_gc_cycle(gc_id,
                                aneprof::GcCycleKind::NativeRequested,
                                before.live_allocations,
                                before.live_bytes,
                                after.live_allocations,
                                after.live_bytes,
                                ok ? "native-gc-requested" : "native-gc-request-failed",
                                flags);
    }
    return make_bool(ok);
}

FREObject profiler_get_status(FREContext, void*, std::uint32_t, FREObject*) {
    std::lock_guard<std::mutex> g(g_mu);
    FREObject obj = nullptr;
    FRENewObject(reinterpret_cast<const std::uint8_t*>("Object"), 0, nullptr, &obj, nullptr);
    if (obj == nullptr) return nullptr;

    DeepProfilerController::Status s{};
    if (g_ctrl) s = g_ctrl->status();

    set_prop_string(obj, "backend", "aneprof");
    set_prop_string(obj, "format", "aneprof");
    set_prop_u32(obj, "state", static_cast<std::uint32_t>(s.state));
    set_prop_f64(obj, "events", static_cast<double>(s.events));
    set_prop_f64(obj, "dropped", static_cast<double>(s.dropped));
    set_prop_f64(obj, "payloadBytes", static_cast<double>(s.payload_bytes));
    set_prop_u32(obj, "elapsedMs", static_cast<std::uint32_t>(s.elapsed_ms > 0xffffffffu
                                                                 ? 0xffffffffu
                                                                 : s.elapsed_ms));
    set_prop_f64(obj, "liveAllocations", static_cast<double>(s.live_allocations));
    set_prop_f64(obj, "liveBytes", static_cast<double>(s.live_bytes));
    set_prop_f64(obj, "totalAllocations", static_cast<double>(s.total_allocations));
    set_prop_f64(obj, "totalFrees", static_cast<double>(s.total_frees));
    set_prop_f64(obj, "totalReallocations", static_cast<double>(s.total_reallocations));
    set_prop_f64(obj, "unknownFrees", static_cast<double>(s.unknown_frees));
    set_prop_f64(obj, "writerQueueDepth", static_cast<double>(s.writer_queue_depth));
    set_prop_f64(obj, "writerQueueCapacity", static_cast<double>(s.writer_queue_capacity));
    set_prop_f64(obj, "writerEventsWritten", static_cast<double>(s.writer_events_written));
    set_prop_f64(obj, "writerBytesWritten", static_cast<double>(s.writer_bytes_written));
    set_prop_f64(obj, "writerOverflowDepth", static_cast<double>(s.writer_overflow_depth));
    set_prop_f64(obj, "writerOverflowPeak", static_cast<double>(s.writer_overflow_peak));
    set_prop_f64(obj, "writerOverflowEvents", static_cast<double>(s.writer_overflow_events));
    set_prop_bool(obj, "timingEnabled", s.timing_enabled);
    set_prop_bool(obj, "memoryEnabled", s.memory_enabled);
    set_prop_bool(obj, "renderEnabled", s.render_enabled);
    set_prop_bool(obj, "snapshotsEnabled", s.snapshots_enabled);
    set_prop_u32(obj, "memoryHookInstalled",
                 (g_memory_hook && g_memory_hook->installed()) ? 1u : 0u);
    set_prop_u32(obj, "as3ObjectHookInstalled",
                 (g_as3_object_hook && g_as3_object_hook->installed()) ? 1u : 0u);
    set_prop_bool(obj, "memoryFreeHooksInstalled",
                  g_memory_hook && g_memory_hook->freeHooksInstalled());
    set_prop_bool(obj, "memoryReallocHooksInstalled",
                  g_memory_hook && g_memory_hook->reallocHooksInstalled());
    set_prop_bool(obj, "memoryLeakDiagnosticsReady",
                  s.memory_enabled &&
                  g_memory_hook &&
                  g_memory_hook->installed() &&
                  g_memory_hook->freeHooksInstalled() &&
                  g_memory_hook->reallocHooksInstalled());
    set_prop_bool(obj, "as3LeakDiagnosticsReady",
                  s.memory_enabled &&
                  g_as3_object_hook &&
                  g_as3_object_hook->installed());
    set_prop_bool(obj, "renderDiagnosticsReady",
                  s.render_enabled &&
                  g_render_hook &&
                  g_render_hook->installed() &&
                  g_render_hook->patchedSlots() > 0);
    if (g_render_hook) {
        set_prop_u32(obj, "renderHookInstalled",
                     g_render_hook->installed() ? 1u : 0u);
        set_prop_f64(obj, "renderHookFailedInstalls",
                     static_cast<double>(g_render_hook->failedInstalls()));
        set_prop_u32(obj, "renderHookLastFailureStage",
                     g_render_hook->lastFailureStage());
        set_prop_f64(obj, "renderHookPatchedSlots",
                     static_cast<double>(g_render_hook->patchedSlots()));
        set_prop_f64(obj, "renderHookVtablePatches",
                     static_cast<double>(g_render_hook->hookInstalls()));
        set_prop_f64(obj, "renderHookDevicePatches",
                     static_cast<double>(g_render_hook->deviceHookInstalls()));
        set_prop_f64(obj, "renderHookTexturePatches",
                     static_cast<double>(g_render_hook->textureHookInstalls()));
        set_prop_f64(obj, "renderHookFrames",
                     static_cast<double>(g_render_hook->renderFrames()));
        set_prop_f64(obj, "renderHookPresentCalls",
                     static_cast<double>(g_render_hook->presentCalls()));
        set_prop_f64(obj, "renderHookDrawCalls",
                     static_cast<double>(g_render_hook->drawCalls()));
        set_prop_f64(obj, "renderHookPrimitiveCount",
                     static_cast<double>(g_render_hook->primitiveCount()));
        set_prop_f64(obj, "renderHookTextureCreates",
                     static_cast<double>(g_render_hook->textureCreates()));
        set_prop_f64(obj, "renderHookTextureUpdates",
                     static_cast<double>(g_render_hook->textureUpdates()));
        set_prop_f64(obj, "renderHookTextureUploadBytes",
                     static_cast<double>(g_render_hook->textureUploadBytes()));
        set_prop_f64(obj, "renderHookTextureCreateBytes",
                     static_cast<double>(g_render_hook->textureCreateBytes()));
    }
    if (g_memory_hook) {
        set_prop_f64(obj, "memoryHookAllocCalls",
                     static_cast<double>(g_memory_hook->allocCalls()));
        set_prop_f64(obj, "memoryHookAllocLockedCalls",
                     static_cast<double>(g_memory_hook->allocLockedCalls()));
        set_prop_f64(obj, "memoryHookHeapAllocCalls",
                     static_cast<double>(g_memory_hook->heapAllocCalls()));
        set_prop_f64(obj, "memoryHookFreeCalls",
                     static_cast<double>(g_memory_hook->freeCalls()));
        set_prop_f64(obj, "memoryHookHeapFreeCalls",
                     static_cast<double>(g_memory_hook->heapFreeCalls()));
        set_prop_f64(obj, "memoryHookHeapReallocCalls",
                     static_cast<double>(g_memory_hook->heapReallocCalls()));
        set_prop_f64(obj, "memoryHookFailedInstalls",
                     static_cast<double>(g_memory_hook->failedInstalls()));
        set_prop_u32(obj, "memoryHookLastFailureStage",
                     g_memory_hook->lastFailureStage());
    }
    if (g_as3_object_hook) {
        set_prop_f64(obj, "as3ObjectAllocCalls",
                     static_cast<double>(g_as3_object_hook->as3AllocCalls()));
        set_prop_f64(obj, "as3ObjectFreeCalls",
                     static_cast<double>(g_as3_object_hook->as3FreeCalls()));
        set_prop_f64(obj, "as3GenericAllocCalls",
                     static_cast<double>(g_as3_object_hook->genericAllocCalls()));
        set_prop_f64(obj, "as3ObjectHookFailedInstalls",
                     static_cast<double>(g_as3_object_hook->failedInstalls()));
        set_prop_f64(obj, "as3ObjectHookChainedInstalls",
                     static_cast<double>(g_as3_object_hook->chainedInstalls()));
        set_prop_f64(obj, "as3ObjectHookDirectSlotInstalls",
                     static_cast<double>(g_as3_object_hook->directSlotInstalls()));
        set_prop_f64(obj, "as3ObjectHookDirectSlotFailures",
                     static_cast<double>(g_as3_object_hook->directSlotFailures()));
        set_prop_bool(obj, "as3ObjectHookChainedSampler",
                      g_as3_object_hook->chainedSampler());
        set_prop_f64(obj, "as3ObjectHookForwardedCalls",
                     static_cast<double>(g_as3_object_hook->forwardedCalls()));
        set_prop_f64(obj, "as3ObjectHookForwardFailures",
                     static_cast<double>(g_as3_object_hook->forwardFailures()));
        set_prop_f64(obj, "as3ObjectStackCacheHits",
                     static_cast<double>(g_as3_object_hook->stackCacheHits()));
        set_prop_f64(obj, "as3ObjectStackCacheMisses",
                     static_cast<double>(g_as3_object_hook->stackCacheMisses()));
        set_prop_f64(obj, "as3ObjectStackUnavailableCalls",
                     static_cast<double>(g_as3_object_hook->stackUnavailableCalls()));
        set_prop_f64(obj, "as3ObjectStackNativeFallbackCalls",
                     static_cast<double>(g_as3_object_hook->stackNativeFallbackCalls()));
        set_prop_f64(obj, "as3RealEdgeHookInstalls",
                     static_cast<double>(g_as3_object_hook->realEdgeHookInstalls()));
        set_prop_f64(obj, "as3RealEdgeHookFailures",
                     static_cast<double>(g_as3_object_hook->realEdgeHookFailures()));
        set_prop_f64(obj, "as3RealDisplayChildEdges",
                     static_cast<double>(g_as3_object_hook->realDisplayChildEdges()));
        set_prop_f64(obj, "as3RealDisplayChildRemoves",
                     static_cast<double>(g_as3_object_hook->realDisplayChildRemoves()));
        set_prop_f64(obj, "as3RealEventListenerEdges",
                     static_cast<double>(g_as3_object_hook->realEventListenerEdges()));
        set_prop_f64(obj, "as3RealEventListenerRemoves",
                     static_cast<double>(g_as3_object_hook->realEventListenerRemoves()));
        set_prop_u32(obj, "as3RealEdgeLastFailureStage",
                     g_as3_object_hook->realEdgeLastFailureStage());
        set_prop_u32(obj, "as3ObjectHookLastFailureStage",
                     g_as3_object_hook->lastFailureStage());

        const auto current_sampler = g_as3_object_hook->currentSamplerPtr();
        const auto current_vtable = g_as3_object_hook->currentSamplerVtable();
        const auto sampler_slot = g_as3_object_hook->samplerSlotPtr();
        const auto previous_sampler = g_as3_object_hook->previousSamplerPtr();
        const auto previous_vtable = g_as3_object_hook->previousSamplerVtable();
        const std::string current_sampler_hex = hex_ptr(current_sampler);
        const std::string current_vtable_hex = hex_ptr(current_vtable);
        const std::string sampler_slot_hex = hex_ptr(sampler_slot);
        const std::string previous_sampler_hex = hex_ptr(previous_sampler);
        const std::string previous_vtable_hex = hex_ptr(previous_vtable);
        const std::string current_module = g_as3_object_hook->currentSamplerModule();
        const std::string current_vtable_module = g_as3_object_hook->currentSamplerVtableModule();
        const std::string previous_module = g_as3_object_hook->previousSamplerModule();
        const std::string previous_vtable_module = g_as3_object_hook->previousSamplerVtableModule();
        const std::string previous_vtable_head = g_as3_object_hook->previousSamplerVtableHead();

        set_prop_f64(obj, "as3SamplerCurrentPtr", static_cast<double>(current_sampler));
        set_prop_string(obj, "as3SamplerCurrentPtrHex", current_sampler_hex.c_str());
        set_prop_f64(obj, "as3SamplerCurrentVtable", static_cast<double>(current_vtable));
        set_prop_string(obj, "as3SamplerCurrentVtableHex", current_vtable_hex.c_str());
        set_prop_f64(obj, "as3SamplerSlotPtr", static_cast<double>(sampler_slot));
        set_prop_string(obj, "as3SamplerSlotPtrHex", sampler_slot_hex.c_str());
        set_prop_string(obj, "as3SamplerCurrentModule", current_module.c_str());
        set_prop_string(obj, "as3SamplerCurrentVtableModule", current_vtable_module.c_str());
        set_prop_f64(obj, "as3SamplerPreviousPtr", static_cast<double>(previous_sampler));
        set_prop_string(obj, "as3SamplerPreviousPtrHex", previous_sampler_hex.c_str());
        set_prop_f64(obj, "as3SamplerPreviousVtable", static_cast<double>(previous_vtable));
        set_prop_string(obj, "as3SamplerPreviousVtableHex", previous_vtable_hex.c_str());
        set_prop_string(obj, "as3SamplerPreviousModule", previous_module.c_str());
        set_prop_string(obj, "as3SamplerPreviousVtableModule", previous_vtable_module.c_str());
        set_prop_string(obj, "as3SamplerPreviousVtableHead", previous_vtable_head.c_str());
    }
    WindowsAirRuntime* runtime = ensure_air_runtime();
    const bool native_gc_available = runtime != nullptr && runtime->initialized();
    set_prop_bool(obj, "nativeGcAvailable", native_gc_available);
    set_prop_u32(obj, "nativeGcLastFailure",
                 runtime ? static_cast<std::uint32_t>(runtime->nativeGcLastError()) : 0u);
    set_prop_u32(obj, "nativeGcRequestCount",
                 runtime ? runtime->nativeGcRequestCount() : 0u);
    set_prop_bool(obj, "nativeGcPending",
                  runtime != nullptr && runtime->nativeGcPending());
    set_prop_f64(obj, "nativeGcPtr",
                 runtime ? static_cast<double>(runtime->nativeGcPtr()) : 0.0);
    set_prop_u32(obj, "modeBAvailable", 0);
    set_prop_u32(obj, "modeBActive", 0);
    set_prop_u32(obj, "weOwnTransport", 0);
    return obj;
}

FREObject profiler_probe_enter(FREContext, void*, std::uint32_t argc, FREObject* argv) {
    if (argc < 1) return make_bool(false);
    std::uint32_t method_id = 0;
    if (!read_u32(argv[0], method_id)) return make_bool(false);
    std::lock_guard<std::mutex> g(g_mu);
    return make_bool(g_ctrl != nullptr && g_ctrl->method_enter(method_id));
}

FREObject profiler_probe_exit(FREContext, void*, std::uint32_t argc, FREObject* argv) {
    if (argc < 1) return make_bool(false);
    std::uint32_t method_id = 0;
    if (!read_u32(argv[0], method_id)) return make_bool(false);
    std::lock_guard<std::mutex> g(g_mu);
    return make_bool(g_ctrl != nullptr && g_ctrl->method_exit(method_id));
}

FREObject profiler_register_method_table(FREContext, void*, std::uint32_t argc, FREObject* argv) {
    if (argc < 1 || argv[0] == nullptr) return make_bool(false);
    FREByteArray ba{};
    if (FREAcquireByteArray(argv[0], &ba) != FRE_OK) return make_bool(false);
    bool ok = false;
    {
        std::lock_guard<std::mutex> g(g_mu);
        ok = g_ctrl != nullptr && g_ctrl->register_method_table(ba.bytes, ba.length);
    }
    FREReleaseByteArray(argv[0]);
    return make_bool(ok);
}

FREObject profiler_record_alloc(FREContext, void*, std::uint32_t argc, FREObject* argv) {
    if (argc < 2) return make_bool(false);
    std::uint64_t ptr = 0;
    std::uint64_t size = 0;
    if (!read_u64_number(argv[0], ptr) || !read_u64_number(argv[1], size)) return make_bool(false);
    std::lock_guard<std::mutex> g(g_mu);
    return make_bool(g_ctrl != nullptr &&
                     g_ctrl->record_alloc(reinterpret_cast<void*>(static_cast<std::uintptr_t>(ptr)), size));
}

FREObject profiler_record_free(FREContext, void*, std::uint32_t argc, FREObject* argv) {
    if (argc < 1) return make_bool(false);
    std::uint64_t ptr = 0;
    if (!read_u64_number(argv[0], ptr)) return make_bool(false);
    std::lock_guard<std::mutex> g(g_mu);
    return make_bool(g_ctrl != nullptr &&
                     g_ctrl->record_free(reinterpret_cast<void*>(static_cast<std::uintptr_t>(ptr))));
}

FREObject profiler_record_realloc(FREContext, void*, std::uint32_t argc, FREObject* argv) {
    if (argc < 3) return make_bool(false);
    std::uint64_t old_ptr = 0;
    std::uint64_t new_ptr = 0;
    std::uint64_t new_size = 0;
    if (!read_u64_number(argv[0], old_ptr) ||
        !read_u64_number(argv[1], new_ptr) ||
        !read_u64_number(argv[2], new_size)) {
        return make_bool(false);
    }
    std::lock_guard<std::mutex> g(g_mu);
    return make_bool(g_ctrl != nullptr &&
                     g_ctrl->record_realloc(
                         reinterpret_cast<void*>(static_cast<std::uintptr_t>(old_ptr)),
                         reinterpret_cast<void*>(static_cast<std::uintptr_t>(new_ptr)),
                         new_size));
}

FREObject profiler_record_frame(FREContext, void*, std::uint32_t argc, FREObject* argv) {
    if (argc < 2) return make_bool(false);
    std::uint64_t frame_index = 0;
    std::uint64_t duration_ns = 0;
    std::uint32_t allocation_count = 0;
    std::uint64_t allocation_bytes = 0;
    std::string label;
    if (!read_u64_number(argv[0], frame_index) || !read_u64_number(argv[1], duration_ns)) {
        return make_bool(false);
    }
    if (argc >= 3 && argv[2] != nullptr) read_u32(argv[2], allocation_count);
    if (argc >= 4 && argv[3] != nullptr) read_u64_number(argv[3], allocation_bytes);
    if (argc >= 5 && argv[4] != nullptr) read_string(argv[4], label);
    std::lock_guard<std::mutex> g(g_mu);
    return make_bool(g_ctrl != nullptr &&
                     g_ctrl->record_frame(frame_index,
                                          duration_ns,
                                          allocation_count,
                                          allocation_bytes,
                                          label));
}

void register_all(FRENamedFunction* out_functions, int capacity, int* cursor) {
    if (out_functions == nullptr || cursor == nullptr) return;
    struct Entry { const char* name; FREFunction fn; };
    const Entry entries[] = {
        { "awesomeUtils_profilerStart",               &profiler_start },
        { "awesomeUtils_profilerStop",                &profiler_stop },
        { "awesomeUtils_profilerGetStatus",           &profiler_get_status },
        { "awesomeUtils_profilerSnapshot",            &profiler_snapshot },
        { "awesomeUtils_profilerMarker",              &profiler_marker },
        { "awesomeUtils_profilerRequestGc",           &profiler_request_gc },
        { "awesomeUtils_profilerProbeEnter",          &profiler_probe_enter },
        { "awesomeUtils_profilerProbeExit",           &profiler_probe_exit },
        { "awesomeUtils_profilerRegisterMethodTable", &profiler_register_method_table },
        { "awesomeUtils_profilerRecordAlloc",         &profiler_record_alloc },
        { "awesomeUtils_profilerRecordFree",          &profiler_record_free },
        { "awesomeUtils_profilerRecordRealloc",       &profiler_record_realloc },
        { "awesomeUtils_profilerRecordFrame",         &profiler_record_frame },
    };
    for (const auto& e : entries) {
        if (*cursor >= capacity) return;
        out_functions[*cursor].name = reinterpret_cast<const std::uint8_t*>(e.name);
        out_functions[*cursor].function = e.fn;
        out_functions[*cursor].functionData = nullptr;
        ++(*cursor);
    }
}

void shutdown() {
    std::lock_guard<std::mutex> g(g_mu);
    if (g_render_hook) g_render_hook->uninstall();
    if (g_as3_object_hook) g_as3_object_hook->uninstall();
    if (g_memory_hook) g_memory_hook->uninstall();
    if (g_ctrl) g_ctrl->stop();
    g_ctrl.reset();
    g_disk.reset();
    g_memory_hook.reset();
    g_as3_object_hook.reset();
    g_render_hook.reset();
    g_air_runtime.reset();
}

} // namespace ane::profiler::bindings
