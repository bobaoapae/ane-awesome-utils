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
#include <signal.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <execinfo.h>
#include <chrono>

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
static int g_crashFd = -1;             // pre-opened fd for crash handler (async-signal-safe)
static std::string g_nativeLogDir;
static std::string g_nativeLogFilePath;
static std::string g_nativeLogPath;     // static return for initNativeLog
static std::string g_sharedLogPath;     // static return for SharedLog_GetPath
static std::string g_nativeLogCurrentDate;
static size_t g_nativeLogOffset = 0;
static bool g_nativeLogInitialized = false;

static std::mutex g_readResultMutex;
static std::vector<uint8_t> g_readResultData;

static bool g_hadUnexpectedShutdown = false;
static std::string g_crashedSessionFile;

static const uint8_t LOG_XOR_KEY[] = {
    0x4A, 0x7B, 0x2C, 0x5D, 0x1E, 0x6F, 0x3A, 0x8B,
    0x9C, 0x0D, 0xFE, 0xAF, 0x50, 0xE1, 0x72, 0xC3
};
static const size_t LOG_XOR_KEY_LEN = sizeof(LOG_XOR_KEY);

static void xorTransform(uint8_t* data, size_t size, size_t offset) {
    for (size_t i = 0; i < size; i++) {
        data[i] ^= LOG_XOR_KEY[(offset + i) % LOG_XOR_KEY_LEN];
    }
}

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

