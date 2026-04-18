// POC v1 — IAT hook on ws2_32!send.
//
// Why IAT hook instead of vtable hook (which I tried first and failed):
//   The RE report listed slot 11 of PlatformSocketWrapper::vftable as the
//   send_bytes thunk, but static analysis of the .rdata shows slot 11
//   actually holds the adjustor thunk for SocketTransport::close_1.
//   The real `send_bytes` body (0x493060) has *zero* references in .text
//   or .rdata — it is invoked through a heap-held function pointer or
//   indirect indexing that I cannot resolve purely from the image.
//
//   In contrast, the `ws2_32.dll!send` import is cleanly located in the
//   IAT at a fixed RVA (0xb05630 in 51.1.3.10 x64; ordinal #19 in
//   ws2_32). Overwriting that one qword slot catches every `send` call
//   Adobe AIR.dll makes, which includes the entire Scout wire traffic.
//
//   Downside: we also intercept any non-telemetry sockets (HTTP, Loader,
//   etc). For the POC that runs against a known .telemetry.cfg target
//   and a tiny loading app, that noise is negligible — and a trivial
//   socket-address filter cleans it up if needed.
//
// Output:
//   %ANE_POC_OUT_PATH%   -- raw bytes written (default %TEMP%\ane_profiler_poc_raw.bin)
//   %ANE_POC_OUT_PATH%.log  -- textual debug log

#include <windows.h>
#include <winsock2.h>
#include <winternl.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <string>

// --- RVAs from docs/profiler-rva-51-1-3-10.md ----------------------------
// Adobe AIR.dll 51.1.3.10 Windows x64.
// SHA256: e24a635554dba434d2cd08ab5b76d0453787a947d0f4a2291e8f0cae9459d6cc
static constexpr std::uint32_t kRvaIatWs2Send = 0x00b05630; // IAT entry for ws2_32!send (#19)

// --- globals --------------------------------------------------------------
typedef int (WSAAPI *SendFn)(SOCKET s, const char* buf, int len, int flags);

static SendFn g_realSend = nullptr;
static HANDLE g_outFile  = INVALID_HANDLE_VALUE;
static HANDLE g_logFile  = INVALID_HANDLE_VALUE;
static std::mutex g_writeMutex;
static std::mutex g_logMutex;
static std::atomic<std::uint64_t> g_bytesCaptured{0};
static std::atomic<std::uint64_t> g_hitCount{0};
static std::atomic<bool> g_hookInstalled{false};

// --- LdrRegisterDllNotification (undocumented but stable since Vista) ----
typedef struct _LDR_DLL_LOADED_NOTIFICATION_DATA {
    ULONG Flags;
    PCUNICODE_STRING FullDllName;
    PCUNICODE_STRING BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} LDR_DLL_LOADED_NOTIFICATION_DATA, *PLDR_DLL_LOADED_NOTIFICATION_DATA;

typedef struct _LDR_DLL_UNLOADED_NOTIFICATION_DATA {
    ULONG Flags;
    PCUNICODE_STRING FullDllName;
    PCUNICODE_STRING BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} LDR_DLL_UNLOADED_NOTIFICATION_DATA, *PLDR_DLL_UNLOADED_NOTIFICATION_DATA;

typedef union _LDR_DLL_NOTIFICATION_DATA {
    LDR_DLL_LOADED_NOTIFICATION_DATA   Loaded;
    LDR_DLL_UNLOADED_NOTIFICATION_DATA Unloaded;
} LDR_DLL_NOTIFICATION_DATA, *PLDR_DLL_NOTIFICATION_DATA;

#define LDR_DLL_NOTIFICATION_REASON_LOADED   1

typedef VOID (CALLBACK *PLDR_DLL_NOTIFICATION_FUNCTION)(
    ULONG, PLDR_DLL_NOTIFICATION_DATA, PVOID);

typedef NTSTATUS (NTAPI *PFN_LdrRegisterDllNotification)(
    ULONG, PLDR_DLL_NOTIFICATION_FUNCTION, PVOID, PVOID*);
typedef NTSTATUS (NTAPI *PFN_LdrUnregisterDllNotification)(PVOID);

static PVOID g_ldrCookie = nullptr;

// --- helpers --------------------------------------------------------------
static void plog(const char* fmt, ...) {
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    OutputDebugStringA(buf);
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_logFile, buf, static_cast<DWORD>(n), &written, nullptr);
    }
}

// --- our replacement for ws2_32!send -------------------------------------
static int WSAAPI hook_send(SOCKET s, const char* buf, int len, int flags) {
    // Call the real send first so the TCP peer (Scout/listener) gets the
    // bytes and returns a real length. We dump whatever it reports as sent.
    int sent = 0;
    if (g_realSend != nullptr) {
        sent = g_realSend(s, buf, len, flags);
    } else {
        // Should never happen after install, but fail-safe.
        sent = len;
    }

    if (sent > 0 && buf != nullptr) {
        std::lock_guard<std::mutex> lk(g_writeMutex);
        if (g_outFile != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            if (WriteFile(g_outFile, buf, static_cast<DWORD>(sent), &written, nullptr)) {
                g_bytesCaptured.fetch_add(written, std::memory_order_relaxed);
            }
        }
    }
    g_hitCount.fetch_add(1, std::memory_order_relaxed);
    return sent;
}

