// JNI bridge — exposes shared/profiler/{CaptureController,DeepProfilerController}
// + AndroidRuntimeHook to Java. Java side (`Profiler.java`) wraps this;
// FREFunctions in AneAwesomeUtilsContext route AS3 calls to it.
//
// Two operating modes:
//
//   1) Legacy Scout-tap mode (CaptureController + AndroidRuntimeHook):
//      - JNI: nativeStart / nativeStop / nativeGetStatus
//      - Output: `.flmc` (Scout TCP byte tap, parser-compatible w/ Windows .flmc)
//      - Active when only AS3 telemetry capture is needed
//
//   2) Deep .aneprof mode (DeepProfilerController + alloc_tracer wiring):
//      - JNI: nativeStartDeep / nativeStopDeep / nativeSnapshot / nativeMarker /
//             nativeRequestGc / nativeGetStatusDeep
//      - Output: `.aneprof` (timing + native memory events + snapshots + markers)
//      - Phase 1 implemented: native alloc/free events via alloc_tracer hooks
//      - Phase 2 (this file): controller lifecycle + JNI surface
//      - Phase 3 (TBD): AS3 method-frame walker (port from Windows AvmCore reader)
//      - Phase 4 (TBD): IMemorySampler hook for AS3 alloc/free with ref graph
//
// The two modes are mutually exclusive at runtime — only one of g_capture_ctrl
// vs g_deep_ctrl is active per session. Both share the global mutex.
//
// Threading: each JNI call holds g_mu. Hot paths (alloc_tracer hook proxies,
// Scout send hook) call into the controller without holding g_mu — controllers
// are internally synchronized.

#include <jni.h>
#include <android/log.h>
#include "AndroidRuntimeHook.hpp"
#include "AndroidRenderHook.hpp"
#include "AndroidDeepMemoryHook.hpp"
#include "AndroidGcHook.hpp"
#include "AndroidSamplerHook.hpp"
#include "AndroidAs3SamplerHook.hpp"
#include "AndroidAs3RefGraphHook.hpp"
#include "AndroidExperimentHook.hpp"
#include "AndroidAs3ObjectHook.hpp"
#include "CaptureController.hpp"
#include "DeepProfilerController.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

#define LOG_TAG "AneProfilerBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Forward-decl to alloc_tracer wiring (defined in alloc_tracer.cpp). When deep
// profiler is recording with memory enabled, native alloc/free events captured
// by alloc_tracer's shadowhook proxies are mirrored into the .aneprof stream.
namespace ane::alloc_tracer {
    void setDeepProfilerController(ane::profiler::DeepProfilerController* dpc);
    ane::profiler::DeepProfilerController* getDeepProfilerController();
    bool isActive();
}

namespace {

std::mutex                                          g_mu;

// Legacy Scout-tap mode
std::unique_ptr<ane::profiler::CaptureController>   g_capture_ctrl;
std::unique_ptr<ane::profiler::AndroidRuntimeHook>  g_hook;

// Deep .aneprof mode
std::unique_ptr<ane::profiler::DeepProfilerController> g_deep_ctrl;
std::unique_ptr<ane::profiler::AndroidRenderHook>      g_render_hook;
std::unique_ptr<ane::profiler::AndroidDeepMemoryHook>  g_deep_mem_hook;
std::unique_ptr<ane::profiler::AndroidGcHook>          g_gc_hook;
std::unique_ptr<ane::profiler::AndroidAs3ObjectHook>   g_as3_object_hook;
std::unique_ptr<ane::profiler::AndroidSamplerHook>     g_sampler_hook;  // Phase 4a RA
std::unique_ptr<ane::profiler::AndroidAs3SamplerHook>  g_as3_sampler_hook;  // Phase 4a productive
std::unique_ptr<ane::profiler::AndroidAs3RefGraphHook> g_as3_refgraph_hook; // Phase 4b

// Helper: fetch a Java String into a std::string. Returns empty on failure.
std::string jstrToCpp(JNIEnv* env, jstring j) {
    if (!j) return {};
    const char* c = env->GetStringUTFChars(j, nullptr);
    if (!c) return {};
    std::string out(c);
    env->ReleaseStringUTFChars(j, c);
    return out;
}

const char* deepStateStr(ane::profiler::DeepProfilerController::State s) {
    using S = ane::profiler::DeepProfilerController::State;
    switch (s) {
        case S::Idle:      return "Idle";
        case S::Starting:  return "Starting";
        case S::Recording: return "Recording";
        case S::Stopping:  return "Stopping";
        case S::Error:     return "Error";
    }
    return "?";
}

} // namespace

