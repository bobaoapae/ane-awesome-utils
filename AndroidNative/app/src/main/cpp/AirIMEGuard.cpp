/**
 * AirIMEGuard — UAF recovery guard for AIR's AndroidInputConnection native methods.
 *
 * Background: AIR's Java_com_adobe_air_AndroidInputConnection_nativeGetText{Before,After}Cursor
 * has a known use-after-free race with a C++ backing-store object disposed by
 * the AIR render/UI thread concurrently with IME callbacks on binder threads.
 * The faulting site does two virtual calls through a reloaded pointer without
 * re-validating — if the object is freed between the calls, the second vtable
 * load returns garbage and the subsequent blr jumps to an unmapped address.
 *
 * Fix: we override AIR's JNI bindings via RegisterNatives at runtime. Our
 * wrappers arm a per-thread sigjmp_buf before delegating to AIR's original
 * native via dlsym. If SIGSEGV fires during the call, CrashSignalHandler's
 * pre-hook (AirIMEGuard_maybeLongjmp) siglongjmp's us back to the wrapper,
 * which returns an empty string instead of crashing. If no fault, behavior
 * is identical to the original native.
 *
 * No patches to runtimeClasses.jar or libCore.so required.
 */
#include <jni.h>
#include <setjmp.h>
#include <signal.h>
#include <dlfcn.h>
#include <pthread.h>
#include <android/log.h>
#include <string.h>

#define TAG "AirIMEGuard"

// Frame is stack-allocated in the wrapper and chained via prev for re-entrance
// safety. TLS holds a pointer to the innermost frame for the current thread.
struct GuardFrame {
    sigjmp_buf jb;
    GuardFrame* prev;
};

static pthread_key_t g_tlsKey;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void* g_coreHandle    = nullptr;
static void* g_origGetBefore    = nullptr;
static void* g_origGetAfter     = nullptr;
static void* g_origSetSelection = nullptr;
static void* g_origGetMaxChars  = nullptr;

static void makeKey() {
    pthread_key_create(&g_tlsKey, nullptr);
}

// Called from CrashSignalHandler.cpp at the top of its SIGSEGV handler.
// Returns true if the fault happened inside a guarded wrapper — in which
// case we siglongjmp and never return to the caller.
extern "C" bool AirIMEGuard_maybeLongjmp(int sig, siginfo_t* info, void* ctx) {
    (void)info; (void)ctx;
    if (sig != SIGSEGV) return false;
    GuardFrame* frame = (GuardFrame*)pthread_getspecific(g_tlsKey);
    if (!frame) return false;
    // Pop before longjmp so nested frames remain consistent.
    pthread_setspecific(g_tlsKey, frame->prev);
    siglongjmp(frame->jb, 1);
    // unreachable
    return true;
}

typedef jstring (*GetTextFn)(JNIEnv*, jobject, jint);

static jstring callProtected(JNIEnv* env, jobject self, jint n, GetTextFn orig, const char* label) {
    if (!orig) return env->NewStringUTF("");
    pthread_once(&g_once, makeKey);

    // Validation log — confirms wrapper is actually dispatched by ART (i.e.
    // RegisterNatives targeted the correct classloader). Absence of this line
    // when the user focuses an EditText means the guard is inert and needs
    // the classloader probe revisited.
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "%s dispatched (n=%d)", label, n);

    GuardFrame frame;
    frame.prev = (GuardFrame*)pthread_getspecific(g_tlsKey);
    pthread_setspecific(g_tlsKey, &frame);

    jstring result = nullptr;
    if (sigsetjmp(frame.jb, 1) == 0) {
        result = orig(env, self, n);
    } else {
        // Recovered from SIGSEGV inside orig(). The JNI local frame survives
        // stack unwinding; any pending exception would be from a dead env
        // state, so clear defensively.
        __android_log_print(ANDROID_LOG_WARN, TAG, "recovered SIGSEGV in %s", label);
        if (env->ExceptionCheck()) env->ExceptionClear();
        result = nullptr;
    }

    // Restore prev. If we longjmp'd, AirIMEGuard_maybeLongjmp already did this,
    // so the setspecific here is a redundant but harmless restore to the same prev.
    pthread_setspecific(g_tlsKey, frame.prev);

    if (!result) result = env->NewStringUTF("");
    return result;
}

static jstring JNICALL wrapGetBefore(JNIEnv* env, jobject self, jint n) {
    return callProtected(env, self, n, (GetTextFn)g_origGetBefore, "nativeGetTextBeforeCursor");
}

static jstring JNICALL wrapGetAfter(JNIEnv* env, jobject self, jint n) {
    return callProtected(env, self, n, (GetTextFn)g_origGetAfter, "nativeGetTextAfterCursor");
}

// ── void(II) wrapper: nativeSetSelection(int start, int end) ──
typedef void (*SetSelectionFn)(JNIEnv*, jobject, jint, jint);

