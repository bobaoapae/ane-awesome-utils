// JNI bridge — exposes shared/profiler/CaptureController + AndroidRuntimeHook
// to Java. Java side (`Profiler.java`) wraps this; FREFunctions in
// AneAwesomeUtilsContext route AS3 calls to it.
//
// API surface mirrors the Windows ProfilerAneBindings (subset for now):
//   nativeStart(outputPath, headerJson, telemetryPort) -> int
//   nativeStop()                                       -> int
//   nativeGetStatus()                                  -> String (JSON)
//
// State lifecycle:
//   - One global CaptureController + one AndroidRuntimeHook instance.
//   - nativeStart: creates controller (if first call), wires hook with
//     telemetry_port filter, starts capture writer thread.
//   - nativeStop: stops controller, uninstalls hook (so post-stop AIR
//     telemetry sends are not classified — saves CPU).
//   - nativeGetStatus: queries controller + hook diag counters; returns
//     a JSON-compatible struct that the Java side can pass through to AS3.
//
// Threading: each JNI call holds a global mutex. Hot path is the hook
// proxies (no locks across the C++/JNI boundary), so this is fine.

#include <jni.h>
#include <android/log.h>
#include "AndroidRuntimeHook.hpp"
#include "CaptureController.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

#define LOG_TAG "AneProfilerBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

std::mutex                                    g_mu;
std::unique_ptr<ane::profiler::CaptureController> g_ctrl;
std::unique_ptr<ane::profiler::AndroidRuntimeHook> g_hook;

// Helper: fetch a Java String into a std::string. Returns empty on failure.
std::string jstrToCpp(JNIEnv* env, jstring j) {
    if (!j) return {};
    const char* c = env->GetStringUTFChars(j, nullptr);
    if (!c) return {};
    std::string out(c);
    env->ReleaseStringUTFChars(j, c);
    return out;
}

} // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeStart(
        JNIEnv* env, jclass,
        jstring jOutputPath,
        jstring jHeaderJson,
        jint    jTelemetryPort) {
    std::lock_guard<std::mutex> lk(g_mu);

    std::string output_path = jstrToCpp(env, jOutputPath);
    std::string header_json = jstrToCpp(env, jHeaderJson);
    if (output_path.empty()) {
        LOGE("nativeStart: empty output_path");
        return -1;
    }

    if (!g_ctrl) g_ctrl = std::make_unique<ane::profiler::CaptureController>();
    if (!g_hook) g_hook = std::make_unique<ane::profiler::AndroidRuntimeHook>();

    if (g_ctrl->state() != ane::profiler::CaptureController::State::Idle) {
        LOGE("nativeStart: controller not Idle (state=%d)",
             static_cast<int>(g_ctrl->state()));
        return -2;
    }

    ane::profiler::CaptureController::Config cfg;
    cfg.output_path = output_path;
    cfg.header_json = header_json;

    if (!g_ctrl->start(cfg)) {
        LOGE("nativeStart: controller.start failed");
        return -3;
    }

    if (!g_hook->install(g_ctrl.get(),
                         static_cast<std::uint16_t>(jTelemetryPort))) {
        LOGE("nativeStart: hook.install failed");
        g_ctrl->stop();
        return -4;
    }
    LOGI("nativeStart: ok output=%s port=%d", output_path.c_str(), (int)jTelemetryPort);
    return 1;
}

JNIEXPORT jint JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeStop(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_ctrl) return 0;
    if (g_hook) g_hook->uninstall();
    g_ctrl->stop();
    LOGI("nativeStop: ok");
    return 1;
}

JNIEXPORT jstring JNICALL
Java_br_com_redesurftank_aneawesomeutils_Profiler_nativeGetStatus(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    char buf[1024];
    if (!g_ctrl) {
        snprintf(buf, sizeof(buf),
                 "{\"state\":\"NotInitialized\",\"installed\":false}");
        return env->NewStringUTF(buf);
    }
    auto status = g_ctrl->status();
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

} // extern "C"