extern "C" {

// ============================================================================
// Mode 1: Legacy Scout-tap (.flmc)
// ============================================================================

JNIEXPORT jint JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeStart(
        JNIEnv* env, jclass,
        jstring jOutputPath,
        jstring jHeaderJson,
        jint    jTelemetryPort) {
    std::lock_guard<std::mutex> lk(g_mu);

    if (g_deep_ctrl) {
        LOGE("nativeStart: deep profiler is active — stop it first via nativeStopDeep");
        return -10;
    }

    std::string output_path = jstrToCpp(env, jOutputPath);
    std::string header_json = jstrToCpp(env, jHeaderJson);
    if (output_path.empty()) {
        LOGE("nativeStart: empty output_path");
        return -1;
    }

    if (!g_capture_ctrl) g_capture_ctrl = std::make_unique<ane::profiler::CaptureController>();
    if (!g_hook)         g_hook         = std::make_unique<ane::profiler::AndroidRuntimeHook>();

    if (g_capture_ctrl->state() != ane::profiler::CaptureController::State::Idle) {
        LOGE("nativeStart: controller not Idle (state=%d)",
             static_cast<int>(g_capture_ctrl->state()));
        return -2;
    }

    ane::profiler::CaptureController::Config cfg;
    cfg.output_path = output_path;
    cfg.header_json = header_json;

    if (!g_capture_ctrl->start(cfg)) {
        LOGE("nativeStart: controller.start failed");
        return -3;
    }

    if (!g_hook->install(g_capture_ctrl.get(),
                         static_cast<std::uint16_t>(jTelemetryPort))) {
        LOGE("nativeStart: hook.install failed");
        g_capture_ctrl->stop();
        return -4;
    }
    LOGI("nativeStart: legacy/.flmc mode ok output=%s port=%d",
         output_path.c_str(), (int)jTelemetryPort);
    return 1;
}

JNIEXPORT jint JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeStop(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_capture_ctrl) return 0;
    if (g_hook) g_hook->uninstall();
    g_capture_ctrl->stop();
    LOGI("nativeStop: ok");
    return 1;
}

JNIEXPORT jstring JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeGetStatus(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    char buf[1024];
    if (!g_capture_ctrl) {
        snprintf(buf, sizeof(buf),
                 "{\"state\":\"NotInitialized\",\"installed\":false}");
        return env->NewStringUTF(buf);
    }
    auto status = g_capture_ctrl->status();
    auto state_str = [&]() -> const char* {
        using S = ane::profiler::CaptureController::State;
        switch (status.state) {
            case S::Idle:      return "Idle";
            case S::Starting:  return "Starting";
            case S::Recording: return "Recording";
            case S::Stopping:  return "Stopping";
            case S::Error:     return "Error";
        }
        return "?";
    }();
    bool installed = g_hook && g_hook->installed();
    std::uint64_t sendCalls    = g_hook ? g_hook->diagSendCalls()    : 0;
    std::uint64_t sendCaptured = g_hook ? g_hook->diagSendCaptured() : 0;
    std::uint64_t connectCalls = g_hook ? g_hook->diagConnectCalls() : 0;
    std::uint64_t connectMatched = g_hook ? g_hook->diagConnectMatched() : 0;
    std::uint64_t closeCalls   = g_hook ? g_hook->diagCloseCalls()   : 0;

    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"installed\":%s,"
             "\"bytesIn\":%llu,\"bytesAccepted\":%llu,\"bytesOut\":%llu,"
             "\"recordCount\":%llu,\"dropCount\":%llu,\"dropBytes\":%llu,"
             "\"elapsedMs\":%llu,"
             "\"diagSendCalls\":%llu,\"diagSendCaptured\":%llu,"
             "\"diagConnectCalls\":%llu,\"diagConnectMatched\":%llu,"
             "\"diagCloseCalls\":%llu}",
             state_str, installed ? "true" : "false",
             (unsigned long long)status.bytes_in,
             (unsigned long long)status.bytes_accepted,
             (unsigned long long)status.bytes_out,
             (unsigned long long)status.record_count,
             (unsigned long long)status.drop_count,
             (unsigned long long)status.drop_bytes,
             (unsigned long long)status.elapsed_ms,
             (unsigned long long)sendCalls,
             (unsigned long long)sendCaptured,
             (unsigned long long)connectCalls,
             (unsigned long long)connectMatched,
             (unsigned long long)closeCalls);
    return env->NewStringUTF(buf);
}

