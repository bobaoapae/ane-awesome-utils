// POC v1 injector — launches the target AIR app CREATE_SUSPENDED, then
// does LoadLibrary in the remote process via CreateRemoteThread so the hook
// DLL is mapped before any non-suspended code runs. On success, resumes the
// main thread and waits for exit.
//
// Usage:
//   profiler_inject.exe <hook_dll_abs> <target_exe_abs>
//
// Exit codes:
//   0   target exited cleanly
//   N   various failure modes (see stderr)

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

static int die(const wchar_t* msg, int code) {
    fwprintf(stderr, L"[inject] ERROR: %s (GetLastError=%lu)\n", msg, GetLastError());
    return code;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        fwprintf(stderr,
                 L"usage: profiler_inject.exe <hook_dll_abs_path> <target_exe_abs_path>\n");
        return 1;
    }

    const std::wstring dllPath = argv[1];
    const std::wstring targetExe = argv[2];

    // Build command line for the target. CreateProcess needs a mutable buffer.
    std::wstring cmdline = L"\"" + targetExe + L"\"";
    std::wstring cmdlineMutable = cmdline;

    // Pre-check: the DLL must exist and be absolute.
    DWORD dllAttr = GetFileAttributesW(dllPath.c_str());
    if (dllAttr == INVALID_FILE_ATTRIBUTES) {
        return die(L"hook DLL not found at given path", 2);
    }
    DWORD exeAttr = GetFileAttributesW(targetExe.c_str());
    if (exeAttr == INVALID_FILE_ATTRIBUTES) {
        return die(L"target exe not found at given path", 3);
    }

    // Spawn the target fully suspended. Working directory = folder of exe.
    wchar_t exeFolder[MAX_PATH];
    wcscpy_s(exeFolder, MAX_PATH, targetExe.c_str());
    for (size_t i = wcslen(exeFolder); i-- > 0;) {
        if (exeFolder[i] == L'\\' || exeFolder[i] == L'/') {
            exeFolder[i] = L'\0';
            break;
        }
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr,
                        cmdlineMutable.data(),
                        nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED,
                        nullptr,
                        exeFolder,
                        &si, &pi)) {
        return die(L"CreateProcess failed", 4);
    }
    wprintf(L"[inject] target PID=%lu suspended\n", pi.dwProcessId);

    // Write the DLL path into the remote process and trigger LoadLibraryW.
    SIZE_T pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remoteStr = VirtualAllocEx(pi.hProcess, nullptr, pathBytes,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remoteStr == nullptr) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return die(L"VirtualAllocEx failed", 5);
    }

    if (!WriteProcessMemory(pi.hProcess, remoteStr, dllPath.c_str(), pathBytes, nullptr)) {
        VirtualFreeEx(pi.hProcess, remoteStr, 0, MEM_RELEASE);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return die(L"WriteProcessMemory failed", 6);
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLib = GetProcAddress(k32, "LoadLibraryW");
    if (loadLib == nullptr) {
        TerminateProcess(pi.hProcess, 1);
        return die(L"GetProcAddress LoadLibraryW failed", 7);
    }

    HANDLE remoteThr = CreateRemoteThread(pi.hProcess, nullptr, 0,
                                          reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLib),
                                          remoteStr, 0, nullptr);
    if (remoteThr == nullptr) {
        VirtualFreeEx(pi.hProcess, remoteStr, 0, MEM_RELEASE);
        TerminateProcess(pi.hProcess, 1);
        return die(L"CreateRemoteThread failed", 8);
    }

    WaitForSingleObject(remoteThr, 10 * 1000);
    DWORD loaded = 0;
    GetExitCodeThread(remoteThr, &loaded);
    CloseHandle(remoteThr);
    VirtualFreeEx(pi.hProcess, remoteStr, 0, MEM_RELEASE);

    if (loaded == 0) {
        fwprintf(stderr, L"[inject] LoadLibrary returned NULL in remote — inject failed\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 9;
    }
    wprintf(L"[inject] DLL loaded at 0x%p in PID %lu\n",
            reinterpret_cast<void*>(static_cast<uintptr_t>(loaded)),
            pi.dwProcessId);

    // Resume the original main thread and wait for process exit (or caller
    // timeout — we just wait forever here; PowerShell wrapper enforces limit).
    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        TerminateProcess(pi.hProcess, 1);
        return die(L"ResumeThread failed", 10);
    }
    wprintf(L"[inject] main thread resumed — waiting for exit\n");

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    wprintf(L"[inject] target exited with code %lu\n", exitCode);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(exitCode);
}
