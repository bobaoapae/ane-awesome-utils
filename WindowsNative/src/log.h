#ifndef LOG_H
#define LOG_H

#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Existing debug log system
void initLog();
void writeLog(const char* message);
void closeLog();

// New structured native logging system
const char* initNativeLog(const char* basePath, const char* profile);
void writeNativeLog(const char* level, const char* tag, const char* message);
std::string getNativeLogFiles();
bool checkUnexpectedShutdown();
std::string getUnexpectedShutdownInfo();
bool startAsyncLogRead(const char* date, std::function<void(bool success, const char* error)> callback);
void getNativeLogReadResult(uint8_t** data, int* size);
void disposeNativeLogReadResult();
bool deleteNativeLogFiles(const char* date);
void closeNativeLog();

// Cross-ANE exports
#ifdef _WIN32
extern "C" {
    __declspec(dllexport) void AneAwesomeUtils_SharedLog_Write(const char* level, const char* tag, const char* message);
    __declspec(dllexport) const char* AneAwesomeUtils_SharedLog_GetPath();
}
#endif

#endif // LOG_H