// ============================================================================
// Mode 2: Deep .aneprof
// ============================================================================
//
// Signature mirrors Windows ProfilerAneBindings::profiler_start positional args:
//   jOutputPath        — path to write .aneprof
//   jHeaderJson        — initial header JSON metadata
//   jTiming            — emit MethodEnter/Exit + Frame events (Phase 3 will
//                        gate this on AS3 walker availability)
//   jMemory            — emit Alloc/Free/Realloc events (wired via alloc_tracer
//                        on Android — alloc_tracer.cpp shadowhooks libc and
//                        forwards to the controller when set)
//   jSnapshots         — allow profilerSnapshot() event emission
//   jMaxLive           — bound on per-snapshot live allocation tracking (4096
//                        default; lower if device memory tight)
//   jSnapshotIntervalMs — automatic snapshot cadence; 0 = manual only
//   jAs3Sampling       — Phase 4 (TBD on Android); ignored for now

JNIEXPORT jint JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeStartDeep(
        JNIEnv* env, jclass,
        jstring  jOutputPath,
        jstring  jHeaderJson,
        jboolean jTiming,
        jboolean jMemory,
        jboolean jSnapshots,
        jint     jMaxLive,
        jint     jSnapshotIntervalMs,
        jboolean jAs3Sampling) {
    std::lock_guard<std::mutex> lk(g_mu);

    if (g_capture_ctrl) {
        LOGE("nativeStartDeep: legacy Scout-tap controller is active — stop "
             "it first via nativeStop");
        return -10;
    }

    std::string output_path = jstrToCpp(env, jOutputPath);
    std::string header_json = jstrToCpp(env, jHeaderJson);
    if (output_path.empty()) {
        LOGE("nativeStartDeep: empty output_path");
        return -1;
    }

    if (!g_deep_ctrl) g_deep_ctrl = std::make_unique<ane::profiler::DeepProfilerController>();

    if (g_deep_ctrl->state() != ane::profiler::DeepProfilerController::State::Idle) {
        LOGE("nativeStartDeep: controller not Idle (state=%d)",
             static_cast<int>(g_deep_ctrl->state()));
        return -2;
    }

    ane::profiler::DeepProfilerController::Config cfg;
    cfg.output_path = output_path;
    cfg.header_json = header_json;
    cfg.timing_enabled    = (jTiming    != JNI_FALSE);
    cfg.memory_enabled    = (jMemory    != JNI_FALSE);
    cfg.render_enabled    = (jTiming != JNI_FALSE);  // Phase 6: EGL/GLES hook (gated by timing flag)
    cfg.snapshots_enabled = (jSnapshots != JNI_FALSE);
    cfg.max_live_allocations_per_snapshot =
        jMaxLive > 0 ? static_cast<std::uint32_t>(jMaxLive) : 4096;
    cfg.snapshot_interval_ms =
        jSnapshotIntervalMs > 0 ? static_cast<std::uint32_t>(jSnapshotIntervalMs) : 0;

    if (!g_deep_ctrl->start(cfg)) {
        LOGE("nativeStartDeep: controller.start failed");
        return -3;
    }

    // Wire alloc_tracer → DeepProfilerController so libc malloc/free/mmap
    // events captured by shadowhook proxies (filtered to allocations whose
    // CALLER is inside libCore.so) flow into the .aneprof stream.
    //
    // Note: the user must also call AllocTracer.start() separately to install
    // the shadowhook proxies. This bridge wiring just sets the destination
    // controller for events that ARE captured.
    if (cfg.memory_enabled) {
        ane::alloc_tracer::setDeepProfilerController(g_deep_ctrl.get());
        LOGI("nativeStartDeep: alloc_tracer wired to deep controller "
             "(call AllocTracer.start() separately to enable hooks)");
    }

    // Phase 6: install EGL/GLES render hook if timing/render is enabled. Hook
    // is global (one render thread per app); shared with the deep controller
    // so RenderFrame events flow into the .aneprof stream.
    if (cfg.render_enabled) {
        g_render_hook = std::make_unique<ane::profiler::AndroidRenderHook>();
        if (!g_render_hook->install(g_deep_ctrl.get())) {
            LOGW("nativeStartDeep: render hook install failed — continuing without RenderFrame events");
            g_render_hook.reset();
        } else {
            LOGI("nativeStartDeep: render hook installed (eglSwapBuffers + glDraw*)");
        }
    }

    // Phase 5: install deep memory hook on libCore.so:GCHeap::Alloc when memory
    // tracking is enabled. Build-id-pinned offset; refuses to install on
    // unknown SDK builds (prevents wild patches). Coexists with alloc_tracer's
    // libc shadowhook — DPC dedupes by ptr.
    if (cfg.memory_enabled) {
        g_deep_mem_hook = std::make_unique<ane::profiler::AndroidDeepMemoryHook>();
        if (!g_deep_mem_hook->install(g_deep_ctrl.get())) {
            LOGW("nativeStartDeep: deep memory hook install failed (unknown build-id "
                 "or libCore.so unloaded) — continuing with alloc_tracer libc shadowhook only");
            g_deep_mem_hook.reset();
        } else {
            LOGI("nativeStartDeep: deep memory hook installed (libCore.so GCHeap::Alloc)");
        }
    }

    // Phase 7a: install GC observer hook on libCore.so:MMgc::GC::Collect.
    // Emits one GcCycle event per Collect() call so the analyzer can correlate
    // alloc/free deltas around GC cycles. Build-id-pinned, no-op on unknown.
    if (cfg.memory_enabled) {
        g_gc_hook = std::make_unique<ane::profiler::AndroidGcHook>();
        if (!g_gc_hook->install(g_deep_ctrl.get())) {
            LOGW("nativeStartDeep: GC observer hook install failed — GcCycle events disabled");
            g_gc_hook.reset();
        } else {
            LOGI("nativeStartDeep: GC observer hook installed (libCore.so GC::Collect)");
        }
    }

    // Phase 4c: install typed-AS3-alloc resolver. Walks VTable→Traits→name
    // chain at ~50us after each FixedMalloc::Alloc, emits as3_alloc markers
    // with class name. Coexists with the deep memory hook (which provides the
    // pending-alloc queue source).
    if (cfg.memory_enabled && g_deep_mem_hook) {
        g_as3_object_hook = std::make_unique<ane::profiler::AndroidAs3ObjectHook>();
        if (!g_as3_object_hook->install(g_deep_ctrl.get())) {
            LOGW("nativeStartDeep: AS3 typed-alloc hook install failed");
            g_as3_object_hook.reset();
        } else {
            g_deep_mem_hook->setAs3ObjectHook(g_as3_object_hook.get());
            LOGI("nativeStartDeep: AS3 typed-alloc resolver installed (Phase 4c)");
        }
    }

    // Phase 4b: install AS3 reference-graph hook. Currently partial —
    // only EventDispatcher::addEventListener (HIGH-confidence offset).
    // addChild/removeChild/removeEventListener pending RA finalization.
    if (cfg.memory_enabled) {
        g_as3_refgraph_hook = std::make_unique<ane::profiler::AndroidAs3RefGraphHook>();
        if (!g_as3_refgraph_hook->install(g_deep_ctrl.get())) {
            LOGW("nativeStartDeep: AS3 ref graph hook install failed (build-id "
                 "not in known list, or hook collision)");
            g_as3_refgraph_hook.reset();
        } else {
            LOGI("nativeStartDeep: AS3 ref graph hook installed (Phase 4b partial)");
        }
    }

    if (jAs3Sampling != JNI_FALSE) {
        LOGW("nativeStartDeep: as3Sampling requested but Android AS3 method "
             "walker / IMemorySampler hook is not yet implemented (Phase 3+4 "
             "TBD). Continuing without AS3 stack annotation.");
    }

    LOGI("nativeStartDeep: ok output=%s timing=%d memory=%d snapshots=%d "
         "maxLive=%d intervalMs=%d",
         output_path.c_str(),
         (int)cfg.timing_enabled, (int)cfg.memory_enabled,
         (int)cfg.snapshots_enabled,
         (int)cfg.max_live_allocations_per_snapshot,
         (int)cfg.snapshot_interval_ms);
    return 1;
}

