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
#include <sys/syscall.h>
#include <sys/types.h>
#include <linux/limits.h>

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
// Marker file written ONLY by the signal handler when a real crash signal fires.
// Next session checks for this marker to disambiguate "process killed by OS / user
// swipe-from-recents" (no marker → don't report) from "real native crash" (marker
// present → report). Path is derived from g_logFilePath at install time:
// "<logDir>/.crash_marker"
static char g_crashMarkerPath[576] = {0};
static struct sigaction g_oldActions[32]; // big enough for any signal number we use
static const int g_signals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSYS};
static const int g_signalCount = sizeof(g_signals) / sizeof(g_signals[0]);
static bool g_signalsInstalled = false;

// XOR key for log file encryption. Set from Java via nativeInstallSignalHandler.
// Must match LOG_XOR_KEY in NativeLogManager.java — otherwise the crash footer
// written here cannot be decoded when the next session reads the file back.
static unsigned char g_logXorKey[32] = {0};
static int g_logXorKeyLen = 0;

// ── Breadcrumb: runtime snapshot updated by RuntimeStatsCollector every 30s.
// Read async-signal-safely from the signal handler to append a "state at
// moment of death" footer to the crash log. volatile because it's written
// from the main looper thread and read from whichever thread crashed — we
// tolerate stale/inconsistent reads (single-tick skew is acceptable for
// diagnosis; no synchronization needed to keep signal handler safe).
static volatile int  g_bc_threads      = -1;
static volatile long g_bc_jvm_used_kb  = -1;
static volatile long g_bc_jvm_max_kb   = -1;
static volatile long g_bc_native_kb    = -1;
static volatile long g_bc_vmrss_kb     = -1;
static volatile long g_bc_vmsize_kb    = -1;
static volatile long g_bc_time_sec     =  0;  // 0 = never updated

// Weak hook — defined by AirIMEGuard.cpp when linked in. Called at the top of
// signalHandler to let the guard intercept SIGSEGV raised inside a protected
// AIR IME native call and siglongjmp out of it. Returning true means the hook
// longjmp'd and control never reaches here (function is noreturn in that path).
extern "C" __attribute__((weak)) bool AirIMEGuard_maybeLongjmp(int sig, siginfo_t* info, void* ctx);

// Same pattern for the Phase 4c AS3-object walker (AndroidAs3ObjectHook). The
// drain thread dereferences arbitrary heap pointers while resolving class
// names; landing in unmapped pages is expected and survivable. The walker
// arms a per-thread sigsetjmp before each guarded read; this callback
// performs the corresponding siglongjmp when SIGSEGV/SIGBUS fires inside it.
extern "C" __attribute__((weak)) bool As3WalkerGuard_maybeLongjmp(int sig, siginfo_t* info, void* ctx);

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

// Manual base-10 itoa — async-signal-safe (no locale, no malloc, no snprintf).
// Writes into caller's fd/offset via writeToLog which is already safe.
static void writeDec(int fd, off_t* offset, long val) {
    char buf[24];
    buf[23] = 0;
    int pos = 23;
    bool negative = val < 0;
    unsigned long u = negative ? (unsigned long)(-(val + 1)) + 1UL : (unsigned long)val;
    if (u == 0UL) {
        buf[--pos] = '0';
    } else {
        while (u && pos > 0) {
            buf[--pos] = (char)('0' + (u % 10UL));
            u /= 10UL;
        }
    }
    if (negative && pos > 0) buf[--pos] = '-';
    writeToLog(fd, offset, buf + pos);
}

