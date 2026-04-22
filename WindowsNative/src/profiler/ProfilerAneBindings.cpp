#include "ProfilerAneBindings.hpp"

#include "CaptureController.hpp"
#include "IDiskMonitor.hpp"
#include "ProfilerFeatures.hpp"
#include "profiler/WindowsAirRuntime.hpp"
#include "profiler/WindowsLoopbackListener.hpp"
#include "profiler/WindowsRuntimeHook.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ane::profiler::bindings {

// ----------- shared singleton state ---------------------------------------
namespace {
std::mutex                          g_mu;
std::unique_ptr<CaptureController>       g_ctrl;
std::unique_ptr<WindowsRuntimeHook>      g_hook;
std::unique_ptr<WindowsAirRuntime>       g_air_runtime;
std::unique_ptr<WindowsLoopbackListener> g_loopback;
std::unique_ptr<IDiskMonitor>            g_disk;
std::vector<std::string>                 g_pending_markers;
bool                                     g_runtime_forced_on = false;
// Post-patch Adobe wires its SocketTransport once at startup and keeps it
// alive for the process lifetime. Tearing down the loopback listener /
// IAT hook between profiler_start cycles would ECONNRESET that transport
// and Adobe would stop sending. So once the pump is up we leave it up —
// recording is toggled purely via CaptureController::start/stop.
bool                                     g_runtime_pump_started = false;
} // namespace

// ----------- small FRE helpers --------------------------------------------

static FREObject make_bool(bool v) {
    FREObject o = nullptr;
    FRENewObjectFromBool(v ? 1u : 0u, &o);
    return o;
}

static FREObject make_u32(std::uint32_t v) {
    FREObject o = nullptr;
    FRENewObjectFromUint32(v, &o);
    return o;
}

static FREObject make_f64(double v) {
    FREObject o = nullptr;
    FRENewObjectFromDouble(v, &o);
    return o;
}

static bool read_string(FREObject o, std::string& out) {
    std::uint32_t len = 0;
    const std::uint8_t* data = nullptr;
    if (FREGetObjectAsUTF8(o, &len, &data) != FRE_OK || data == nullptr) return false;
    out.assign(reinterpret_cast<const char*>(data), len);
    return true;
}

static bool read_u32(FREObject o, std::uint32_t& out) {
    return FREGetObjectAsUint32(o, &out) == FRE_OK;
}

static bool read_i32(FREObject o, std::int32_t& out) {
    return FREGetObjectAsInt32(o, &out) == FRE_OK;
}

static bool read_bool(FREObject o, bool& out) {
    std::uint32_t v = 0;
    if (FREGetObjectAsBool(o, &v) != FRE_OK) return false;
    out = (v != 0);
    return true;
}

static void set_prop_u32(FREObject obj, const char* name, std::uint32_t v) {
    FREObject tmp = make_u32(v);
    FRESetObjectProperty(obj, reinterpret_cast<const std::uint8_t*>(name), tmp, nullptr);
}
static void set_prop_f64(FREObject obj, const char* name, double v) {
    FREObject tmp = make_f64(v);
    FRESetObjectProperty(obj, reinterpret_cast<const std::uint8_t*>(name), tmp, nullptr);
}

// Reads a boolean property from an AS3 object, defaulting if absent.
static bool read_bool_prop(FREObject obj, const char* name, bool dflt) {
    FREObject prop = nullptr;
    if (FREGetObjectProperty(obj, reinterpret_cast<const std::uint8_t*>(name),
                              &prop, nullptr) != FRE_OK || prop == nullptr) {
        return dflt;
    }
    bool v = dflt;
    if (!read_bool(prop, v)) return dflt;
    return v;
}

static std::uint32_t read_u32_prop(FREObject obj, const char* name, std::uint32_t dflt) {
    FREObject prop = nullptr;
    if (FREGetObjectProperty(obj, reinterpret_cast<const std::uint8_t*>(name),
                              &prop, nullptr) != FRE_OK || prop == nullptr) {
        return dflt;
    }
    std::uint32_t v = dflt;
    if (!read_u32(prop, v)) return dflt;
    return v;
}

// ----------- FREFunctions -------------------------------------------------

