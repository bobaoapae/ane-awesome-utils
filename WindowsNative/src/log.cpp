#include "log.h"
#include <cstdio>
#include <exception>
#include <Windows.h>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            initLog();
        writeLog("DLL loaded (DLL_PROCESS_ATTACH)");
        break;
        case DLL_THREAD_ATTACH:
            writeLog("DLL loaded (DLL_THREAD_ATTACH)");
        break;
        case DLL_THREAD_DETACH:
            writeLog("DLL unloaded (DLL_THREAD_DETACH)");
        break;
        case DLL_PROCESS_DETACH:
            writeLog("DLL unloaded (DLL_PROCESS_DETACH)");
        closeLog();
        break;
    }
    return TRUE;
}

FILE *logFile = nullptr;

void initLog() {
    try {
        fopen_s(&logFile, "C:/debug/ane-awesome-utils.txt", "a");
        if (logFile != nullptr) {
            fprintf(logFile, "Log initialized\n");
            fflush(logFile);
        }
    } catch (std::exception &e) {
        fprintf(stderr, "Error initializing log: %s\n", e.what());
    }
}

void writeLog(const char *message) {
    //fprintf(stdout, "%s\n", message);
    if (logFile != nullptr) {
        fprintf(logFile, "%s\n", message);
        fflush(logFile);
    }
}

void closeLog() {
    if (logFile != nullptr) {
        fprintf(logFile, "Log closed\n");
        fclose(logFile);
        logFile = nullptr;
    }
}