// Copy the contents of a small /proc file (maps, status, comm) into the log,
// XOR-encoded the same way. Reads chunked into a stack buffer. Async-signal-safe:
// open/read/close are syscalls, no libc allocation. Returns false if file missing.
static bool writeFileContents(int destFd, off_t* offset, const char* srcPath) {
    int src = open(srcPath, O_RDONLY);
    if (src < 0) return false;
    char buf[512];
    ssize_t n;
    while ((n = read(src, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = 0;
        writeToLog(destFd, offset, buf);
    }
    close(src);
    return true;
}

// Dump the `comm` (thread name) plus minimal scheduling state for every thread
// under /proc/self/task. One line per thread: "tid=<N> name=<str>".
// Signal-safe: only opendir/readdir/open/read/close. Never reads user-space memory.
// Reused from the faulting thread's signal handler; listing /proc is OK from a
// signal context because the kernel serves it from in-kernel structures.
static void writeThreadList(int destFd, off_t* offset) {
    // Poor-man's opendir: read /proc/self/task directly via getdents is messy —
    // use opendir/readdir which are async-signal-safe on bionic (they're thin
    // wrappers around getdents64 + a stack-allocated dirent buffer).
    int taskFd = open("/proc/self/task", O_RDONLY | O_DIRECTORY);
    if (taskFd < 0) return;

    // Use linux_dirent64 layout directly — bionic's opendir allocates, which we
    // want to avoid in the signal path. getdents64 returns a packed array.
    char buf[4096];
    for (;;) {
        long n = syscall(SYS_getdents64, taskFd, buf, sizeof(buf));
        if (n <= 0) break;
        for (long pos = 0; pos < n; ) {
            struct linux_dirent64 {
                ino64_t        d_ino;
                off64_t        d_off;
                unsigned short d_reclen;
                unsigned char  d_type;
                char           d_name[];
            };
            auto* d = (linux_dirent64*)(buf + pos);
            pos += d->d_reclen;
            const char* name = d->d_name;
            // Skip "." / ".."
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
            writeToLog(destFd, offset, "  tid=");
            writeToLog(destFd, offset, name);
            // Read /proc/self/task/<tid>/comm for the thread name
            char commPath[64];
            int cp = 0;
            const char* pre = "/proc/self/task/";
            while (pre[cp]) { commPath[cp] = pre[cp]; cp++; }
            int nlen = 0;
            while (name[nlen] && cp + nlen < 58) {
                commPath[cp + nlen] = name[nlen];
                nlen++;
            }
            const char* suf = "/comm";
            int s = 0;
            while (suf[s] && cp + nlen + s < 63) {
                commPath[cp + nlen + s] = suf[s];
                s++;
            }
            commPath[cp + nlen + s] = 0;
            int commFd = open(commPath, O_RDONLY);
            if (commFd >= 0) {
                char commBuf[24];
                ssize_t cr = read(commFd, commBuf, sizeof(commBuf) - 1);
                close(commFd);
                if (cr > 0) {
                    // strip trailing \n
                    while (cr > 0 && (commBuf[cr-1] == '\n' || commBuf[cr-1] == '\r')) cr--;
                    commBuf[cr] = 0;
                    writeToLog(destFd, offset, " name=");
                    writeToLog(destFd, offset, commBuf);
                }
            }
            writeToLog(destFd, offset, "\n");
        }
    }
    close(taskFd);
}

static void signalHandler(int sig, siginfo_t* info, void* ctx) {
    // Give AirIMEGuard first dibs on SIGSEGV — if we're inside a protected
    // IME native call, it will siglongjmp out and this function never returns
    // here. Falls through if the guard isn't armed or this isn't SIGSEGV.
    if (sig == SIGSEGV && AirIMEGuard_maybeLongjmp && AirIMEGuard_maybeLongjmp(sig, info, ctx)) {
        return; // unreachable — longjmp already happened
    }

    // Same dibs for the Phase 4c walker — if the drain thread is mid-deref
    // inside a guarded read, longjmp out instead of crashing the process.
    if ((sig == SIGSEGV || sig == SIGBUS) &&
        As3WalkerGuard_maybeLongjmp && As3WalkerGuard_maybeLongjmp(sig, info, ctx)) {
        return; // unreachable
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

    // Register dump of the faulting thread — every arm64 general-purpose
    // register, SP, PC, and PSTATE. Unlike the tombstone's minimal dump we
    // include x0-x30 so later Ghidra analysis can correlate argument/return
    // slots, callee-saved regs, and frame layouts at the fault site. All
    // values are stable in the signal context; reading them is free.
    if (ctx) {
        writeToLog(fd, &offset, "---REGISTERS---\n");
        ucontext_t* uc = (ucontext_t*)ctx;
#if defined(__aarch64__)
        for (int i = 0; i < 31; i++) {
            writeToLog(fd, &offset, "x");
            writeDec(fd, &offset, (long)i);
            writeToLog(fd, &offset, "=");
            writeHex(fd, &offset, (uintptr_t)uc->uc_mcontext.regs[i]);
            writeToLog(fd, &offset, (i % 4 == 3) ? "\n" : " ");
        }
        writeToLog(fd, &offset, "sp=");
        writeHex(fd, &offset, (uintptr_t)uc->uc_mcontext.sp);
        writeToLog(fd, &offset, " pc=");
        writeHex(fd, &offset, (uintptr_t)uc->uc_mcontext.pc);
        writeToLog(fd, &offset, " pstate=");
        writeHex(fd, &offset, (uintptr_t)uc->uc_mcontext.pstate);
        writeToLog(fd, &offset, "\n");
#elif defined(__arm__)
        const char* names[] = {
            "r0","r1","r2","r3","r4","r5","r6","r7",
            "r8","r9","r10","fp","ip","sp","lr","pc"
        };
        unsigned long regs[16];
        regs[0]  = uc->uc_mcontext.arm_r0;
        regs[1]  = uc->uc_mcontext.arm_r1;
        regs[2]  = uc->uc_mcontext.arm_r2;
        regs[3]  = uc->uc_mcontext.arm_r3;
        regs[4]  = uc->uc_mcontext.arm_r4;
        regs[5]  = uc->uc_mcontext.arm_r5;
        regs[6]  = uc->uc_mcontext.arm_r6;
        regs[7]  = uc->uc_mcontext.arm_r7;
        regs[8]  = uc->uc_mcontext.arm_r8;
        regs[9]  = uc->uc_mcontext.arm_r9;
        regs[10] = uc->uc_mcontext.arm_r10;
        regs[11] = uc->uc_mcontext.arm_fp;
        regs[12] = uc->uc_mcontext.arm_ip;
        regs[13] = uc->uc_mcontext.arm_sp;
        regs[14] = uc->uc_mcontext.arm_lr;
        regs[15] = uc->uc_mcontext.arm_pc;
        for (int i = 0; i < 16; i++) {
            writeToLog(fd, &offset, names[i]);
            writeToLog(fd, &offset, "=");
            writeHex(fd, &offset, (uintptr_t)regs[i]);
            writeToLog(fd, &offset, (i % 4 == 3) ? "\n" : " ");
        }
        writeToLog(fd, &offset, "cpsr=");
        writeHex(fd, &offset, (uintptr_t)uc->uc_mcontext.arm_cpsr);
        writeToLog(fd, &offset, "\n");
#else
        writeToLog(fd, &offset, "(unsupported arch)\n");
#endif
    }

    // /proc/self/maps — full memory map of every loaded .so with base addresses.
    // ESSENTIAL for symbolication: converts raw PCs from the backtrace into
    // (library_name, offset_within_library) pairs that we can feed into
    // addr2line/Ghidra for every loaded library, not just the stripped ones.
    writeToLog(fd, &offset, "---MAPS---\n");
    writeFileContents(fd, &offset, "/proc/self/maps");

    // /proc/self/status — complete process-level counters (thread count, VmPeak,
    // VmLck, SigQ, etc). Superset of the 30s breadcrumb.
    writeToLog(fd, &offset, "---STATUS---\n");
    writeFileContents(fd, &offset, "/proc/self/status");

    // Thread roster — name + tid for every live thread. Useful when the ANR
    // or crash happens due to contention: we can identify which worker was
    // stuck by name even without a full stack for it.
    writeToLog(fd, &offset, "---THREADS---\n");
    writeThreadList(fd, &offset);

    // Breadcrumb footer — runtime snapshot from the last RuntimeStatsCollector
    // tick (updated every 30s). Attaches thread count + heap pressure at the
    // moment of death. Critical for OOM diagnosis: Play Console SIGABRT tombstones
    // alone don't tell us if the process was near memory limits; these numbers do.
    if (g_bc_time_sec > 0) {
        writeToLog(fd, &offset, "---BREADCRUMB---\n");
        writeToLog(fd, &offset, "snapshot_age_sec=");
        writeDec(fd, &offset, (long)(time(nullptr) - g_bc_time_sec));
        writeToLog(fd, &offset, "\nthreads=");
        writeDec(fd, &offset, (long)g_bc_threads);
        writeToLog(fd, &offset, "\njvm_used_kb=");
        writeDec(fd, &offset, g_bc_jvm_used_kb);
        writeToLog(fd, &offset, "\njvm_max_kb=");
        writeDec(fd, &offset, g_bc_jvm_max_kb);
        writeToLog(fd, &offset, "\nnative_kb=");
        writeDec(fd, &offset, g_bc_native_kb);
        writeToLog(fd, &offset, "\nvmrss_kb=");
        writeDec(fd, &offset, g_bc_vmrss_kb);
        writeToLog(fd, &offset, "\nvmsize_kb=");
        writeDec(fd, &offset, g_bc_vmsize_kb);
        writeToLog(fd, &offset, "\n");
    }

    writeToLog(fd, &offset, "========================\n");

    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }

    // Write the crash marker file. Async-signal-safe: only open/write/close on a
    // pre-computed path. Marks this session as a real native crash for the next
    // boot to find — distinguishes from OS kills / swipe-from-recents which leave
    // SESSION_MARKER but never invoke this handler.
    if (g_crashMarkerPath[0]) {
        int mfd = open(g_crashMarkerPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (mfd >= 0) {
            const char ch = '1';
            write(mfd, &ch, 1);
            fsync(mfd);
            close(mfd);
        }
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

        // Derive crash marker path: <logDir>/.crash_marker
        // Done once at install time so the signal handler only does open/write/close.
        int len = (int)strlen(g_logFilePath);
        int slashPos = len;
        while (slashPos > 0 && g_logFilePath[slashPos - 1] != '/') slashPos--;
        if (slashPos > 0 && slashPos < (int)sizeof(g_crashMarkerPath) - 16) {
            memcpy(g_crashMarkerPath, g_logFilePath, (size_t)slashPos);
            const char* marker = ".crash_marker";
            memcpy(g_crashMarkerPath + slashPos, marker, strlen(marker) + 1);
        }
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

/**
 * Called from RuntimeStatsCollector.tick() on the main looper every 30s.
 * Writes breadcrumb slots that the crash signal handler reads without
 * synchronization — tolerating single-tick skew in exchange for signal
 * safety. No allocations, no logging, ~6 aligned stores. Zero overhead
 * when not called.
 */
JNIEXPORT void JNICALL
Java_br_com_redesurftank_aneawesomeutils_RuntimeStatsCollector_nativeUpdateBreadcrumb(
        JNIEnv* /*env*/, jclass /*clazz*/,
        jint threads, jlong jvmUsedKb, jlong jvmMaxKb,
        jlong nativeKb, jlong vmRssKb, jlong vmSizeKb) {
    g_bc_threads     = threads;
    g_bc_jvm_used_kb = jvmUsedKb;
    g_bc_jvm_max_kb  = jvmMaxKb;
    g_bc_native_kb   = nativeKb;
    g_bc_vmrss_kb    = vmRssKb;
    g_bc_vmsize_kb   = vmSizeKb;
    g_bc_time_sec    = (long)time(nullptr);
}

} // extern "C"
