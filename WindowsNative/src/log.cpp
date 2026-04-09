#include "log.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <chrono>
#include "WindowsFilterInputs.h"
#include "AudioSafetyHook.h"
#include "SamplerSafetyHook.h"

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
        InstallAudioSafetyHook();
        InstallSamplerSafetyHook();
    } else if (reason == DLL_PROCESS_DETACH) {
        RemoveSamplerSafetyHook();
        RemoveAudioSafetyHook();
        writeLog("DLL unloaded (DLL_PROCESS_DETACH)");
        closeLog();
    }
    return TRUE;
}

// ============================================================================
// Structured native logging system
// ============================================================================

static std::mutex g_nativeLogMutex;
static HANDLE g_nativeLogHandle = INVALID_HANDLE_VALUE;
static std::string g_nativeLogDir;
static std::string g_nativeLogPath;
static std::string g_nativeLogCurrentDate;
static size_t g_nativeLogOffset = 0;
static bool g_unexpectedShutdown = false;
static std::string g_crashedSessionFile;

static std::mutex g_readResultMutex;
static std::vector<uint8_t> g_readResultBuffer;

static const uint8_t LOG_XOR_KEY[] = {
    0x4A, 0x7B, 0x2C, 0x5D, 0x1E, 0x6F, 0x3A, 0x8B,
    0x9C, 0x0D, 0xFE, 0xAF, 0x50, 0xE1, 0x72, 0xC3
};
static const size_t LOG_XOR_KEY_LEN = sizeof(LOG_XOR_KEY);

// Log rotation knobs — must stay in sync with NativeLogManager.java on Android
// and log.cpp on Apple.
static constexpr int ROTATION_DAYS = 7;
static constexpr size_t MAX_LOG_FILES = 30;

static void xorTransform(uint8_t* data, size_t size, size_t offset) {
    for (size_t i = 0; i < size; i++) {
        data[i] ^= LOG_XOR_KEY[(offset + i) % LOG_XOR_KEY_LEN];
    }
}

static void writeXored(HANDLE hFile, const char* data, DWORD size) {
    std::vector<uint8_t> buf(size);
    memcpy(buf.data(), data, size);
    xorTransform(buf.data(), size, g_nativeLogOffset);
    DWORD written = 0;
    WriteFile(hFile, buf.data(), size, &written, NULL);
    g_nativeLogOffset += written;
}

static void createDirectoryRecursive(const std::string& path) {
    size_t pos = 0;
    while ((pos = path.find_first_of("\\/", pos + 1)) != std::string::npos) {
        std::string sub = path.substr(0, pos);
        CreateDirectoryA(sub.c_str(), NULL);
    }
    CreateDirectoryA(path.c_str(), NULL);
}

