// AudioSafetyHook.cpp — IAT hook on waveOutOpen to prevent RPC crashes
//
// Problem: When the Windows Audio Service (Audiosrv) is unavailable,
// waveOutOpen() internally calls an RPC endpoint that raises exception 0x6BA
// (RPC_S_SERVER_UNAVAILABLE). Adobe AIR.dll has no SEH guard around this call,
// so the exception propagates and crashes the process.
//
// Solution: Replace the IAT entry for waveOutOpen in Adobe AIR.dll with a
// wrapper that checks if the audio service is running BEFORE calling the
// real waveOutOpen. If the service is down, return MMSYSERR_NODRIVER
// immediately — never touching the audio stack at all.
// AIR already handles that error gracefully (returns false, continues without audio).
//
// Why not SEH? The RPC runtime has its own nested exception handlers inside
// NdrClientCall2 that interfere with outer __try/__except blocks.
// The pre-check approach avoids the exception entirely.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <cstring>
#include "AudioSafetyHook.h"
#include "log.h"

// ---------- function pointer type ----------
typedef MMRESULT(WINAPI *PFN_waveOutOpen)(
    LPHWAVEOUT phwo, UINT uDeviceID, LPCWAVEFORMATEX pwfx,
    DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen);

// ---------- state ----------
static PFN_waveOutOpen g_originalWaveOutOpen = nullptr;
static void           *g_patchedIatSlot      = nullptr;

// ---------- audio service check ----------
// Queries the Service Control Manager to see if "Audiosrv" (Windows Audio)
// is currently running.  This does NOT touch any audio API, so it cannot
// trigger the RPC exception we're trying to avoid.
static BOOL IsAudioServiceRunning()
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return TRUE;          // can't check → assume running

    SC_HANDLE svc = OpenServiceW(scm, L"Audiosrv", SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return TRUE;                // service not found → assume running
    }

    SERVICE_STATUS_PROCESS ssp;
    DWORD needed = 0;
    BOOL running = FALSE;

    if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                             reinterpret_cast<LPBYTE>(&ssp),
                             sizeof(ssp), &needed))
    {
        running = (ssp.dwCurrentState == SERVICE_RUNNING);
    }
    else
    {
        running = TRUE;             // query failed → assume running
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return running;
}

// ---------- hooked function ----------
static MMRESULT WINAPI HookedWaveOutOpen(
    LPHWAVEOUT phwo, UINT uDeviceID, LPCWAVEFORMATEX pwfx,
    DWORD_PTR dwCallback, DWORD_PTR dwInstance, DWORD fdwOpen)
{
    if (!IsAudioServiceRunning()) {
        writeLog("[AudioSafetyHook] Audiosrv not running — "
                 "skipping waveOutOpen, returning MMSYSERR_NODRIVER");
        return MMSYSERR_NODRIVER;
    }

    return g_originalWaveOutOpen(phwo, uDeviceID, pwfx,
                                dwCallback, dwInstance, fdwOpen);
}

// ---------- generic IAT patcher ----------
static void *PatchIat(HMODULE hModule,
                      const char *dllName,
                      const char *funcName,
                      void       *hookFunc,
                      void      **outOriginal)
{
    auto base = reinterpret_cast<BYTE *>(hModule);

    auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

    auto nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return nullptr;

    auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress);

    for (; imp->Name; ++imp) {
        auto name = reinterpret_cast<const char *>(base + imp->Name);
        if (_stricmp(name, dllName) != 0) continue;

        auto oft = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->OriginalFirstThunk);
        auto iat = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->FirstThunk);

        for (; oft->u1.AddressOfData; ++oft, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;

            auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(
                base + oft->u1.AddressOfData);
            if (strcmp(ibn->Name, funcName) != 0) continue;

            DWORD oldProt;
            if (!VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function),
                                PAGE_READWRITE, &oldProt))
                return nullptr;

            if (outOriginal)
                *outOriginal = reinterpret_cast<void *>(iat->u1.Function);

            iat->u1.Function = reinterpret_cast<ULONG_PTR>(hookFunc);

            VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function),
                           oldProt, &oldProt);

            return &iat->u1.Function;
        }
    }
    return nullptr;
}

// ---------- public API ----------

void InstallAudioSafetyHook()
{
    if (g_patchedIatSlot) return;

    HMODULE airDll = GetModuleHandleA("Adobe AIR.dll");
    if (!airDll) {
        writeLog("[AudioSafetyHook] Adobe AIR.dll not loaded — skipping");
        return;
    }

    void *original = nullptr;
    void *slot = PatchIat(airDll, "winmm.dll", "waveOutOpen",
                          reinterpret_cast<void *>(HookedWaveOutOpen),
                          &original);
    if (slot && original) {
        g_originalWaveOutOpen = reinterpret_cast<PFN_waveOutOpen>(original);
        g_patchedIatSlot      = slot;

        BOOL audioRunning = IsAudioServiceRunning();
        char msg[128];
        wsprintfA(msg, "[AudioSafetyHook] Hooked waveOutOpen — Audiosrv %s",
                  audioRunning ? "RUNNING" : "NOT RUNNING");
        writeLog(msg);
    } else {
        writeLog("[AudioSafetyHook] waveOutOpen not found in Adobe AIR.dll IAT");
    }
}

void RemoveAudioSafetyHook()
{
    if (g_patchedIatSlot && g_originalWaveOutOpen) {
        DWORD oldProt;
        if (VirtualProtect(g_patchedIatSlot, sizeof(ULONG_PTR),
                           PAGE_READWRITE, &oldProt)) {
            *reinterpret_cast<ULONG_PTR *>(g_patchedIatSlot) =
                reinterpret_cast<ULONG_PTR>(g_originalWaveOutOpen);
            VirtualProtect(g_patchedIatSlot, sizeof(ULONG_PTR), oldProt, &oldProt);
            writeLog("[AudioSafetyHook] Restored original waveOutOpen");
        }
        g_originalWaveOutOpen = nullptr;
        g_patchedIatSlot      = nullptr;
    }
}