// --- IAT patching ---------------------------------------------------------
static bool install_iat_hook(std::uintptr_t base) {
    if (g_hookInstalled.load(std::memory_order_acquire)) return true;

    void** slot = reinterpret_cast<void**>(base + kRvaIatWs2Send);
    void*  current = *slot;
    plog("[poc] air_base=%p  iat_slot=%p  current_ptr=%p  our=%p\n",
         reinterpret_cast<void*>(base),
         static_cast<void*>(slot),
         current,
         reinterpret_cast<void*>(&hook_send));

    // Validate: current_ptr should point into ws2_32.dll .text.
    HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
    if (ws2 != nullptr) {
        MODULEINFO mi{};
        if (GetModuleInformation(GetCurrentProcess(), ws2, &mi, sizeof(mi))) {
            std::uintptr_t ws2Start = reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll);
            std::uintptr_t ws2End   = ws2Start + mi.SizeOfImage;
            std::uintptr_t cur      = reinterpret_cast<std::uintptr_t>(current);
            if (cur < ws2Start || cur >= ws2End) {
                plog("[poc] WARNING: IAT slot does not point into ws2_32 (%p) [%p..%p]\n",
                     current, (void*)ws2Start, (void*)ws2End);
            } else {
                plog("[poc] IAT slot validated — points into ws2_32\n");
            }
        }
    }

    g_realSend = reinterpret_cast<SendFn>(current);

    DWORD oldProt = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
        plog("[poc] VirtualProtect RW failed err=%lu\n", GetLastError());
        return false;
    }
    *slot = reinterpret_cast<void*>(&hook_send);
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), oldProt, &tmp);

    g_hookInstalled.store(true, std::memory_order_release);
    plog("[poc] IAT hook installed on Adobe AIR.dll!imports.ws2_32.send\n");
    return true;
}

static bool is_adobe_air_basename(PCUNICODE_STRING baseDllName) {
    if (baseDllName == nullptr || baseDllName->Buffer == nullptr) return false;
    const wchar_t* needle = L"Adobe AIR.dll";
    constexpr USHORT needleLen = 13;
    if (baseDllName->Length / sizeof(wchar_t) != needleLen) return false;
    for (USHORT i = 0; i < needleLen; ++i) {
        wchar_t a = baseDllName->Buffer[i], b = needle[i];
        if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a + 32);
        if (b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b + 32);
        if (a != b) return false;
    }
    return true;
}

static VOID CALLBACK ldr_dll_callback(ULONG reason, PLDR_DLL_NOTIFICATION_DATA data, PVOID) {
    if (reason != LDR_DLL_NOTIFICATION_REASON_LOADED || data == nullptr) return;
    if (!is_adobe_air_basename(data->Loaded.BaseDllName)) return;
    plog("[poc] Ldr notification: Adobe AIR.dll mapped at %p size=0x%lx\n",
         data->Loaded.DllBase, static_cast<unsigned long>(data->Loaded.SizeOfImage));
    install_iat_hook(reinterpret_cast<std::uintptr_t>(data->Loaded.DllBase));
}

// --- output file management ----------------------------------------------
static std::wstring resolve_output_path() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ANE_POC_OUT_PATH", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::wstring(buf, n);
    wchar_t temp[MAX_PATH];
    DWORD tlen = GetTempPathW(MAX_PATH, temp);
    std::wstring out(temp, tlen);
    out += L"ane_profiler_poc_raw.bin";
    return out;
}

static bool open_output_files(const std::wstring& outPath) {
    g_outFile = CreateFileW(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::wstring logPath = outPath + L".log";
    g_logFile = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return g_outFile != INVALID_HANDLE_VALUE;
}

static bool register_ldr_callback() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    auto reg = reinterpret_cast<PFN_LdrRegisterDllNotification>(
        GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    if (!reg) return false;
    NTSTATUS st = reg(0, ldr_dll_callback, nullptr, &g_ldrCookie);
    if (st != 0) {
        plog("[poc] LdrRegisterDllNotification failed NTSTATUS=0x%08lx\n",
             static_cast<unsigned long>(st));
        return false;
    }
    HMODULE already = GetModuleHandleW(L"Adobe AIR.dll");
    if (already != nullptr) {
        plog("[poc] Adobe AIR.dll already loaded at register time — patching now\n");
        install_iat_hook(reinterpret_cast<std::uintptr_t>(already));
    } else {
        plog("[poc] Ldr callback registered; awaiting Adobe AIR.dll load\n");
    }
    return true;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hModule);
        const std::wstring outPath = resolve_output_path();
        if (!open_output_files(outPath)) {
            OutputDebugStringA("[poc] failed to open output file — aborting\n");
            return TRUE;
        }
        plog("[poc] DLL_PROCESS_ATTACH  out=%ls  pid=%lu\n",
             outPath.c_str(), GetCurrentProcessId());
        register_ldr_callback();
        break;
    }
    case DLL_PROCESS_DETACH: {
        plog("[poc] DLL_PROCESS_DETACH  hits=%llu  bytes=%llu  installed=%d\n",
             static_cast<unsigned long long>(g_hitCount.load()),
             static_cast<unsigned long long>(g_bytesCaptured.load()),
             static_cast<int>(g_hookInstalled.load()));
        if (g_ldrCookie != nullptr) {
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            if (ntdll) {
                auto unreg = reinterpret_cast<PFN_LdrUnregisterDllNotification>(
                    GetProcAddress(ntdll, "LdrUnregisterDllNotification"));
                if (unreg) unreg(g_ldrCookie);
            }
            g_ldrCookie = nullptr;
        }
        if (g_outFile != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(g_outFile); CloseHandle(g_outFile);
            g_outFile = INVALID_HANDLE_VALUE;
        }
        if (g_logFile != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(g_logFile); CloseHandle(g_logFile);
            g_logFile = INVALID_HANDLE_VALUE;
        }
        break;
    }
    }
    return TRUE;
}
