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

#define TAG "CrashSignalHandler"
#define MAX_FRAMES 32

static char g_logFilePath[512] = {0};
static struct sigaction g_oldActions[32]; // big enough for any signal number we use
static const int g_signals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSYS};
static const int g_signalCount = sizeof(g_signals) / sizeof(g_signals[0]);

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

// Async-signal-safe write to log file. No malloc, no stdio.
static void writeToLog(int fd, const char* str) {
    if (fd < 0 || !str) return;
    size_t len = 0;
    while (str[len]) len++;
    write(fd, str, len);
}

static void writeHex(int fd, uintptr_t val) {
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
    writeToLog(fd, buf);
}

static void signalHandler(int sig, siginfo_t* info, void* ctx) {
    // Open log file in append mode (async-signal-safe via open())
    int fd = -1;
    if (g_logFilePath[0]) {
        fd = open(g_logFilePath, O_WRONLY | O_APPEND | O_CREAT, 0644);
    }

    // Write crash header
    writeToLog(fd, "\n===== NATIVE CRASH =====\n");
    writeToLog(fd, "Signal: ");
    writeToLog(fd, signalName(sig));

    const char* desc = codeDescription(sig, info ? info->si_code : 0);
    if (desc[0]) {
        writeToLog(fd, " (");
        writeToLog(fd, desc);
        writeToLog(fd, ")");
    }

    writeToLog(fd, "\nFault address: ");
    writeHex(fd, (uintptr_t)(info ? info->si_addr : 0));

    // Minimal backtrace using libunwind (available in NDK)
    writeToLog(fd, "\nBacktrace:\n");
    void* frames[MAX_FRAMES];
    // _Unwind_Backtrace is complex; use dladdr for the PC from ucontext instead
    if (ctx) {
        ucontext_t* uc = (ucontext_t*)ctx;
#if defined(__aarch64__)
        uintptr_t pc = uc->uc_mcontext.pc;
        uintptr_t lr = uc->uc_mcontext.regs[30]; // x30 = LR
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
        writeToLog(fd, "  PC: ");
        writeHex(fd, pc);
        if (dladdr((void*)pc, &dlInfo)) {
            writeToLog(fd, " ");
            writeToLog(fd, dlInfo.dli_fname ? dlInfo.dli_fname : "?");
            if (dlInfo.dli_sname) {
                writeToLog(fd, " (");
                writeToLog(fd, dlInfo.dli_sname);
                writeToLog(fd, "+");
                writeHex(fd, pc - (uintptr_t)dlInfo.dli_saddr);
                writeToLog(fd, ")");
            }
        }
        writeToLog(fd, "\n");

        if (lr) {
            writeToLog(fd, "  LR: ");
            writeHex(fd, lr);
            if (dladdr((void*)lr, &dlInfo)) {
                writeToLog(fd, " ");
                writeToLog(fd, dlInfo.dli_fname ? dlInfo.dli_fname : "?");
                if (dlInfo.dli_sname) {
                    writeToLog(fd, " (");
                    writeToLog(fd, dlInfo.dli_sname);
                    writeToLog(fd, ")");
                }
            }
            writeToLog(fd, "\n");
        }
    }

    writeToLog(fd, "========================\n");

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

extern "C" {

JNIEXPORT void JNICALL
Java_br_com_redesurftank_aneawesomeutils_NativeLogManager_nativeInstallSignalHandler(JNIEnv* env, jclass clazz, jstring logPath) {
    const char* path = env->GetStringUTFChars(logPath, nullptr);
    if (path) {
        strncpy(g_logFilePath, path, sizeof(g_logFilePath) - 1);
        g_logFilePath[sizeof(g_logFilePath) - 1] = 0;
        env->ReleaseStringUTFChars(logPath, path);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = signalHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    for (int i = 0; i < g_signalCount; i++) {
        sigaction(g_signals[i], &sa, &g_oldActions[g_signals[i]]);
    }

    __android_log_print(ANDROID_LOG_INFO, TAG, "Signal handlers installed for crash capture (log: %s)", g_logFilePath);
}

} // extern "C"