static void formatNativeTimestamp(char* buf, int bufSize) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wsprintfA(buf, "%04u-%02u-%02u %02u:%02u:%02u",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

static std::string getTodayDateString() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[16];
    wsprintfA(buf, "%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

static std::string getSessionTimestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    wsprintfA(buf, "%04u-%02u-%02u_%02u%02u%02u",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

static std::string dateFromFilename(const std::string& filename) {
    // Extract YYYY-MM-DD from "ane-log-YYYY-MM-DD_HHmmss.txt" (per-session)
    // or legacy "ane-log-YYYY-MM-DD.txt".
    const char* prefix = "ane-log-";
    size_t prefixLen = 8;
    if (filename.size() >= prefixLen + 10 && filename.compare(0, prefixLen, prefix) == 0) {
        return filename.substr(prefixLen, 10);
    }
    return "";
}

static bool isDateOlderThanDays(const std::string& dateStr, int days) {
    if (dateStr.size() < 10) return false;
    SYSTEMTIME fileSt = {};
    fileSt.wYear = static_cast<WORD>(atoi(dateStr.substr(0, 4).c_str()));
    fileSt.wMonth = static_cast<WORD>(atoi(dateStr.substr(5, 2).c_str()));
    fileSt.wDay = static_cast<WORD>(atoi(dateStr.substr(8, 2).c_str()));
    FILETIME fileFt;
    SystemTimeToFileTime(&fileSt, &fileFt);
    ULARGE_INTEGER fileTime;
    fileTime.LowPart = fileFt.dwLowDateTime;
    fileTime.HighPart = fileFt.dwHighDateTime;

    SYSTEMTIME nowSt;
    GetLocalTime(&nowSt);
    nowSt.wHour = 0; nowSt.wMinute = 0; nowSt.wSecond = 0; nowSt.wMilliseconds = 0;
    FILETIME nowFt;
    SystemTimeToFileTime(&nowSt, &nowFt);
    ULARGE_INTEGER nowTime;
    nowTime.LowPart = nowFt.dwLowDateTime;
    nowTime.HighPart = nowFt.dwHighDateTime;

    // 100-nanosecond intervals per day
    ULONGLONG daysTicks = static_cast<ULONGLONG>(days) * 24ULL * 60ULL * 60ULL * 10000000ULL;
    return (nowTime.QuadPart - fileTime.QuadPart) > daysTicks;
}

static std::vector<std::string> scanLogFiles(const std::string& dir) {
    std::vector<std::string> files;
    std::string pattern = dir + "\\ane-log-*.txt";
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                files.emplace_back(fd.cFileName);
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
    std::sort(files.begin(), files.end());
    return files;
}

const char* initNativeLog(const char* basePath, const char* profile) {
    std::lock_guard lock(g_nativeLogMutex);

    if (g_nativeLogHandle != INVALID_HANDLE_VALUE) {
        return g_nativeLogDir.c_str();
    }

    g_nativeLogDir = std::string(basePath) + "\\ane-awesome-utils-logs\\" + profile;
    createDirectoryRecursive(g_nativeLogDir);

    // Check for unexpected shutdown (session marker contains crashed session's filename)
    std::string sessionPath = g_nativeLogDir + "\\.session_active";
    std::string bgPath = g_nativeLogDir + "\\.background_since";
    DWORD attr = GetFileAttributesA(sessionPath.c_str());
    bool sessionMarkerExists = (attr != INVALID_FILE_ATTRIBUTES);

    if (sessionMarkerExists) {
        // Session wasn't closed cleanly - but was it an OS background kill?
        DWORD bgAttr = GetFileAttributesA(bgPath.c_str());
        bool bgMarkerExists = (bgAttr != INVALID_FILE_ATTRIBUTES);

        if (bgMarkerExists) {
            // Read background timestamp
            long long bgSince = 0;
            HANDLE hBg = CreateFileA(bgPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hBg != INVALID_HANDLE_VALUE) {
                char buf[64] = {};
                DWORD bytesRead = 0;
                ReadFile(hBg, buf, sizeof(buf) - 1, &bytesRead, NULL);
                CloseHandle(hBg);
                bgSince = _atoi64(buf);
            }
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            long long elapsed = now - bgSince;
            static const long long BACKGROUND_GRACE_MS = 2LL * 60 * 1000; // 2 minutes

            if (bgSince > 0 && elapsed > BACKGROUND_GRACE_MS) {
                g_unexpectedShutdown = false;
            } else {
                g_unexpectedShutdown = true;
                HANDLE hMarker = CreateFileA(sessionPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hMarker != INVALID_HANDLE_VALUE) {
                    char buf[512] = {};
                    DWORD bytesRead = 0;
                    ReadFile(hMarker, buf, sizeof(buf) - 1, &bytesRead, NULL);
                    CloseHandle(hMarker);
                    g_crashedSessionFile = std::string(buf, bytesRead);
                }
            }
            DeleteFileA(bgPath.c_str());
        } else {
            g_unexpectedShutdown = true;
            HANDLE hMarker = CreateFileA(sessionPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hMarker != INVALID_HANDLE_VALUE) {
                char buf[512] = {};
                DWORD bytesRead = 0;
                ReadFile(hMarker, buf, sizeof(buf) - 1, &bytesRead, NULL);
                CloseHandle(hMarker);
                g_crashedSessionFile = std::string(buf, bytesRead);
            }
        }
        DeleteFileA(sessionPath.c_str());
    } else {
        g_unexpectedShutdown = false;
        DeleteFileA(bgPath.c_str()); // clean up stale bg marker if any
    }

    // Age-based rotation: drop anything older than ROTATION_DAYS.
    {
        auto files = scanLogFiles(g_nativeLogDir);
        for (auto& f : files) {
            std::string date = dateFromFilename(f);
            if (isDateOlderThanDays(date, ROTATION_DAYS)) {
                std::string fullPath = g_nativeLogDir + "\\" + f;
                DeleteFileA(fullPath.c_str());
            }
        }
    }

    // Count-based rotation: every session creates a new file, so frequent
    // launches pile up files until the age cutoff kicks in. Cap at
    // MAX_LOG_FILES and drop the oldest. scanLogFiles already returns them
    // sorted alphabetically, which matches chronological order because the
    // filename embeds a sortable timestamp ("ane-log-YYYY-MM-DD_HHmmss.txt").
    {
        auto files = scanLogFiles(g_nativeLogDir);
        if (files.size() > MAX_LOG_FILES) {
            size_t toDelete = files.size() - MAX_LOG_FILES;
            for (size_t i = 0; i < toDelete; i++) {
                std::string fullPath = g_nativeLogDir + "\\" + files[i];
                DeleteFileA(fullPath.c_str());
            }
        }
    }

    // Open a new session log file (each app launch = new file)
    g_nativeLogCurrentDate = getTodayDateString();
    std::string sessionTs = getSessionTimestamp();
    g_nativeLogPath = g_nativeLogDir + "\\ane-log-" + sessionTs + ".txt";
    g_nativeLogHandle = CreateFileA(
        g_nativeLogPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    g_nativeLogOffset = 0;

    // Flush log file to disk to ensure it persists through crashes
    if (g_nativeLogHandle != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_nativeLogHandle);
    }

    // Create session marker with current log filename
    HANDLE hSession = CreateFileA(sessionPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hSession != INVALID_HANDLE_VALUE) {
        std::string logFilename = "ane-log-" + sessionTs + ".txt";
        DWORD written = 0;
        WriteFile(hSession, logFilename.c_str(), static_cast<DWORD>(logFilename.size()), &written, NULL);
        FlushFileBuffers(hSession);
        CloseHandle(hSession);
    }

    return g_nativeLogDir.c_str();
}

void writeNativeLog(const char* level, const char* tag, const char* message) {
    std::lock_guard lock(g_nativeLogMutex);

    // Check if we need to rotate to a new day's file
    std::string today = getTodayDateString();
    if (g_nativeLogHandle != INVALID_HANDLE_VALUE && today != g_nativeLogCurrentDate) {
        CloseHandle(g_nativeLogHandle);
        g_nativeLogCurrentDate = today;
        std::string sessionTs = getSessionTimestamp();
        g_nativeLogPath = g_nativeLogDir + "\\ane-log-" + sessionTs + ".txt";
        g_nativeLogHandle = CreateFileA(
            g_nativeLogPath.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            NULL,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );
        g_nativeLogOffset = 0;
    }

    if (g_nativeLogHandle == INVALID_HANDLE_VALUE) return;

    char ts[32];
    formatNativeTimestamp(ts, sizeof(ts));

    // Format: [YYYY-MM-DD HH:MM:SS] [LEVEL] [TAG] message\r\n
    std::string line = std::string("[") + ts + "] [" + (level ? level : "") + "] [" + (tag ? tag : "") + "] " + (message ? message : "") + "\r\n";

    writeXored(g_nativeLogHandle, line.c_str(), static_cast<DWORD>(line.size()));
    FlushFileBuffers(g_nativeLogHandle);

    printf("[%s] [%s] [%s] %s\n", ts, level ? level : "", tag ? tag : "", message ? message : "");
}

std::string getNativeLogFiles() {
    std::lock_guard lock(g_nativeLogMutex);

    std::string json = "[";
    auto files = scanLogFiles(g_nativeLogDir);
    bool first = true;
    for (auto& f : files) {
        std::string fullPath = g_nativeLogDir + "\\" + f;
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(fullPath.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            ULARGE_INTEGER fileSize;
            fileSize.LowPart = fd.nFileSizeLow;
            fileSize.HighPart = fd.nFileSizeHigh;
            std::string date = dateFromFilename(f);
            if (!first) json += ",";
            first = false;
            json += "{\"date\":\"" + date + "\",\"size\":" + std::to_string(fileSize.QuadPart) + ",\"path\":\"";
            // Escape backslashes for JSON
            for (char c : fullPath) {
                if (c == '\\') json += "\\\\";
                else json += c;
            }
            json += "\"}";
            FindClose(hFind);
        }
    }
    json += "]";
    return json;
}

bool checkUnexpectedShutdown() {
    return g_unexpectedShutdown;
}

std::string getUnexpectedShutdownInfo() {
    std::lock_guard lock(g_nativeLogMutex);

    if (g_crashedSessionFile.empty()) return "[]";

    std::string fullPath = g_nativeLogDir + "\\" + g_crashedSessionFile;
    std::string date = dateFromFilename(g_crashedSessionFile);

    std::string json = "[{\"date\":\"" + date + "\",\"path\":\"";
    for (char c : fullPath) {
        if (c == '\\') json += "\\\\";
        else json += c;
    }
    json += "\"}]";
    return json;
}

bool startAsyncLogRead(const char* date, std::function<void(bool success, const char* error)> callback) {
    std::string dateStr = date ? date : "";
    std::string dir = g_nativeLogDir;

    std::thread([dateStr, dir, callback]() {
        std::vector<uint8_t> buffer;
        auto files = scanLogFiles(dir);

        // Filter by date prefix if specified
        std::vector<std::string> matched;
        for (auto& f : files) {
            if (!dateStr.empty()) {
                std::string d = dateFromFilename(f);
                if (d != dateStr) continue;
            }
            matched.push_back(f);
        }

        if (matched.empty() && !dateStr.empty()) {
            if (callback) callback(false, "File not found");
            return;
        }

        for (auto& f : matched) {
            std::string fullPath = dir + "\\" + f;
            HANDLE hFile = CreateFileA(fullPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile == INVALID_HANDLE_VALUE) continue;
            DWORD fileSize = GetFileSize(hFile, NULL);
            if (fileSize > 0 && fileSize != INVALID_FILE_SIZE) {
                size_t prevSize = buffer.size();
                buffer.resize(prevSize + fileSize);
                DWORD bytesRead = 0;
                ReadFile(hFile, buffer.data() + prevSize, fileSize, &bytesRead, NULL);
                buffer.resize(prevSize + bytesRead);
                xorTransform(buffer.data() + prevSize, bytesRead, 0);
            }
            CloseHandle(hFile);
        }

        {
            std::lock_guard lock(g_readResultMutex);
            g_readResultBuffer = std::move(buffer);
        }

        if (callback) callback(true, nullptr);
    }).detach();

    return true;
}

void getNativeLogReadResult(uint8_t** data, int* size) {
    std::lock_guard lock(g_readResultMutex);
    if (g_readResultBuffer.empty()) {
        *data = nullptr;
        *size = 0;
    } else {
        *data = g_readResultBuffer.data();
        *size = static_cast<int>(g_readResultBuffer.size());
    }
}

void disposeNativeLogReadResult() {
    std::lock_guard lock(g_readResultMutex);
    g_readResultBuffer.clear();
    g_readResultBuffer.shrink_to_fit();
}

bool deleteNativeLogFiles(const char* date) {
    std::lock_guard lock(g_nativeLogMutex);

    std::string dateStr = date ? date : "";
    auto files = scanLogFiles(g_nativeLogDir);

    for (auto& f : files) {
        if (!dateStr.empty()) {
            std::string d = dateFromFilename(f);
            if (d != dateStr) continue;
        }
        std::string fullPath = g_nativeLogDir + "\\" + f;
        // If it's the currently open file, close it first
        if (fullPath == g_nativeLogPath && g_nativeLogHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(g_nativeLogHandle);
            g_nativeLogHandle = INVALID_HANDLE_VALUE;
        }
        DeleteFileA(fullPath.c_str());
    }
    return true;
}

void nativeLogOnBackground() {
    if (g_nativeLogDir.empty()) return;
    std::string bgPath = g_nativeLogDir + "\\.background_since";
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    HANDLE hBg = CreateFileA(bgPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hBg != INVALID_HANDLE_VALUE) {
        std::string ts = std::to_string(now);
        DWORD written = 0;
        WriteFile(hBg, ts.c_str(), static_cast<DWORD>(ts.size()), &written, NULL);
        FlushFileBuffers(hBg);
        CloseHandle(hBg);
    }
}

void nativeLogOnForeground() {
    if (g_nativeLogDir.empty()) return;
    std::string bgPath = g_nativeLogDir + "\\.background_since";
    DeleteFileA(bgPath.c_str());
}

void closeNativeLog() {
    std::lock_guard lock(g_nativeLogMutex);

    if (g_nativeLogHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_nativeLogHandle);
        g_nativeLogHandle = INVALID_HANDLE_VALUE;
    }

    // Delete session marker
    std::string sessionPath = g_nativeLogDir + "\\.session_active";
    DeleteFileA(sessionPath.c_str());
}

// Cross-ANE exports
extern "C" {
    __declspec(dllexport) void AneAwesomeUtils_SharedLog_Write(const char* level, const char* tag, const char* message) {
        writeNativeLog(level, tag, message);
    }

    __declspec(dllexport) const char* AneAwesomeUtils_SharedLog_GetPath() {
        return g_nativeLogDir.c_str();
    }
}