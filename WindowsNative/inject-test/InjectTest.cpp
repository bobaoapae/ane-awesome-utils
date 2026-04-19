// InjectTest — early-load DLL that deposits a transient .telemetry.cfg
// in the user profile so Adobe AIR.dll's init_telemetry sees a valid
// config at Player startup. This is what unlocks AvmCore::ctor's sampler
// creation path (needed for `.sampler.sample` + `.sampler.methodNameMap`
// records to flow into Scout).
//
// Expected to load via PE import-table injection on either the captive
// EXE or Adobe AIR.dll itself. PROCESS_ATTACH writes the cfg; the app
// start-up then reads it normally. PROCESS_DETACH deletes the file so
// the user profile stays clean after the app exits.
//
// STATUS (as of 2026-04-19): the concept is validated end-to-end
// manually (writing/deleting the cfg by hand around a launch produces
// a working sampler session — see docs/profiler-mode-b-sampler-gap.md),
// but the automated injection path via PE import-table rewrite has not
// been made to fire DllMain on the captive-EXE target. Investigation
// pending:
//   - TestProfilerApp.exe (CaptiveAppEntry) patched with a new
//     .newimp section + extra import descriptor: loader reports the
//     DLL as loaded but DllMain is never invoked. Unclear whether a
//     second pass is needed, or whether a descriptor-field detail is
//     off. 1-byte patches to Adobe AIR.dll itself DO take effect, so
//     the captive runtime isn't doing byte-integrity checks — the
//     issue is specific to how the rebuilt import table is processed.
//   - Simpler alternative for production: direct byte patch of
//     cfgDefaultInit (Adobe AIR.dll RVA 0x38b2a7) so it hardcodes the
//     host/port/flags on every invocation. Fully in-place, no new
//     section, no DLL to bundle. Enters the SDK under patches/
//     fix_telemetry_mode_b/apply.py.
//
// Built static-CRT (kernel32 only) so the DllMain runs even very early
// in the load order, before VCRUNTIME / UCRT are pulled in.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static char g_cfg_path[MAX_PATH];

static void write_cfg() {
    DWORD n = GetEnvironmentVariableA("USERPROFILE", g_cfg_path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 32) return;
    const char* tail = "\.telemetry.cfg";
    for (int i = 0; tail[i] && n + i < MAX_PATH - 1; ++i) {
        g_cfg_path[n + i] = tail[i];
    }
    g_cfg_path[n + 15] = 0;

    HANDLE h = CreateFileA(g_cfg_path, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const char body[] =
        "TelemetryAddress=127.0.0.1:9999\r\n"
        "SamplerEnabled=true\r\n"
        "CPUCapture=true\r\n"
        "ScriptObjectAllocationTraces=true\r\n"
        "AllGCAllocationTraces=true\r\n"
        "GCAllocationTracesThreshold=1024\r\n";
    DWORD written = 0;
    WriteFile(h, body, sizeof(body) - 1, &written, nullptr);
    CloseHandle(h);
}

static void remove_cfg() {
    if (g_cfg_path[0]) DeleteFileA(g_cfg_path);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) write_cfg();
    else if (reason == DLL_PROCESS_DETACH) remove_cfg();
    return TRUE;
}

// Named export so the PE import referencing this symbol resolves cleanly
// (Windows' loader wants at least one named or ordinal binding per
// descriptor). The function itself is never called.
extern "C" __declspec(dllexport) void InjectTestKeepSymbol() {}
