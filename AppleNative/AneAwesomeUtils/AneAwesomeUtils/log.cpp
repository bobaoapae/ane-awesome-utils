#include "log.hpp"
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <string>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <os/log.h>

os_log_t logObject;

// ── Existing debug log system ──────────────────────────────────────────────────
// NOTE: initLog/closeLog are NOT __attribute__((constructor/destructor)) anymore.
// Running as __mod_init_func caused the function to execute before main(),
// which could deadlock or crash if os_log or .NET NativeAOT init hadn't completed.
// Now uses lazy initialization on first writeLog() call.

static bool g_debugLogInitialized = false;

void initLog() {
    if (g_debugLogInitialized) return;
    g_debugLogInitialized = true;
    logObject = os_log_create("br.com.redesurftank.aneawesomeutils", "AneAwesomeUtils");
    os_log(logObject, "Log initialized");
}

void writeLog(const char *message) {
    if (!g_debugLogInitialized) initLog();
    fprintf(stdout, "%s\n", message);
    os_log(logObject, "%{public}s", message);
}

void closeLog() {
    if (g_debugLogInitialized) {
        os_log(logObject, "Log closed");
    }
}

// ── New structured native logging system ───────────────────────────────────────

static std::mutex g_nativeLogMutex;
static FILE* g_nativeLogFile = nullptr;
static std::string g_nativeLogDir;
static std::string g_nativeLogPath;     // static return for initNativeLog
static std::string g_sharedLogPath;     // static return for SharedLog_GetPath
static bool g_nativeLogInitialized = false;

static std::mutex g_readResultMutex;
static std::vector<uint8_t> g_readResultData;

static bool g_hadUnexpectedShutdown = false;

// ── Helpers ────────────────────────────────────────────────────────────────────

static void mkdirRecursive(const std::string& path) {
    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
        current += path[i];
        if (path[i] == '/' || i == path.size() - 1) {
            mkdir(current.c_str(), 0755);
        }
    }
}

static void formatDate(char* buf, size_t bufSize) {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    snprintf(buf, bufSize, "%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}

static void formatTimestamp(char* buf, size_t bufSize) {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    snprintf(buf, bufSize, "%04d-%02d-%02d %02d:%02d:%02d",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min, lt.tm_sec);
}

static bool parseDate(const char* filename, int& year, int& month, int& day) {
    // Expected: ane-log-YYYY-MM-DD.txt
    if (strncmp(filename, "ane-log-", 8) != 0) return false;
    const char* dateStart = filename + 8;
    if (strlen(dateStart) < 14) return false; // YYYY-MM-DD.txt
    if (sscanf(dateStart, "%d-%d-%d", &year, &month, &day) != 3) return false;
    return true;
}

static time_t makeTime(int year, int month, int day) {
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    return mktime(&t);
}

static void rotateOldLogs(const std::string& dir) {
    time_t now = time(nullptr);
    DIR* d = opendir(dir.c_str());
    if (!d) return;

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        int year, month, day;
        if (!parseDate(entry->d_name, year, month, day)) continue;

        time_t fileTime = makeTime(year, month, day);
        double diffDays = difftime(now, fileTime) / (60.0 * 60.0 * 24.0);
        if (diffDays > 7.0) {
            std::string fullPath = dir + "/" + entry->d_name;
            unlink(fullPath.c_str());
        }
    }
    closedir(d);
}

// ── Native log API ─────────────────────────────────────────────────────────────

const char* initNativeLog(const char* basePath, const char* profile) {
    std::lock_guard<std::mutex> lock(g_nativeLogMutex);
    if (g_nativeLogInitialized) {
        return g_nativeLogPath.c_str();
    }

    g_nativeLogDir = std::string(basePath) + "/ane-awesome-utils-logs/" + profile + "/";
    mkdirRecursive(g_nativeLogDir);

    // Check for unexpected shutdown
    std::string sessionMarker = g_nativeLogDir + ".session_active";
    struct stat st;
    g_hadUnexpectedShutdown = (stat(sessionMarker.c_str(), &st) == 0);

    // Rotate old logs
    rotateOldLogs(g_nativeLogDir);

    // Open today's log file
    char dateBuf[16];
    formatDate(dateBuf, sizeof(dateBuf));
    std::string logFilePath = g_nativeLogDir + "ane-log-" + dateBuf + ".txt";
    g_nativeLogFile = fopen(logFilePath.c_str(), "a");

    // Create session marker
    FILE* marker = fopen(sessionMarker.c_str(), "w");
    if (marker) fclose(marker);

    g_nativeLogInitialized = true;
    g_nativeLogPath = g_nativeLogDir;
    g_sharedLogPath = g_nativeLogDir;
    return g_nativeLogPath.c_str();
}

void writeNativeLog(const char* level, const char* tag, const char* message) {
    std::lock_guard<std::mutex> lock(g_nativeLogMutex);
    if (!g_nativeLogInitialized || !g_nativeLogFile) return;

    char ts[32];
    formatTimestamp(ts, sizeof(ts));

    fprintf(g_nativeLogFile, "[%s] [%s] [%s] %s\n", ts, level, tag, message);
    fflush(g_nativeLogFile);

    // Also log via os_log
    os_log(logObject, "[%{public}s] [%{public}s] %{public}s", level, tag, message);
}

std::string getNativeLogFiles() {
    std::lock_guard<std::mutex> lock(g_nativeLogMutex);
    std::string json = "[";
    bool first = true;

    if (g_nativeLogDir.empty()) return "[]";

    DIR* d = opendir(g_nativeLogDir.c_str());
    if (!d) return "[]";

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        if (strncmp(entry->d_name, "ane-log-", 8) != 0) continue;
        const char* ext = strstr(entry->d_name, ".txt");
        if (!ext) continue;

        // Extract date part: ane-log-YYYY-MM-DD.txt -> YYYY-MM-DD
        std::string name(entry->d_name);
        std::string date = name.substr(8, 10);

        // Get file size
        std::string fullPath = g_nativeLogDir + entry->d_name;
        struct stat st;
        long fileSize = 0;
        if (stat(fullPath.c_str(), &st) == 0) {
            fileSize = static_cast<long>(st.st_size);
        }

        if (!first) json += ",";
        first = false;
        json += "{\"date\":\"" + date + "\",\"size\":" + std::to_string(fileSize) + "}";
    }
    closedir(d);

    json += "]";
    return json;
}

