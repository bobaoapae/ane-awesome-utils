#include "log.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdio>
#include "WindowsFilterInputs.h"

static HANDLE g_logHandle = NULL;

static BOOL directoryExists(const char* path) {
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
}

static void formatTimestampFile(char* buf, int bufSize) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wsprintfA(buf, "%04u-%02u-%02u-%02u-%02u-%02u",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

static void formatTimestampLine(char* buf, int bufSize) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wsprintfA(buf, "%04u-%02u-%02u %02u:%02u:%02u",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

void initLog() {
    if (g_logHandle) return;
    if (!directoryExists("C:\\debug1")) return;
    char ts[32];
    formatTimestampFile(ts, sizeof(ts));
    char path[MAX_PATH];
    wsprintfA(path, "C:\\debug1\\ane-awesome-utils-%s.txt", ts);
    g_logHandle = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

void writeLog(const char* message) {
    if (!g_logHandle) initLog();
    if (!g_logHandle || g_logHandle == INVALID_HANDLE_VALUE) return;
    char ts[32];
    formatTimestampLine(ts, sizeof(ts));
    DWORD written = 0;
    WriteFile(g_logHandle, "[", 1, &written, NULL);
    WriteFile(g_logHandle, ts, (DWORD)lstrlenA(ts), &written, NULL);
    WriteFile(g_logHandle, "] ", 2, &written, NULL);
    if (message) {
        WriteFile(g_logHandle, message, (DWORD)lstrlenA(message), &written, NULL);
    }
    WriteFile(g_logHandle, "\r\n", 2, &written, NULL);
    FlushFileBuffers(g_logHandle);
}

void closeLog() {
    if (g_logHandle && g_logHandle != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_logHandle, "Log closed\r\n", 12, &written, NULL);
        CloseHandle(g_logHandle);
        g_logHandle = NULL;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        SetAneModuleHandle(hModule);
        initLog();
        writeLog("DLL loaded (DLL_PROCESS_ATTACH)");
    } else if (reason == DLL_PROCESS_DETACH) {
        writeLog("DLL unloaded (DLL_PROCESS_DETACH)");
        closeLog();
    }
    return TRUE;
}