static void JNICALL wrapSetSelection(JNIEnv* env, jobject self, jint start, jint end) {
    SetSelectionFn orig = (SetSelectionFn)g_origSetSelection;
    if (!orig) return;
    pthread_once(&g_once, makeKey);

    __android_log_print(ANDROID_LOG_DEBUG, TAG,
        "nativeSetSelection dispatched (start=%d end=%d)", start, end);

    GuardFrame frame;
    frame.prev = (GuardFrame*)pthread_getspecific(g_tlsKey);
    pthread_setspecific(g_tlsKey, &frame);

    if (sigsetjmp(frame.jb, 1) == 0) {
        orig(env, self, start, end);
    } else {
        __android_log_print(ANDROID_LOG_WARN, TAG,
            "recovered SIGSEGV in nativeSetSelection");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    pthread_setspecific(g_tlsKey, frame.prev);
}

// ── int() wrapper: nativeGetTextBoxMaxChars() ──
typedef jint (*GetMaxCharsFn)(JNIEnv*, jobject);

static jint JNICALL wrapGetMaxChars(JNIEnv* env, jobject self) {
    GetMaxCharsFn orig = (GetMaxCharsFn)g_origGetMaxChars;
    if (!orig) return 0;
    pthread_once(&g_once, makeKey);

    __android_log_print(ANDROID_LOG_DEBUG, TAG,
        "nativeGetTextBoxMaxChars dispatched");

    GuardFrame frame;
    frame.prev = (GuardFrame*)pthread_getspecific(g_tlsKey);
    pthread_setspecific(g_tlsKey, &frame);

    jint result = 0;
    if (sigsetjmp(frame.jb, 1) == 0) {
        result = orig(env, self);
    } else {
        __android_log_print(ANDROID_LOG_WARN, TAG,
            "recovered SIGSEGV in nativeGetTextBoxMaxChars");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    pthread_setspecific(g_tlsKey, frame.prev);
    return result;
}

// from CrashSignalHandler.cpp
extern "C" void AneAwesomeUtils_installSignalHandlerIfNeeded();

extern "C" JNIEXPORT jboolean JNICALL
Java_br_com_redesurftank_aneawesomeutils_AirIMEGuard_nativeInstall(
        JNIEnv* env, jclass /*self*/, jclass aicCls) {
    pthread_once(&g_once, makeKey);

    // Make sure CrashSignalHandler's sigaction is live — that handler calls
    // our pre-hook on SEGV. Without this, a raw AIR crash handler (if any)
    // would intercept first and the longjmp never fires.
    AneAwesomeUtils_installSignalHandlerIfNeeded();

    // Android's System.loadLibrary dlopen's with RTLD_LOCAL, so libCore.so's
    // symbols are NOT visible via RTLD_DEFAULT from other .so's. Open a fresh
    // handle — dlopen of an already-loaded library returns the existing handle
    // with the reference count bumped (cheap, safe, idempotent) and lets us
    // dlsym through a specific scope that WILL resolve the JNI entries.
    if (!g_coreHandle) {
        g_coreHandle = dlopen("libCore.so", RTLD_NOW);
        if (!g_coreHandle) {
            __android_log_print(ANDROID_LOG_WARN, TAG,
                "dlopen libCore.so failed: %s — AIR runtime not loaded yet",
                dlerror());
            return JNI_FALSE;
        }
        __android_log_print(ANDROID_LOG_INFO, TAG,
            "dlopen libCore.so handle=%p", g_coreHandle);
    }
    if (!g_origGetBefore) {
        g_origGetBefore = dlsym(g_coreHandle,
            "Java_com_adobe_air_AndroidInputConnection_nativeGetTextBeforeCursor");
    }
    if (!g_origGetAfter) {
        g_origGetAfter = dlsym(g_coreHandle,
            "Java_com_adobe_air_AndroidInputConnection_nativeGetTextAfterCursor");
    }
    if (!g_origSetSelection) {
        g_origSetSelection = dlsym(g_coreHandle,
            "Java_com_adobe_air_AndroidInputConnection_nativeSetSelection");
    }
    if (!g_origGetMaxChars) {
        g_origGetMaxChars = dlsym(g_coreHandle,
            "Java_com_adobe_air_AndroidInputConnection_nativeGetTextBoxMaxChars");
    }
    // before/after are the primary known UAF sites — mandatory. set/maxChars
    // are defensive coverage; if they're missing in this AIR build, skip them
    // but still register the primary pair.
    if (!g_origGetBefore || !g_origGetAfter) {
        __android_log_print(ANDROID_LOG_WARN, TAG,
            "dlsym failed (before=%p after=%p) — libCore handle=%p, symbols missing?",
            g_origGetBefore, g_origGetAfter, g_coreHandle);
        return JNI_FALSE;
    }

    // Build methods array dynamically so a missing optional symbol doesn't
    // skip the mandatory ones. RegisterNatives is all-or-nothing per call,
    // and we want best-effort coverage.
    JNINativeMethod methods[4];
    int methodCount = 0;
    methods[methodCount++] = {(char*)"nativeGetTextBeforeCursor",
        (char*)"(I)Ljava/lang/String;", (void*)wrapGetBefore};
    methods[methodCount++] = {(char*)"nativeGetTextAfterCursor",
        (char*)"(I)Ljava/lang/String;", (void*)wrapGetAfter};
    if (g_origSetSelection) {
        methods[methodCount++] = {(char*)"nativeSetSelection",
            (char*)"(II)V", (void*)wrapSetSelection};
    }
    if (g_origGetMaxChars) {
        methods[methodCount++] = {(char*)"nativeGetTextBoxMaxChars",
            (char*)"()I", (void*)wrapGetMaxChars};
    }
    jint rc = env->RegisterNatives(aicCls, methods, methodCount);
    if (rc != JNI_OK) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "RegisterNatives failed rc=%d", rc);
        if (env->ExceptionCheck()) env->ExceptionClear();
        return JNI_FALSE;
    }
    __android_log_print(ANDROID_LOG_INFO, TAG,
        "installed %d methods (before=%p after=%p setSel=%p maxChars=%p)",
        methodCount, g_origGetBefore, g_origGetAfter, g_origSetSelection, g_origGetMaxChars);
    return JNI_TRUE;
}
