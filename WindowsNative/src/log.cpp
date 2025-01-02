#include "log.h"
#include <cstdio>
#include <ctime>
#include <exception>
#include <string>
#include <Windows.h>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            initLog();
            writeLog("DLL loaded (DLL_PROCESS_ATTACH)");
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
        time_t now = time(nullptr);
        tm localTime;
        localtime_s(&localTime, &now);

        char timestamp[20];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d-%H-%M-%S", &localTime);

        // Create unique log file name
        std::string fileName = "C:/debug/ane-awesome-utils-" + std::string(timestamp) + ".txt";
        fopen_s(&logFile, fileName.c_str(), "a");
        if (logFile != nullptr) {
            fprintf(logFile, "Log initialized\n");
            fflush(logFile);
        }
    } catch (std::exception &e) {
        fprintf(stderr, "Error initializing log: %s\n", e.what());
    }
}

void writeLog(const char *message) {
    if (logFile != nullptr) {
        // Get current date and time
        time_t now = time(nullptr);
        tm localTime;
        localtime_s(&localTime, &now);

        char timestamp[20];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &localTime);

        // Write log message with timestamp
        fprintf(logFile, "[%s] %s\n", timestamp, message);
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
