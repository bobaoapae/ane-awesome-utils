//
// Created by User on 08/09/2025.
//
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>
#include <algorithm>
#include "WindowsFilterInputs.h"

#define LLKHF_INJECTED 0x00000010
#define LLKHF_LOWER_IL_INJECTED 0x00000002
#define LLMHF_INJECTED 0x00000001
#define LLMHF_LOWER_IL_INJECTED 0x00000002

#define GWLP_WNDPROC -4
#define WM_MOUSELEAVE 0x02A3

typedef enum _AVRT_PRIORITY { AVRT_PRIORITY_LOW=-2, AVRT_PRIORITY_NORMAL=0, AVRT_PRIORITY_HIGH=1, AVRT_PRIORITY_CRITICAL=2 } AVRT_PRIORITY;
typedef HANDLE (WINAPI* PFN_AvSetMmThreadCharacteristicsW)(LPCWSTR, LPDWORD);
typedef BOOL   (WINAPI* PFN_AvSetMmThreadPriority)(HANDLE, AVRT_PRIORITY);
typedef BOOL   (WINAPI* PFN_AvRevertMmThreadCharacteristics)(HANDLE);

static PFN_AvSetMmThreadCharacteristicsW pAvSetMmThreadCharacteristicsW = nullptr;
static PFN_AvSetMmThreadPriority pAvSetMmThreadPriority = nullptr;
static PFN_AvRevertMmThreadCharacteristics pAvRevertMmThreadCharacteristics = nullptr;

static HHOOK g_keyboardHook = NULL;
static HHOOK g_mouseHook = NULL;

static HANDLE g_kbThreadHandle = NULL;
static HANDLE g_msThreadHandle = NULL;
static DWORD  g_kbThreadId = 0;
static DWORD  g_msThreadId = 0;

static volatile LONG g_stopHooks = 0;

static WNDPROC oldProc = nullptr;

static std::vector<DWORD> g_filteredKeys;
static bool g_filterAll = true;

LRESULT CALLBACK SubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_MOUSELEAVE) return 0;
    return CallWindowProc(oldProc, hWnd, msg, wParam, lParam);
}

static void LoadMmcss() {
    static HMODULE h = LoadLibraryW(L"Avrt.dll");
    if (!h) return;
    if (!pAvSetMmThreadCharacteristicsW) pAvSetMmThreadCharacteristicsW = (PFN_AvSetMmThreadCharacteristicsW)GetProcAddress(h, "AvSetMmThreadCharacteristicsW");
    if (!pAvSetMmThreadPriority) pAvSetMmThreadPriority = (PFN_AvSetMmThreadPriority)GetProcAddress(h, "AvSetMmThreadPriority");
    if (!pAvRevertMmThreadCharacteristics) pAvRevertMmThreadCharacteristics = (PFN_AvRevertMmThreadCharacteristics)GetProcAddress(h, "AvRevertMmThreadCharacteristics");
}

static void BoostThisThread() {
    LoadMmcss();
    HANDLE hTask = NULL;
    DWORD taskIdx = 0;
    if (pAvSetMmThreadCharacteristicsW) {
        hTask = pAvSetMmThreadCharacteristicsW(L"Games", &taskIdx);
        if (hTask && pAvSetMmThreadPriority) pAvSetMmThreadPriority(hTask, AVRT_PRIORITY_CRITICAL);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        bool isInjected = (kb->flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED)) != 0;
        bool isFilteredKey = std::find(g_filteredKeys.begin(), g_filteredKeys.end(), kb->vkCode) != g_filteredKeys.end();
        if (isInjected && (g_filterAll || isFilteredKey)) return 1;
    }
    return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
}

LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        if (ms->flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) return 1;
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

static DWORD WINAPI KeyboardHookThread(LPVOID) {
    BoostThisThread();
    HINSTANCE hInstance = GetModuleHandleW(nullptr);
    g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, hInstance, 0);
    MSG msg;
    while (InterlockedCompareExchange(&g_stopHooks, 0, 0) == 0) {
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
    }
    if (g_keyboardHook) { UnhookWindowsHookEx(g_keyboardHook); g_keyboardHook = NULL; }
    return 0;
}

static DWORD WINAPI MouseHookThread(LPVOID) {
    BoostThisThread();
    HINSTANCE hInstance = GetModuleHandleW(nullptr);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, hInstance, 0);
    MSG msg;
    while (InterlockedCompareExchange(&g_stopHooks, 0, 0) == 0) {
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
    }
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = NULL; }
    return 0;
}

void StartHooksIfNeeded(bool filterAll, const std::vector<DWORD> &filteredKeys) {
    g_filterAll = filterAll;
    g_filteredKeys = filteredKeys;
    if (g_kbThreadHandle && g_msThreadHandle) return;
    InterlockedExchange(&g_stopHooks, 0);
    g_kbThreadHandle = CreateThread(nullptr, 0, KeyboardHookThread, nullptr, 0, &g_kbThreadId);
    g_msThreadHandle = CreateThread(nullptr, 0, MouseHookThread, nullptr, 0, &g_msThreadId);
}

void StopHooks() {
    if (!g_kbThreadHandle && !g_msThreadHandle) return;
    InterlockedExchange(&g_stopHooks, 1);
    if (g_kbThreadId) PostThreadMessageW(g_kbThreadId, WM_QUIT, 0, 0);
    if (g_msThreadId) PostThreadMessageW(g_msThreadId, WM_QUIT, 0, 0);
    if (g_kbThreadHandle) { WaitForSingleObject(g_kbThreadHandle, INFINITE); CloseHandle(g_kbThreadHandle); g_kbThreadHandle = NULL; g_kbThreadId = 0; }
    if (g_msThreadHandle) { WaitForSingleObject(g_msThreadHandle, INFINITE); CloseHandle(g_msThreadHandle); g_msThreadHandle = NULL; g_msThreadId = 0; }
    g_filteredKeys.clear();
    g_filterAll = true;
}

void SubclassMainWindow() {
    HWND hwnd = FindWindowA("ApolloRuntimeContentWindow", NULL);
    if (hwnd && !oldProc) {
        oldProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)SubclassProc);
    }
}

void UnsubclassMainWindow() {
    HWND hwnd = FindWindowA("ApolloRuntimeContentWindow", NULL);
    if (hwnd && oldProc) {
        SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)oldProc);
        oldProc = nullptr;
    }
}