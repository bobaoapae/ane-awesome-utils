/**
 * Native signal handler for Android — captures SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSYS
 * and writes crash info to the ane-awesome-utils log file before dying.
 *
 * The log file path is set by Java NativeLogManager via JNI_OnLoad or initSignalHandler().
 * On crash, writes signal info + minimal backtrace to the log, then re-raises for the
 * system debuggerd to generate tombstone / Play Console report.
 */
#include <jni.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <android/log.h>
#include <dlfcn.h>
#include <unwind.h>

#define TAG "CrashSignalHandler"
#define MAX_FRAMES 32

// Callback state for _Unwind_Backtrace
struct BacktraceState {
    uintptr_t* frames;
    int frameCount;
    int maxFrames;
};

static _Unwind_Reason_Code unwindCallback(struct _Unwind_Context* context, void* arg) {
    BacktraceState* state = static_cast<BacktraceState*>(arg);
    uintptr_t ip = _Unwind_GetIP(context);
    if (ip == 0) return _URC_END_OF_STACK;
    if (state->frameCount < state->maxFrames) {
        state->frames[state->frameCount++] = ip;
        return _URC_NO_REASON;
    }
    return _URC_END_OF_STACK;
}

static char g_logFilePath[512] = {0};
static struct sigaction g_oldActions[32]; // big enough for any signal number we use
static const int g_signals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSYS};
static const int g_signalCount = sizeof(g_signals) / sizeof(g_signals[0]);
static bool g_signalsInstalled = false;

// XOR key for log file encryption. Set from Java via nativeInstallSignalHandler.
// Must match LOG_XOR_KEY in NativeLogManager.java — otherwise the crash footer
// written here cannot be decoded when the next session reads the file back.
static unsigned char g_logXorKey[32] = {0};
static int g_logXorKeyLen = 0;

// Weak hook — defined by AirIMEGuard.cpp when linked in. Called at the top of
// signalHandler to let the guard intercept SIGSEGV raised inside a protected
// AIR IME native call and siglongjmp out of it. Returning true means the hook
// longjmp'd and control never reaches here (function is noreturn in that path).
extern "C" __attribute__((weak)) bool AirIMEGuard_maybeLongjmp(int sig, siginfo_t* info, void* ctx);

static const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
        case SIGSYS:  return "SIGSYS";
        default:      return "UNKNOWN";
    }
}

static const char* codeDescription(int sig, int code) {
    if (sig == SIGSEGV) {
        switch (code) {
            case SEGV_MAPERR: return "address not mapped";
            case SEGV_ACCERR: return "invalid permissions";
            default: return "";
        }
    }
    if (sig == SIGBUS) {
        switch (code) {
            case BUS_ADRALN: return "invalid address alignment";
            case BUS_ADRERR: return "non-existent physical address";
            default: return "";
        }
    }
    return "";
}

// Async-signal-safe XOR-encoded write. Matches Java's XorOutputStream encoding
// so the crash footer we append here survives when the next session reads the
// file and XOR-decrypts the whole thing. *offset tracks our running position
// in the encrypted stream and is advanced by the bytes actually written. No
// malloc, no stdio — everything goes through a small stack buffer.
static void writeToLog(int fd, off_t* offset, const char* str) {
    if (fd < 0 || !str || g_logXorKeyLen == 0 || !offset) return;
    size_t len = 0;
    while (str[len]) len++;
    unsigned char buf[256];
    size_t written = 0;
    while (written < len) {
        size_t chunk = len - written;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        for (size_t i = 0; i < chunk; i++) {
            buf[i] = (unsigned char)str[written + i] ^ g_logXorKey[(*offset + i) % g_logXorKeyLen];
        }
        ssize_t w = write(fd, buf, chunk);
        if (w <= 0) break;
        *offset += (off_t)w;
        written += (size_t)w;
    }
}

static void writeHex(int fd, off_t* offset, uintptr_t val) {
    char buf[20];
    buf[0] = '0'; buf[1] = 'x';
    int pos = 18;
    buf[19] = 0;
    for (int i = 2; i < 19; i++) buf[i] = '0';
    while (val && pos >= 2) {
        int d = val & 0xF;
        buf[pos--] = d < 10 ? ('0' + d) : ('a' + d - 10);
        val >>= 4;
    }
    writeToLog(fd, offset, buf);
}

