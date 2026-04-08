// SamplerSafetyHook.cpp — VEH guard for avmplus sampler record parser crash
//
// Problem: When the SWF is compiled with -advanced-telemetry=true, Adobe AIR.dll
// creates a MemoryTelemetrySampler during native boot that starts recording before
// the sampler buffer is fully initialized. The record parser (parseSamplerRecord)
// advances the cursor past valid memory and dereferences an invalid pointer:
//   x86: mov eax, [ecx]  at RVA 0x000B8A71
//   x64: mov rcx, [rax]  at RVA 0x000D3967
// causing ACCESS_VIOLATION (0xC0000005).
//
// Solution: VEH that detects AV inside parseSamplerRecord, redirects to the
// function's exit path (output struct is already zeroed by the prologue), and
// forces the caller's loop counter to 1 so the loop exits cleanly after return.
// The sampler remains active — only the current batch of bad records is lost.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "SamplerSafetyHook.h"
#include "log.h"

// ---- Version-specific data for Adobe AIR.dll 51.1.3.10 ----

struct SamplerPatchInfo {
    DWORD func_start_rva;   // parseSamplerRecord entry
    DWORD func_end_rva;     // last instruction before int3 padding
    DWORD func_exit_rva;    // epilogue (pop regs + ret)
    BOOL  rbx_on_stack;     // x64: RBX is saved at [RSP+0x30] inside the function
    DWORD rbx_stack_offset; // offset from RSP to saved RBX (x64 only)
};

// x86 — verified from crash dump
static const SamplerPatchInfo g_x86_51_1_3_10 = {
    0x000B8A08,  // func_start_rva
    0x000B8AF8,  // func_end_rva
    0x000B8AF5,  // func_exit_rva: pop esi; ret 8
    FALSE,       // EBX is not saved by parseSamplerRecord in x86
    0            // N/A
};

// x64 — verified from static disassembly
static const SamplerPatchInfo g_x64_51_1_3_10 = {
    0x000D38EC,  // func_start_rva
    0x000D3A01,  // func_end_rva
    0x000D39F7,  // func_exit_rva: mov rbx,[rsp+0x30]; add rsp,0x20; pop rdi; ret
    TRUE,        // RBX is saved on stack (used as cursor inside function)
    0x30         // [RSP+0x30] = saved caller's RBX
};

// ---- State ----

static PVOID                   g_vehHandle = nullptr;
static HMODULE                 g_airDll    = nullptr;
static const SamplerPatchInfo* g_patch     = nullptr;
static volatile LONG           g_hitCount  = 0;

// ---- Version detection ----

// Returns TRUE if the loaded Adobe AIR.dll matches version major.minor.build.rev
static BOOL CheckAirVersion(HMODULE hMod, WORD major, WORD minor, WORD build, WORD rev)
{
    char path[MAX_PATH];
    if (!GetModuleFileNameA(hMod, path, MAX_PATH)) return FALSE;

    DWORD dummy;
    DWORD verSize = GetFileVersionInfoSizeA(path, &dummy);
    if (!verSize) return FALSE;

    void* verData = HeapAlloc(GetProcessHeap(), 0, verSize);
    if (!verData) return FALSE;

    BOOL match = FALSE;
    if (GetFileVersionInfoA(path, 0, verSize, verData))
    {
        VS_FIXEDFILEINFO* ffi = nullptr;
        UINT ffiLen = 0;
        if (VerQueryValueA(verData, "\\", (void**)&ffi, &ffiLen) && ffi)
        {
            match = (HIWORD(ffi->dwFileVersionMS) == major &&
                     LOWORD(ffi->dwFileVersionMS) == minor &&
                     HIWORD(ffi->dwFileVersionLS) == build &&
                     LOWORD(ffi->dwFileVersionLS) == rev);

            char msg[128];
            wsprintfA(msg, "[SamplerSafetyHook] AIR version: %d.%d.%d.%d (%s)",
                      HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                      HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS),
                      match ? "MATCH" : "NO MATCH — hook NOT installed");
            writeLog(msg);
        }
    }
    HeapFree(GetProcessHeap(), 0, verData);
    return match;
}