JNIEXPORT jint JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeStopDeep(
        JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_deep_ctrl) return 0;
    // Unwire alloc_tracer first so any in-flight events stop trying to call
    // record_alloc/free on a stopped controller.
    ane::alloc_tracer::setDeepProfilerController(nullptr);
    // Uninstall render hook before stopping the controller (proxies hold a
    // weak ref; uninstall() clears that and the swap proxy stops emitting).
    if (g_render_hook) {
        g_render_hook->uninstall();
        g_render_hook.reset();
    }
    // Uninstall Phase 4c AS3 typed-alloc hook BEFORE the deep memory hook
    // (the deep memory hook proxy holds a pointer to the AS3 hook). Order
    // matters: clear g_as3_hook on the deep memory hook side first, then
    // uninstall the AS3 hook itself (which joins its drain thread).
    if (g_deep_mem_hook && g_as3_object_hook) {
        g_deep_mem_hook->setAs3ObjectHook(nullptr);
    }
    if (g_as3_object_hook) {
        g_as3_object_hook->uninstall();
        g_as3_object_hook.reset();
    }
    // Uninstall Phase 5 deep memory hook before stopping the controller.
    if (g_deep_mem_hook) {
        g_deep_mem_hook->uninstall();
        g_deep_mem_hook.reset();
    }
    // Phase 4b — uninstall ref-graph hook before deep_ctrl goes away (the
    // proxy holds a g_controller reference; tear down before the controller
    // is destroyed).
    if (g_as3_refgraph_hook) {
        g_as3_refgraph_hook->uninstall();
        g_as3_refgraph_hook.reset();
    }
    // Phase 4a productive — uninstall recordAllocationSample hook FIRST.
    if (g_as3_sampler_hook) {
        g_as3_sampler_hook->uninstall();
        g_as3_sampler_hook.reset();
    }
    // Phase 4a RA — uninstall sampler diagnostic hook FIRST (it shadowhooks
    // sampler vftable slots; if those funcs are called after we tear down
    // the proxies' counters, we'd UAF).
    if (g_sampler_hook) {
        g_sampler_hook->uninstall();
        g_sampler_hook.reset();
    }
    // Uninstall Phase 7a GC observer hook before stopping the controller.
    if (g_gc_hook) {
        g_gc_hook->uninstall();
        g_gc_hook.reset();
    }
    bool ok = g_deep_ctrl->stop();
    LOGI("nativeStopDeep: stopped (rc=%d)", ok ? 1 : 0);
    return ok ? 1 : 0;
}