// profilerStart(outputPath:String,
//               maxBytesMb:uint = 900,
//               compressionLevel:int = 6,
//               headerJson:String = "",
//               features:Object = null,
//               host:String = "127.0.0.1",
//               port:uint = 7934): Boolean
FREObject profiler_start(FREContext ctx, void*, std::uint32_t argc, FREObject* argv) {
    std::lock_guard<std::mutex> g(g_mu);
    if (argc < 1) return make_bool(false);

    std::string output_path;
    if (!read_string(argv[0], output_path) || output_path.empty()) return make_bool(false);

    std::uint32_t max_bytes_mb = 900;
    std::int32_t  level = 6;
    std::string   header_json;
    if (argc >= 2) read_u32(argv[1], max_bytes_mb);
    if (argc >= 3) read_i32(argv[2], level);
    if (argc >= 4) read_string(argv[3], header_json);

    ProfilerFeatures features;
    if (argc >= 5 && argv[4] != nullptr) {
        FREObject f = argv[4];
        features.sampler_enabled                 = read_bool_prop(f, "samplerEnabled",       true);
        features.cpu_capture                     = read_bool_prop(f, "cpuCapture",           true);
        features.display_object_capture          = read_bool_prop(f, "displayObjectCapture", false);
        features.stage3d_capture                 = read_bool_prop(f, "stage3DCapture",       false);
        features.script_object_allocation_traces = read_bool_prop(f, "scriptObjectAllocationTraces", false);
        features.all_gc_allocation_traces        = read_bool_prop(f, "allGcAllocationTraces",        false);
        features.gc_allocation_traces_threshold  = read_u32_prop (f, "gcAllocationTracesThreshold", 1024);
    }

    std::string host = "127.0.0.1";
    std::uint32_t port = 7934;
    if (argc >= 6) read_string(argv[5], host);
    if (argc >= 7) read_u32(argv[6], port);

    if (g_ctrl && g_ctrl->state() != CaptureController::State::Idle) {
        return make_bool(false); // already recording
    }

    if (!g_ctrl)        g_ctrl        = std::make_unique<CaptureController>();
    if (!g_hook)        g_hook        = std::make_unique<WindowsRuntimeHook>();
    if (!g_air_runtime) g_air_runtime = std::make_unique<WindowsAirRuntime>();
    if (!g_loopback)    g_loopback    = std::make_unique<WindowsLoopbackListener>();
    if (!g_disk)        g_disk        = IDiskMonitor::create();

    // The profiler is strictly Mode B: we refuse to start unless we can
    // force the runtime telemetry on ourselves. This guarantees zero idle
    // overhead — without a successful Start, no runtime telemetry ever
    // runs — AND it means a stale .telemetry.cfg on disk can never turn
    // the profiler on silently. `profilerStop` is the only way back to
    // idle; the next `profilerStart` re-allocates and re-wires from scratch.
    if (!g_air_runtime->initialize()) {
        return make_bool(false);
    }
    if (g_air_runtime->tryCapturePlayer(static_cast<void*>(ctx)) == nullptr) {
        return make_bool(false);
    }

    // Disk-space gate.
    if (g_disk) {
        constexpr std::uint64_t kMinFree = 200ull * 1024ull * 1024ull;
        const std::uint64_t free = g_disk->free_bytes(output_path);
        if (free != UINT64_MAX && free < kMinFree) return make_bool(false);
    }

    // Reject non-loopback / zero-port configs — without the internal
    // loopback listener there is no peer for the runtime to connect to,
    // and forceEnable would time out.
    const bool is_loopback = (host == "127.0.0.1" || host == "localhost" || host == "::1");
    if (!is_loopback || port == 0 || port > 65535) {
        return make_bool(false);
    }

    // File + hook + listener all come up first, then we force-enable the
    // runtime. Symmetric teardown on stop or on any failure below.
    CaptureController::Config cfg;
    cfg.output_path       = std::move(output_path);
    cfg.max_bytes_out     = (max_bytes_mb == 0) ? 0ull
                              : static_cast<std::uint64_t>(max_bytes_mb) * 1024ull * 1024ull;
    cfg.compression_level = (level < 1 || level > 9) ? 6 : level;
    cfg.header_json       = header_json.empty()
        ? std::string(R"({"platform":"windows","air_version":"51.1.3.10",)"
                      R"("compression":"zlib","wire_protocol":"scout-amf3"})")
        : std::move(header_json);

    if (!g_ctrl->start(cfg)) return make_bool(false);

    if (!g_runtime_pump_started) {
        // First start in this process: bring up the IAT hook, bind the
        // loopback listener, and force-enable telemetry. These stay up
        // across subsequent start/stop cycles.
        if (!g_hook->install(g_ctrl.get())) {
            g_ctrl->stop();
            return make_bool(false);
        }

        g_loopback->start(static_cast<std::uint16_t>(port));

        g_runtime_forced_on = g_air_runtime->forceEnableTelemetry(host, port, features);
        if (!g_runtime_forced_on) {
            g_loopback->stop();
            g_hook->uninstall();
            g_ctrl->stop();
            return make_bool(false);
        }

        g_runtime_pump_started = true;
    } else {
        // Pump already running from a prior cycle — just repoint the
        // hook's controller at the new CaptureController instance so
        // in-flight bytes land in the fresh .flmc.
        g_hook->install(g_ctrl.get());
    }

    g_pending_markers.clear();
    return make_bool(true);
}

FREObject profiler_stop(FREContext, void*, std::uint32_t, FREObject*) {
    std::lock_guard<std::mutex> g(g_mu);

    // Toggle recording off — the capture file is finalised, but the
    // loopback listener, IAT hook, and Adobe's forced-on telemetry pump
    // stay live. Any further bytes Adobe pushes while we're idle are
    // dropped by CaptureController (state=Idle bumps a drop counter).
    // Tearing those three down here would ECONNRESET Adobe's long-lived
    // SocketTransport and the next profiler_start would never see a byte.
    if (g_ctrl) g_ctrl->stop();
    return make_bool(true);
}