bool checkUnexpectedShutdown() {
    return g_hadUnexpectedShutdown;
}

std::string getUnexpectedShutdownInfo() {
    if (!g_hadUnexpectedShutdown) return "{}";
    return "{\"unexpectedShutdown\":true}";
}

bool startAsyncLogRead(const char* date, std::function<void(bool success, const char* error)> callback) {
    std::string dateStr(date ? date : "");
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(g_nativeLogMutex);
        if (!g_nativeLogInitialized) {
            callback(false, "Log not initialized");
            return false;
        }
        dir = g_nativeLogDir;
    }

    std::thread([dateStr, dir, callback]() {
        std::string filePath = dir + "ane-log-" + dateStr + ".txt";

        FILE* f = fopen(filePath.c_str(), "rb");
        if (!f) {
            callback(false, "File not found");
            return;
        }

        fseek(f, 0, SEEK_END);
        long fileSize = ftell(f);
        fseek(f, 0, SEEK_SET);

        std::vector<uint8_t> data(static_cast<size_t>(fileSize));
        if (fileSize > 0) {
            fread(data.data(), 1, static_cast<size_t>(fileSize), f);
        }
        fclose(f);

        {
            std::lock_guard<std::mutex> lock(g_readResultMutex);
            g_readResultData = std::move(data);
        }

        callback(true, nullptr);
    }).detach();

    return true;
}

void getNativeLogReadResult(uint8_t** data, int* size) {
    std::lock_guard<std::mutex> lock(g_readResultMutex);
    if (g_readResultData.empty()) {
        *data = nullptr;
        *size = 0;
        return;
    }
    *data = g_readResultData.data();
    *size = static_cast<int>(g_readResultData.size());
}

void disposeNativeLogReadResult() {
    std::lock_guard<std::mutex> lock(g_readResultMutex);
    g_readResultData.clear();
    g_readResultData.shrink_to_fit();
}

bool deleteNativeLogFiles(const char* date) {
    std::lock_guard<std::mutex> lock(g_nativeLogMutex);
    if (!g_nativeLogInitialized || g_nativeLogDir.empty()) return false;

    std::string dateStr(date ? date : "");

    if (dateStr.empty()) {
        // Delete all log files
        DIR* d = opendir(g_nativeLogDir.c_str());
        if (!d) return false;

        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr) {
            if (strncmp(entry->d_name, "ane-log-", 8) != 0) continue;
            std::string fullPath = g_nativeLogDir + entry->d_name;
            unlink(fullPath.c_str());
        }
        closedir(d);
        return true;
    }

    // Delete specific date's log file
    std::string filePath = g_nativeLogDir + "ane-log-" + dateStr + ".txt";
    return unlink(filePath.c_str()) == 0;
}

void closeNativeLog() {
    std::lock_guard<std::mutex> lock(g_nativeLogMutex);
    if (!g_nativeLogInitialized) return;

    if (g_nativeLogFile) {
        fclose(g_nativeLogFile);
        g_nativeLogFile = nullptr;
    }

    // Remove session marker
    std::string sessionMarker = g_nativeLogDir + ".session_active";
    unlink(sessionMarker.c_str());

    g_nativeLogInitialized = false;
}

// ── Cross-ANE exports ──────────────────────────────────────────────────────────

extern "C" {

__attribute__((visibility("default")))
void AneAwesomeUtils_SharedLog_Write(const char* level, const char* tag, const char* message) {
    writeNativeLog(level, tag, message);
}

__attribute__((visibility("default")))
const char* AneAwesomeUtils_SharedLog_GetPath() {
    return g_sharedLogPath.c_str();
}

}