JNIEXPORT jboolean JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeSnapshot(
        JNIEnv* env, jclass, jstring jLabel) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_deep_ctrl) return JNI_FALSE;
    std::string label = jLabel ? jstrToCpp(env, jLabel) : std::string{};
    return g_deep_ctrl->snapshot(label) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeMarker(
        JNIEnv* env, jclass, jstring jName, jstring jValueJson) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_deep_ctrl) return JNI_FALSE;
    std::string name = jName ? jstrToCpp(env, jName) : std::string{};
    std::string value_json = jValueJson ? jstrToCpp(env, jValueJson) : std::string{};
    return g_deep_ctrl->marker(name, value_json) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeGetStatusDeep(
        JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    char buf[1536];
    if (!g_deep_ctrl) {
        snprintf(buf, sizeof(buf),
                 "{\"state\":\"NotInitialized\",\"installed\":false}");
        return env->NewStringUTF(buf);
    }
    auto status = g_deep_ctrl->status();
    bool tracer_active = ane::alloc_tracer::isActive();
    bool tracer_wired  = (ane::alloc_tracer::getDeepProfilerController() == g_deep_ctrl.get());

    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\","
             "\"events\":%llu,\"dropped\":%llu,\"payloadBytes\":%llu,"
             "\"elapsedMs\":%llu,"
             "\"liveAllocations\":%llu,\"liveBytes\":%llu,"
             "\"totalAllocations\":%llu,\"totalFrees\":%llu,"
             "\"totalReallocations\":%llu,\"unknownFrees\":%llu,"
             "\"writerQueueDepth\":%llu,\"writerQueueCapacity\":%llu,"
             "\"writerEventsWritten\":%llu,\"writerBytesWritten\":%llu,"
             "\"writerOverflowDepth\":%llu,\"writerOverflowPeak\":%llu,"
             "\"writerOverflowEvents\":%llu,"
             "\"timingEnabled\":%s,\"memoryEnabled\":%s,"
             "\"snapshotsEnabled\":%s,"
             "\"allocTracerActive\":%s,\"allocTracerWired\":%s}",
             deepStateStr(status.state),
             (unsigned long long)status.events,
             (unsigned long long)status.dropped,
             (unsigned long long)status.payload_bytes,
             (unsigned long long)status.elapsed_ms,
             (unsigned long long)status.live_allocations,
             (unsigned long long)status.live_bytes,
             (unsigned long long)status.total_allocations,
             (unsigned long long)status.total_frees,
             (unsigned long long)status.total_reallocations,
             (unsigned long long)status.unknown_frees,
             (unsigned long long)status.writer_queue_depth,
             (unsigned long long)status.writer_queue_capacity,
             (unsigned long long)status.writer_events_written,
             (unsigned long long)status.writer_bytes_written,
             (unsigned long long)status.writer_overflow_depth,
             (unsigned long long)status.writer_overflow_peak,
             (unsigned long long)status.writer_overflow_events,
             status.timing_enabled    ? "true" : "false",
             status.memory_enabled    ? "true" : "false",
             status.snapshots_enabled ? "true" : "false",
             tracer_active ? "true" : "false",
             tracer_wired  ? "true" : "false");
    return env->NewStringUTF(buf);
}