FREObject profiler_get_status(FREContext, void*, std::uint32_t, FREObject*) {
    std::lock_guard<std::mutex> g(g_mu);
    FREObject obj = nullptr;
    FRENewObject(reinterpret_cast<const std::uint8_t*>("Object"), 0, nullptr, &obj, nullptr);
    if (obj == nullptr) return nullptr;

    CaptureController::Status s{};
    if (g_ctrl) s = g_ctrl->status();
    else        s.state = CaptureController::State::Idle;

    set_prop_u32(obj, "state",     static_cast<std::uint32_t>(s.state));
    set_prop_f64(obj, "bytesIn",   static_cast<double>(s.bytes_in));
    set_prop_f64(obj, "bytesOut",  static_cast<double>(s.bytes_out));
    set_prop_f64(obj, "records",   static_cast<double>(s.record_count));
    set_prop_f64(obj, "drops",     static_cast<double>(s.drop_count));
    set_prop_f64(obj, "dropBytes", static_cast<double>(s.drop_bytes));
    set_prop_u32(obj, "elapsedMs", static_cast<std::uint32_t>(s.elapsed_ms > 0xFFFFFFFFu
                                                                 ? 0xFFFFFFFFu
                                                                 : s.elapsed_ms));
    set_prop_u32(obj, "modeBAvailable",
                 (g_air_runtime && g_air_runtime->initialized()) ? 1u : 0u);
    set_prop_u32(obj, "modeBActive", g_runtime_forced_on ? 1u : 0u);
    set_prop_u32(obj, "playerCaptured",
                 (g_air_runtime && g_air_runtime->player() != nullptr) ? 1u : 0u);
    set_prop_u32(obj, "lastError",
                 g_air_runtime ? static_cast<std::uint32_t>(g_air_runtime->lastError()) : 0u);
    if (g_air_runtime) {
        set_prop_f64(obj, "diagSlotTransport",     static_cast<double>(g_air_runtime->diagSlotTransport()));
        set_prop_f64(obj, "diagSlotTelemetry",     static_cast<double>(g_air_runtime->diagSlotTelemetry()));
        set_prop_f64(obj, "diagSlotPlayerTel",     static_cast<double>(g_air_runtime->diagSlotPlayerTelemetry()));
        set_prop_f64(obj, "playerPtr",             static_cast<double>(reinterpret_cast<std::uintptr_t>(g_air_runtime->player())));
        set_prop_f64(obj, "diagChainFrame",        static_cast<double>(g_air_runtime->diagChainFrame()));
        set_prop_f64(obj, "diagChainStep1",        static_cast<double>(g_air_runtime->diagChainStep1()));
        set_prop_f64(obj, "diagChainStep2",        static_cast<double>(g_air_runtime->diagChainStep2()));
        set_prop_f64(obj, "diagChainStep3",        static_cast<double>(g_air_runtime->diagChainStep3()));
        set_prop_f64(obj, "diagPlayerVtable",      static_cast<double>(g_air_runtime->diagPlayerVtable()));
    }
    return obj;
}

FREObject profiler_take_marker(FREContext, void*, std::uint32_t argc, FREObject* argv) {
    std::lock_guard<std::mutex> g(g_mu);
    if (argc < 1) return make_bool(false);
    std::string name;
    if (!read_string(argv[0], name)) return make_bool(false);
    g_pending_markers.emplace_back(std::move(name));
    return make_bool(true);
}

// ----------- registration -------------------------------------------------

void register_all(FRENamedFunction* out_functions, int capacity, int* cursor) {
    if (out_functions == nullptr || cursor == nullptr) return;
    struct Entry { const char* name; FREFunction fn; };
    const Entry entries[] = {
        { "awesomeUtils_profilerStart",       &profiler_start },
        { "awesomeUtils_profilerStop",        &profiler_stop },
        { "awesomeUtils_profilerGetStatus",   &profiler_get_status },
        { "awesomeUtils_profilerTakeMarker",  &profiler_take_marker },
    };
    for (const auto& e : entries) {
        if (*cursor >= capacity) return;
        out_functions[*cursor].name     = reinterpret_cast<const std::uint8_t*>(e.name);
        out_functions[*cursor].function = e.fn;
        out_functions[*cursor].functionData = nullptr;
        ++(*cursor);
    }
}

void shutdown() {
    std::lock_guard<std::mutex> g(g_mu);
    if (g_runtime_forced_on && g_air_runtime) {
        g_air_runtime->forceDisableTelemetry();
        g_runtime_forced_on = false;
    }
    if (g_loopback) g_loopback->stop();
    if (g_ctrl)     g_ctrl->stop();
    if (g_hook)     g_hook->uninstall();
    g_ctrl.reset();
    g_hook.reset();
    g_air_runtime.reset();
    g_loopback.reset();
    g_disk.reset();
    g_pending_markers.clear();
}

} // namespace ane::profiler::bindings