// ---- VEH handler ----

static LONG WINAPI SamplerVEH(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    if (!g_airDll || !g_patch)
        return EXCEPTION_CONTINUE_SEARCH;

    DWORD_PTR base = (DWORD_PTR)g_airDll;
    DWORD_PTR rva  = (DWORD_PTR)ep->ExceptionRecord->ExceptionAddress - base;

    if (rva < g_patch->func_start_rva || rva > g_patch->func_end_rva)
        return EXCEPTION_CONTINUE_SEARCH;

    // --- We are inside parseSamplerRecord ---

    // 1. Force caller's loop counter (EBX/RBX) to 1.
    //    After return, caller does "sub ebx/rbx, 1; jne loop" → exits loop.
#ifdef _WIN64
    if (g_patch->rbx_on_stack)
    {
        // x64: parseSamplerRecord uses RBX as cursor and saves caller's RBX
        // on the stack. Modify the saved value so it's restored as 1.
        *(DWORD64*)(ep->ContextRecord->Rsp + g_patch->rbx_stack_offset) = 1;
    }
    else
    {
        ep->ContextRecord->Rbx = 1;
    }
    // 2. Redirect to function epilogue
    ep->ContextRecord->Rip = base + g_patch->func_exit_rva;
#else
    // x86: EBX is not saved by parseSamplerRecord — modify directly
    ep->ContextRecord->Ebx = 1;
    ep->ContextRecord->Eip = (DWORD)(base + g_patch->func_exit_rva);
#endif

    // 3. Log (first 3 occurrences only to avoid spam)
    LONG hits = InterlockedIncrement(&g_hitCount);
    if (hits <= 3)
    {
        char msg[160];
        wsprintfA(msg,
            "[SamplerSafetyHook] Caught AV in sampler parser at RVA 0x%X "
            "— redirected to exit, loop terminated (hit #%d)",
            (DWORD)rva, (DWORD)hits);
        writeLog(msg);
    }

    return EXCEPTION_CONTINUE_EXECUTION;
}

// ---- Public API ----

void InstallSamplerSafetyHook()
{
    if (g_vehHandle) return;  // already installed

    g_airDll = GetModuleHandleA("Adobe AIR.dll");
    if (!g_airDll)
    {
        writeLog("[SamplerSafetyHook] Adobe AIR.dll not loaded — skipping");
        return;
    }

    // Version check: only install for known versions
    if (CheckAirVersion(g_airDll, 51, 1, 3, 10))
    {
#ifdef _WIN64
        g_patch = &g_x64_51_1_3_10;
#else
        g_patch = &g_x86_51_1_3_10;
#endif
    }
    // Future versions: add more CheckAirVersion blocks here
    // else if (CheckAirVersion(g_airDll, 51, 1, 3, 12)) { ... }

    if (!g_patch)
    {
        writeLog("[SamplerSafetyHook] No patch data for this AIR version — skipping");
        return;
    }

    // Install VEH as first handler (priority 1)
    g_vehHandle = AddVectoredExceptionHandler(1, SamplerVEH);
    if (g_vehHandle)
    {
        char msg[128];
        wsprintfA(msg, "[SamplerSafetyHook] VEH installed — guarding RVA 0x%X-0x%X",
                  g_patch->func_start_rva, g_patch->func_end_rva);
        writeLog(msg);
    }
    else
    {
        writeLog("[SamplerSafetyHook] AddVectoredExceptionHandler failed!");
    }
}

void RemoveSamplerSafetyHook()
{
    if (g_vehHandle)
    {
        RemoveVectoredExceptionHandler(g_vehHandle);
        g_vehHandle = nullptr;
        g_patch     = nullptr;

        char msg[64];
        wsprintfA(msg, "[SamplerSafetyHook] VEH removed (total hits: %d)",
                  (DWORD)g_hitCount);
        writeLog(msg);
    }
}