// ----- Phase 3+4 compiler-injected method probes -----
//
// These three FREFunctions mirror the Windows ProfilerAneBindings probe API
// (`awesomeUtils_profilerProbeEnter/Exit/RegisterMethodTable`). Activated by
// compiling AS3 with `--profile-probes`: the compiler injects calls to
// `awesomeUtils_profilerProbeEnter(method_id)` at every method entry and
// `awesomeUtils_profilerProbeExit(method_id)` at every exit. DPC maintains a
// per-thread stack of method IDs; native alloc events tagged via the proxy
// chain inherit `current_method_id()` automatically.
//
// The MethodTable is a packed binary table of (method_id → name) registered
// once at app startup with all known method IDs. Used by `aneprof_analyze.py`
// to render human-readable AS3 method names instead of opaque hashes.

JNIEXPORT jboolean JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeProbeEnter(
        JNIEnv* env, jclass, jint jMethodId) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_deep_ctrl) return JNI_FALSE;
    return g_deep_ctrl->method_enter(static_cast<std::uint32_t>(jMethodId)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeProbeExit(
        JNIEnv* env, jclass, jint jMethodId) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_deep_ctrl) return JNI_FALSE;
    return g_deep_ctrl->method_exit(static_cast<std::uint32_t>(jMethodId)) ? JNI_TRUE : JNI_FALSE;
}

// Phase 7b: AS3-side `Profiler.recordFrame(idx, durationNs, allocCount,
// allocBytes, label)` emits a Frame event into the .aneprof stream. This is
// SEPARATE from Phase 6 RenderFrame events (which are auto-emitted from the
// EGL hook) — Frame events are explicit AS3-side markers for things like
// scene transitions, "battle_start", etc. The analyzer renders both event
// types in the timeline.
JNIEXPORT jboolean JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeRecordFrame(
        JNIEnv* env, jclass,
        jlong jFrameIndex, jlong jDurationNs,
        jint jAllocCount, jlong jAllocBytes,
        jstring jLabel) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_deep_ctrl) return JNI_FALSE;
    std::string label = jLabel ? jstrToCpp(env, jLabel) : std::string{};
    bool ok = g_deep_ctrl->record_frame(
        static_cast<std::uint64_t>(jFrameIndex),
        static_cast<std::uint64_t>(jDurationNs),
        static_cast<std::uint32_t>(jAllocCount < 0 ? 0 : jAllocCount),
        static_cast<std::uint64_t>(jAllocBytes < 0 ? 0 : jAllocBytes),
        label);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeRegisterMethodTable(
        JNIEnv* env, jclass, jbyteArray jBytes) {
    if (jBytes == nullptr) return JNI_FALSE;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_deep_ctrl) return JNI_FALSE;
    jsize len = env->GetArrayLength(jBytes);
    if (len <= 0) return JNI_FALSE;
    jbyte* data = env->GetByteArrayElements(jBytes, nullptr);
    if (data == nullptr) return JNI_FALSE;
    bool ok = g_deep_ctrl->register_method_table(
            reinterpret_cast<const void*>(data), static_cast<std::uint32_t>(len));
    env->ReleaseByteArrayElements(jBytes, data, JNI_ABORT);
    return ok ? JNI_TRUE : JNI_FALSE;
}

