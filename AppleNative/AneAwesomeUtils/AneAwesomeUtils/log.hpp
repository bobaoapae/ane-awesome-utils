//
//  log.hpp
//  WebSocketANE
//
//  Created by João Vitor Borges on 23/08/24.
//

#ifndef log_hpp
#define log_hpp

#include <stdio.h>
#include <cstdint>
#include <string>
#include <functional>

// Existing debug log system (lazy-initialized on first writeLog call)
void initLog();
void writeLog(const char *message);
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
#ifdef __cplusplus
extern "C" {
#endif
__attribute__((visibility("default"))) void AneAwesomeUtils_SharedLog_Write(const char* level, const char* tag, const char* message);
__attribute__((visibility("default"))) const char* AneAwesomeUtils_SharedLog_GetPath();
#ifdef __cplusplus
}
#endif

#endif /* log_hpp */