static void signalHandler(int sig, siginfo_t* info, void* ctx) {
    // Give AirIMEGuard first dibs on SIGSEGV — if we're inside a protected
    // IME native call, it will siglongjmp out and this function never returns
    // here. Falls through if the guard isn't armed or this isn't SIGSEGV.
    if (sig == SIGSEGV && AirIMEGuard_maybeLongjmp && AirIMEGuard_maybeLongjmp(sig, info, ctx)) {
        return; // unreachable — longjmp already happened
    }

    // Open log file — note: no O_APPEND. With O_APPEND the kernel would move us
    // to EOF before each write(), so any concurrent writer could slide bytes in
    // between our lseek and our write and we'd XOR-encode at a stale offset.
    // Instead we lseek once to the current EOF and then write sequentially; the
    // race window with the Java writer is narrowed to "between open and lseek".
    int fd = -1;
    if (g_logFilePath[0]) {
        fd = open(g_logFilePath, O_WRONLY | O_CREAT, 0644);
    }

    // Starting offset = current file size = Java's XorOutputStream offset
    // (Java flushes after every write so kernel page cache is authoritative).
    off_t offset = (fd >= 0) ? lseek(fd, 0, SEEK_END) : 0;
    if (offset < 0) offset = 0;

    // Write crash header
    writeToLog(fd, &offset, "\n===== NATIVE CRASH =====\n");
    writeToLog(fd, &offset, "Signal: ");
    writeToLog(fd, &offset, signalName(sig));

    const char* desc = codeDescription(sig, info ? info->si_code : 0);
    if (desc[0]) {
        writeToLog(fd, &offset, " (");
        writeToLog(fd, &offset, desc);
        writeToLog(fd, &offset, ")");
    }

    writeToLog(fd, &offset, "\nFault address: ");
    writeHex(fd, &offset, (uintptr_t)(info ? info->si_addr : 0));

    // Full backtrace via _Unwind_Backtrace + dladdr for symbol resolution
    writeToLog(fd, &offset, "\nBacktrace:\n");

    uintptr_t frames[MAX_FRAMES];
    BacktraceState state = {frames, 0, MAX_FRAMES};
    _Unwind_Backtrace(unwindCallback, &state);

    if (state.frameCount > 0) {
        // We got unwound frames — resolve each one
        char numBuf[8];
        Dl_info dlInfo;
        for (int i = 0; i < state.frameCount; i++) {
            writeToLog(fd, &offset, "  #");
            // Frame index (0-99)
            if (i < 10) {
                numBuf[0] = '0' + i; numBuf[1] = 0;
            } else {
                numBuf[0] = '0' + (i / 10); numBuf[1] = '0' + (i % 10); numBuf[2] = 0;
            }
            writeToLog(fd, &offset, numBuf);
            writeToLog(fd, &offset, " ");
            writeHex(fd, &offset, frames[i]);
            if (dladdr((void*)frames[i], &dlInfo)) {
                writeToLog(fd, &offset, " ");
                writeToLog(fd, &offset, dlInfo.dli_fname ? dlInfo.dli_fname : "?");
                if (dlInfo.dli_sname) {
                    writeToLog(fd, &offset, " (");
                    writeToLog(fd, &offset, dlInfo.dli_sname);
                    writeToLog(fd, &offset, "+");
                    writeHex(fd, &offset, frames[i] - (uintptr_t)dlInfo.dli_saddr);
                    writeToLog(fd, &offset, ")");
                }
            }
            writeToLog(fd, &offset, "\n");
        }
    } else if (ctx) {
        // Fallback: _Unwind_Backtrace failed, extract PC/LR from ucontext
        ucontext_t* uc = (ucontext_t*)ctx;
#if defined(__aarch64__)
        uintptr_t pc = uc->uc_mcontext.pc;
        uintptr_t lr = uc->uc_mcontext.regs[30];
#elif defined(__arm__)
        uintptr_t pc = uc->uc_mcontext.arm_pc;
        uintptr_t lr = uc->uc_mcontext.arm_lr;
#elif defined(__x86_64__)
        uintptr_t pc = uc->uc_mcontext.gregs[REG_RIP];
        uintptr_t lr = 0;
#elif defined(__i386__)
        uintptr_t pc = uc->uc_mcontext.gregs[REG_EIP];
        uintptr_t lr = 0;
#else
        uintptr_t pc = 0, lr = 0;
#endif
        Dl_info dlInfo;
        writeToLog(fd, &offset, "  PC: ");
        writeHex(fd, &offset, pc);
        if (dladdr((void*)pc, &dlInfo)) {
            writeToLog(fd, &offset, " ");
            writeToLog(fd, &offset, dlInfo.dli_fname ? dlInfo.dli_fname : "?");
            if (dlInfo.dli_sname) {
                writeToLog(fd, &offset, " (");
                writeToLog(fd, &offset, dlInfo.dli_sname);
                writeToLog(fd, &offset, "+");
                writeHex(fd, &offset, pc - (uintptr_t)dlInfo.dli_saddr);
                writeToLog(fd, &offset, ")");
            }
        }
        writeToLog(fd, &offset, "\n");
        if (lr) {
            writeToLog(fd, &offset, "  LR: ");
            writeHex(fd, &offset, lr);
            if (dladdr((void*)lr, &dlInfo)) {
                writeToLog(fd, &offset, " ");
                writeToLog(fd, &offset, dlInfo.dli_fname ? dlInfo.dli_fname : "?");
                if (dlInfo.dli_sname) {
                    writeToLog(fd, &offset, " (");
                    writeToLog(fd, &offset, dlInfo.dli_sname);
                    writeToLog(fd, &offset, ")");
                }
            }
            writeToLog(fd, &offset, "\n");
        }
    }

    writeToLog(fd, &offset, "========================\n");

    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }

    // Also log to logcat
    __android_log_print(ANDROID_LOG_FATAL, TAG, "Native crash: %s at %p", signalName(sig), info ? info->si_addr : 0);

    // Re-raise with original handler so debuggerd generates tombstone + Play Console report
    struct sigaction* old = &g_oldActions[sig];
    if (old->sa_flags & SA_SIGINFO) {
        old->sa_sigaction(sig, info, ctx);
    } else if (old->sa_handler == SIG_DFL) {
        // Reset to default and re-raise
        signal(sig, SIG_DFL);
        raise(sig);
    } else if (old->sa_handler != SIG_IGN) {
        old->sa_handler(sig);
    } else {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

// Idempotent sigaction installer. Safe to call from multiple entry points
// (NativeLogManager for crash logging, AirIMEGuard for SEGV recovery). The
// first call wins; later calls are no-ops. Both features share g_oldActions
// so the chain to debuggerd / AIR's handler is preserved exactly once.
extern "C" void AneAwesomeUtils_installSignalHandlerIfNeeded() {
    if (g_signalsInstalled) return;
    g_signalsInstalled = true;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = signalHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    for (int i = 0; i < g_signalCount; i++) {
        sigaction(g_signals[i], &sa, &g_oldActions[g_signals[i]]);
    }

    __android_log_print(ANDROID_LOG_INFO, TAG, "Signal handlers installed");
}

extern "C" {

JNIEXPORT void JNICALL
Java_br_com_redesurftank_aneawesomeutils_NativeLogManager_nativeInstallSignalHandler(JNIEnv* env, jclass clazz, jstring logPath, jbyteArray xorKey) {
    const char* path = env->GetStringUTFChars(logPath, nullptr);
    if (path) {
        strncpy(g_logFilePath, path, sizeof(g_logFilePath) - 1);
        g_logFilePath[sizeof(g_logFilePath) - 1] = 0;
        env->ReleaseStringUTFChars(logPath, path);
    }

    if (xorKey) {
        jsize keyLen = env->GetArrayLength(xorKey);
        if (keyLen > 0 && keyLen <= (jsize)sizeof(g_logXorKey)) {
            jbyte* keyBytes = env->GetByteArrayElements(xorKey, nullptr);
            if (keyBytes) {
                memcpy(g_logXorKey, keyBytes, (size_t)keyLen);
                g_logXorKeyLen = (int)keyLen;
                env->ReleaseByteArrayElements(xorKey, keyBytes, JNI_ABORT);
            }
        }
    }

    AneAwesomeUtils_installSignalHandlerIfNeeded();
    __android_log_print(ANDROID_LOG_INFO, TAG, "Crash log configured (log: %s, keyLen: %d)", g_logFilePath, g_logXorKeyLen);
}

} // extern "C"