// Phase 7a — programmatic GC trigger. Routes through AndroidGcHook which
// invokes the original `MMgc::GC::Collect` with the runtime-captured GC
// singleton. Returns false until the first observed Collect has populated
// the singleton (typically within a few seconds of profiler start).
JNIEXPORT jboolean JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeRequestGc(
        JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_gc_hook) return JNI_FALSE;
    return g_gc_hook->requestCollect() ? JNI_TRUE : JNI_FALSE;
}

// RA helper — dump AvmCore via the GC observer hook (gc+0x10 = AvmCore*).
// Used to take labeled snapshots before/after `flash.sampler.startSampling()`
// so we can diff in logcat and identify the m_sampler offset.
JNIEXPORT jboolean JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeDumpAvmCore(
        JNIEnv* env, jclass, jstring jLabel) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_gc_hook) return JNI_FALSE;
    std::string label = jLabel ? jstrToCpp(env, jLabel) : std::string("?");
    return g_gc_hook->dumpAvmCore(label.c_str()) ? JNI_TRUE : JNI_FALSE;
}

// Phase 4a RA — install diagnostic sampler hook on every non-null vftable
// slot. Requires AndroidGcHook captured the GC singleton (call after
// forceGcViaChurn). Per-slot hit counts logged on uninstall.
JNIEXPORT jboolean JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeSamplerHookInstall(
        JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_sampler_hook) {
        // Already installed — uninstall and reinstall to reset counters
        g_sampler_hook->uninstall();
        g_sampler_hook.reset();
    }
    g_sampler_hook = std::make_unique<ane::profiler::AndroidSamplerHook>();
    bool ok = g_sampler_hook->install(g_deep_ctrl.get());
    if (!ok) {
        g_sampler_hook.reset();
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeSamplerHookUninstall(
        JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_sampler_hook) return JNI_FALSE;
    g_sampler_hook->uninstall();
    g_sampler_hook.reset();
    return JNI_TRUE;
}

// Phase 4a productive — install hook on recordAllocationSample slot.
// Resolves class names via Traits walk and emits as3_alloc_sampler markers.
JNIEXPORT jboolean JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeAs3SamplerInstall(
        JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_as3_sampler_hook) {
        g_as3_sampler_hook->uninstall();
        g_as3_sampler_hook.reset();
    }
    g_as3_sampler_hook = std::make_unique<ane::profiler::AndroidAs3SamplerHook>();
    bool ok = g_as3_sampler_hook->install(g_deep_ctrl.get());
    if (!ok) {
        g_as3_sampler_hook.reset();
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeAs3SamplerUninstall(
        JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_as3_sampler_hook) return JNI_FALSE;
    g_as3_sampler_hook->uninstall();
    g_as3_sampler_hook.reset();
    return JNI_TRUE;
}

// Phase 4b RA tooling — generic experiment hook. AS3 passes any libCore.so
// offset; ANE shadowhooks it at runtime; logs args + hit counts. Useful
// for testing RA candidates without rebuilding the ANE.
JNIEXPORT jint JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeExperimentHookInstall(
        JNIEnv* env, jclass, jlong jOffset, jstring jLabel) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::string label = jLabel ? jstrToCpp(env, jLabel) : std::string("?");
    return ane::profiler::AndroidExperimentHook::install(
        static_cast<std::uintptr_t>(jOffset), label.c_str());
}

JNIEXPORT jlong JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeExperimentHookHits(
        JNIEnv* env, jclass, jlong jOffset) {
    std::lock_guard<std::mutex> lk(g_mu);
    return static_cast<jlong>(
        ane::profiler::AndroidExperimentHook::hitsForOffset(
            static_cast<std::uintptr_t>(jOffset)));
}

JNIEXPORT void JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeExperimentHookUninstallAll(
        JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    ane::profiler::AndroidExperimentHook::uninstallAll();
}

} // extern "C"
