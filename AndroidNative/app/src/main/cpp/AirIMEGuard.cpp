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
static void* g_origGetBefore = nullptr;
static void* g_origGetAfter  = nullptr;

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

    if (!g_origGetBefore) {
        g_origGetBefore = dlsym(RTLD_DEFAULT,
            "Java_com_adobe_air_AndroidInputConnection_nativeGetTextBeforeCursor");
    }
    if (!g_origGetAfter) {
        g_origGetAfter = dlsym(RTLD_DEFAULT,
            "Java_com_adobe_air_AndroidInputConnection_nativeGetTextAfterCursor");
    }
    if (!g_origGetBefore || !g_origGetAfter) {
        __android_log_print(ANDROID_LOG_WARN, TAG,
            "symbol resolve failed (before=%p after=%p) — libCore not loaded yet?",
            g_origGetBefore, g_origGetAfter);
        return JNI_FALSE;
    }

    JNINativeMethod methods[] = {
        {(char*)"nativeGetTextBeforeCursor", (char*)"(I)Ljava/lang/String;", (void*)wrapGetBefore},
        {(char*)"nativeGetTextAfterCursor",  (char*)"(I)Ljava/lang/String;", (void*)wrapGetAfter},
    };
    jint rc = env->RegisterNatives(aicCls, methods, 2);
    if (rc != JNI_OK) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "RegisterNatives failed rc=%d", rc);
        if (env->ExceptionCheck()) env->ExceptionClear();
        return JNI_FALSE;
    }
    __android_log_print(ANDROID_LOG_INFO, TAG,
        "installed (before=%p after=%p)", g_origGetBefore, g_origGetAfter);
    return JNI_TRUE;
}