// fsync a directory to ensure its entries (new files, renames) are persisted on disk.
static void fsyncDir(const std::string& dirPath) {
    int dfd = open(dirPath.c_str(), O_RDONLY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
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

static void formatSessionTimestamp(char* buf, size_t bufSize) {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    snprintf(buf, bufSize, "%04d-%02d-%02d_%02d%02d%02d",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min, lt.tm_sec);
}

static std::string dateFromFilename(const char* filename) {
    // Extract YYYY-MM-DD from "ane-log-YYYY-MM-DD_HHmmss.txt"
    if (strncmp(filename, "ane-log-", 8) != 0) return "";
    if (strlen(filename + 8) < 10) return "";
    return std::string(filename + 8, 10);
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

// forward declarations for crash handler (defined below)
static char g_crashLogPath[512] = {0};
static void installCrashSignalHandlers();

// ── Native log API ─────────────────────────────────────────────────────────────

const char* initNativeLog(const char* basePath, const char* profile) {
    std::lock_guard<std::mutex> lock(g_nativeLogMutex);
    if (g_nativeLogInitialized) {
        return g_nativeLogPath.c_str();
    }

    g_nativeLogDir = std::string(basePath) + "/ane-awesome-utils-logs/" + profile + "/";
    mkdirRecursive(g_nativeLogDir);

    // Check for unexpected shutdown (marker contains crashed session's filename)
    std::string sessionMarker = g_nativeLogDir + ".session_active";
    std::string bgMarker = g_nativeLogDir + ".background_since";
    struct stat st;
    bool sessionMarkerExists = (stat(sessionMarker.c_str(), &st) == 0);

    if (sessionMarkerExists) {
        // Session wasn't closed cleanly - but was it an OS background kill?
        struct stat bgSt;
        bool bgMarkerExists = (stat(bgMarker.c_str(), &bgSt) == 0);

        if (bgMarkerExists) {
            // Read background timestamp
            long long bgSince = 0;
            FILE* bgf = fopen(bgMarker.c_str(), "r");
            if (bgf) {
                char buf[64] = {};
                fread(buf, 1, sizeof(buf) - 1, bgf);
                fclose(bgf);
                bgSince = atoll(buf);
            }
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            long long elapsed = now - bgSince;
            static const long long BACKGROUND_GRACE_MS = 2LL * 60 * 1000; // 2 minutes

            if (bgSince > 0 && elapsed > BACKGROUND_GRACE_MS) {
                // App was in background for a long time - OS killed it
                g_hadUnexpectedShutdown = false;
            } else {
                // Short background - user force-closed
                g_hadUnexpectedShutdown = true;
                FILE* mf = fopen(sessionMarker.c_str(), "r");
                if (mf) {
                    char buf[512] = {};
                    size_t n = fread(buf, 1, sizeof(buf) - 1, mf);
                    fclose(mf);
                    g_crashedSessionFile = std::string(buf, n);
                }
            }
            unlink(bgMarker.c_str());
        } else {
            // No background marker - crashed while in foreground
            g_hadUnexpectedShutdown = true;
            FILE* mf = fopen(sessionMarker.c_str(), "r");
            if (mf) {
                char buf[512] = {};
                size_t n = fread(buf, 1, sizeof(buf) - 1, mf);
                fclose(mf);
                g_crashedSessionFile = std::string(buf, n);
            }
        }
        unlink(sessionMarker.c_str());
    } else {
        g_hadUnexpectedShutdown = false;
        unlink(bgMarker.c_str()); // clean up stale bg marker if any
    }

    // Rotate old logs
    rotateOldLogs(g_nativeLogDir);

    // Open a new session log file (each app launch = new file)
    char dateBuf[16];
    formatDate(dateBuf, sizeof(dateBuf));
    g_nativeLogCurrentDate = dateBuf;
    char sessionBuf[32];
    formatSessionTimestamp(sessionBuf, sizeof(sessionBuf));
    g_nativeLogFilePath = g_nativeLogDir + "ane-log-" + sessionBuf + ".txt";
    g_nativeLogFile = fopen(g_nativeLogFilePath.c_str(), "wb");
    g_nativeLogOffset = 0;

    // Open a raw fd for the crash handler — kept open for the process lifetime.
    // The crash signal handler uses this directly (no open() call during signal).
    if (g_crashFd >= 0) { close(g_crashFd); g_crashFd = -1; }
    g_crashFd = open(g_nativeLogFilePath.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);

    // fsync file + directory to guarantee the file entry is on disk before any crash
    if (g_crashFd >= 0) fsync(g_crashFd);
    fsyncDir(g_nativeLogDir);

    // Create session marker with current log filename
    std::string logFilename = std::string("ane-log-") + sessionBuf + ".txt";
    FILE* marker = fopen(sessionMarker.c_str(), "w");
    if (marker) {
        fwrite(logFilename.c_str(), 1, logFilename.size(), marker);
        fclose(marker);
    }
    fsyncDir(g_nativeLogDir); // persist the marker too

    // Install native signal handlers to capture crash info before death
    strncpy(g_crashLogPath, g_nativeLogFilePath.c_str(), sizeof(g_crashLogPath) - 1);
    installCrashSignalHandlers();

    g_nativeLogInitialized = true;
    g_nativeLogPath = g_nativeLogDir;
    g_sharedLogPath = g_nativeLogDir;
    return g_nativeLogPath.c_str();
}

void writeNativeLog(const char* level, const char* tag, const char* message) {
    std::lock_guard<std::mutex> lock(g_nativeLogMutex);
    if (!g_nativeLogInitialized || !g_nativeLogFile) return;

    // Check if we need to rotate to a new day's file
    char dateBuf[16];
    formatDate(dateBuf, sizeof(dateBuf));
    if (std::string(dateBuf) != g_nativeLogCurrentDate) {
        fclose(g_nativeLogFile);
        if (g_crashFd >= 0) { close(g_crashFd); g_crashFd = -1; }
        g_nativeLogCurrentDate = dateBuf;
        char sessionBuf[32];
        formatSessionTimestamp(sessionBuf, sizeof(sessionBuf));
        g_nativeLogFilePath = g_nativeLogDir + "ane-log-" + sessionBuf + ".txt";
        g_nativeLogFile = fopen(g_nativeLogFilePath.c_str(), "wb");
        g_nativeLogOffset = 0;
        g_crashFd = open(g_nativeLogFilePath.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (g_crashFd >= 0) fsync(g_crashFd);
        fsyncDir(g_nativeLogDir);
        strncpy(g_crashLogPath, g_nativeLogFilePath.c_str(), sizeof(g_crashLogPath) - 1);
    }

    char ts[32];
    formatTimestamp(ts, sizeof(ts));

    char lineBuf[2048];
    int lineLen = snprintf(lineBuf, sizeof(lineBuf), "[%s] [%s] [%s] %s\n", ts, level, tag, message);
    if (lineLen > 0) {
        xorTransform(reinterpret_cast<uint8_t*>(lineBuf), lineLen, g_nativeLogOffset);
        fwrite(lineBuf, 1, lineLen, g_nativeLogFile);
        g_nativeLogOffset += lineLen;
        fflush(g_nativeLogFile);
    }

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

        // Extract date part: ane-log-YYYY-MM-DD_HHmmss.txt -> YYYY-MM-DD
        std::string date = dateFromFilename(entry->d_name);
        if (date.empty()) continue;

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
    if (!g_hadUnexpectedShutdown || g_crashedSessionFile.empty()) return "[]";
    std::lock_guard<std::mutex> lock(g_nativeLogMutex);

    std::string fullPath = g_nativeLogDir + g_crashedSessionFile;
    std::string date = dateFromFilename(g_crashedSessionFile.c_str());

    std::string json = "[{\"date\":\"" + date + "\",\"path\":\"" + fullPath + "\"}]";
    return json;
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
        std::vector<uint8_t> buffer;

        // Scan directory for matching log files
        DIR* d = opendir(dir.c_str());
        if (!d) {
            callback(false, "Cannot open log directory");
            return;
        }

        std::vector<std::string> matched;
        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr) {
            if (strncmp(entry->d_name, "ane-log-", 8) != 0) continue;
            if (!strstr(entry->d_name, ".txt")) continue;
            if (!dateStr.empty()) {
                std::string fileDate = dateFromFilename(entry->d_name);
                if (fileDate != dateStr) continue;
            }
            matched.emplace_back(entry->d_name);
        }
        closedir(d);

        std::sort(matched.begin(), matched.end());

        if (matched.empty() && !dateStr.empty()) {
            callback(false, "File not found");
            return;
        }

        for (auto& fname : matched) {
            std::string fullPath = dir + fname;
            FILE* f = fopen(fullPath.c_str(), "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long fileSize = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (fileSize > 0) {
                size_t prevSize = buffer.size();
                buffer.resize(prevSize + fileSize);
                fread(buffer.data() + prevSize, 1, fileSize, f);
                // XOR-decrypt each file independently (each starts at offset 0)
                xorTransform(buffer.data() + prevSize, fileSize, 0);
            }
            fclose(f);
        }

        {
            std::lock_guard<std::mutex> lock(g_readResultMutex);
            g_readResultData = std::move(buffer);
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

    DIR* d = opendir(g_nativeLogDir.c_str());
    if (!d) return false;

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        if (strncmp(entry->d_name, "ane-log-", 8) != 0) continue;
        if (!dateStr.empty()) {
            std::string fileDate = dateFromFilename(entry->d_name);
            if (fileDate != dateStr) continue;
        }
        std::string fullPath = g_nativeLogDir + entry->d_name;
        // If it's the currently open file, close it first
        if (fullPath == g_nativeLogFilePath && g_nativeLogFile) {
            fclose(g_nativeLogFile);
            g_nativeLogFile = nullptr;
        }
        unlink(fullPath.c_str());
    }
    closedir(d);
    return true;
}

// ── Native signal handler for crash capture ───────────────────────────────────

static struct sigaction g_oldActions[32];
static const int g_crashSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
static const int g_crashSignalCount = sizeof(g_crashSignals) / sizeof(g_crashSignals[0]);

static const char* crashSignalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
        default:      return "UNKNOWN";
    }
}

// Async-signal-safe XOR write — uses g_nativeLogOffset (racy but best-effort)
static void crashWrite(int fd, const char* s) {
    if (fd < 0 || !s) return;
    size_t len = 0;
    while (s[len]) len++;
    char buf[512];
    size_t toWrite = len < sizeof(buf) ? len : sizeof(buf);
    for (size_t i = 0; i < toWrite; i++) {
        buf[i] = s[i] ^ LOG_XOR_KEY[(g_nativeLogOffset + i) % LOG_XOR_KEY_LEN];
    }
    write(fd, buf, toWrite);
    g_nativeLogOffset += toWrite;
}

static void crashWriteHex(int fd, uintptr_t val) {
    char buf[20] = "0x0000000000000000";
    buf[19] = 0;
    int pos = 17;
    while (val && pos >= 2) {
        int d = val & 0xF;
        buf[pos--] = d < 10 ? ('0' + d) : ('a' + d - 10);
        val >>= 4;
    }
    crashWrite(fd, buf);
}

static void crashSignalHandler(int sig, siginfo_t* info, void* ctx) {
    // Use pre-opened fd (set during initNativeLog) — no syscall needed here.
    // Fall back to opening by path only if the pre-opened fd is not available.
    int fd = g_crashFd;
    if (fd < 0 && g_crashLogPath[0]) {
        fd = open(g_crashLogPath, O_WRONLY | O_APPEND | O_CREAT, 0644);
    }

    crashWrite(fd, "\n===== NATIVE CRASH =====\n");
    crashWrite(fd, "Signal: ");
    crashWrite(fd, crashSignalName(sig));
    crashWrite(fd, "\nFault address: ");
    crashWriteHex(fd, (uintptr_t)(info ? info->si_addr : 0));

    // Use execinfo backtrace (available on macOS/iOS)
    crashWrite(fd, "\nBacktrace:\n");
    void* frames[32];
    int count = backtrace(frames, 32);
    // backtrace_symbols is NOT async-signal-safe, but we use dladdr instead
    for (int i = 0; i < count; i++) {
        crashWrite(fd, "  #");
        char num[4] = {0};
        num[0] = '0' + (i / 10);
        num[1] = '0' + (i % 10);
        crashWrite(fd, num);
        crashWrite(fd, "  ");
        crashWriteHex(fd, (uintptr_t)frames[i]);
        Dl_info dlInfo;
        if (dladdr(frames[i], &dlInfo)) {
            crashWrite(fd, " ");
            crashWrite(fd, dlInfo.dli_fname ? dlInfo.dli_fname : "?");
            if (dlInfo.dli_sname) {
                crashWrite(fd, " (");
                crashWrite(fd, dlInfo.dli_sname);
                crashWrite(fd, ")");
            }
        }
        crashWrite(fd, "\n");
    }

    crashWrite(fd, "========================\n");

    if (fd >= 0) {
        fsync(fd);
        // Don't close — process is about to die anyway, and closing the
        // pre-opened g_crashFd could race with the re-raised signal.
    }

    // Re-raise with original handler
    struct sigaction* old = &g_oldActions[sig];
    if (old->sa_flags & SA_SIGINFO) {
        old->sa_sigaction(sig, info, ctx);
    } else if (old->sa_handler == SIG_DFL || old->sa_handler == SIG_IGN) {
        signal(sig, SIG_DFL);
        raise(sig);
    } else {
        old->sa_handler(sig);
    }
}

static void installCrashSignalHandlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crashSignalHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);

    for (int i = 0; i < g_crashSignalCount; i++) {
        sigaction(g_crashSignals[i], &sa, &g_oldActions[g_crashSignals[i]]);
    }
}

void nativeLogOnBackground() {
    if (!g_nativeLogInitialized || g_nativeLogDir.empty()) return;
    std::string bgMarker = g_nativeLogDir + ".background_since";
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    FILE* f = fopen(bgMarker.c_str(), "w");
    if (f) {
        std::string ts = std::to_string(now);
        fwrite(ts.c_str(), 1, ts.size(), f);
        fclose(f);
    }
}

void nativeLogOnForeground() {
    if (!g_nativeLogInitialized || g_nativeLogDir.empty()) return;
    std::string bgMarker = g_nativeLogDir + ".background_since";
    unlink(bgMarker.c_str());
}

void closeNativeLog() {
    std::lock_guard<std::mutex> lock(g_nativeLogMutex);
    if (!g_nativeLogInitialized) return;

    if (g_nativeLogFile) {
        fclose(g_nativeLogFile);
        g_nativeLogFile = nullptr;
    }
    if (g_crashFd >= 0) {
        close(g_crashFd);
        g_crashFd = -1;
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
