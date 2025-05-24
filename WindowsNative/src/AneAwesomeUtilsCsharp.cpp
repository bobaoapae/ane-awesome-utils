//
// Created by User on 29/10/2024.
//

#include "AneAwesomeUtilsCsharp.h"
#include <iostream>
#include <memory>
#include <string>
#include <Windows.h>
#include <log.h>
// Use a unique_ptr with a custom deleter to manage the library handle
static std::unique_ptr<std::remove_pointer<HMODULE>::type, decltype(&FreeLibrary)> library(nullptr, FreeLibrary);

std::string GetLibraryLocation(int argc, char *argv[]) {
    std::string baseDirectory;
#if defined(_WIN64)
    const std::string arch = "Windows-x86-64";
#else
    const std::string arch = "Windows-x86";
#endif

    // Check for -extdir argument
    for (int i = 0; i < argc; ++i) {
        if (std::string(argv[i]) == "-extdir" && i + 1 < argc) {
            baseDirectory = argv[i + 1];
            baseDirectory += R"(\br.com.redesurftank.aneawesomeutils.ane)";
            break;
        }
    }

    if (baseDirectory.empty()) {
        char buffer[MAX_PATH];
        DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
        if (length == 0) {
            std::cerr << "Error getting module file name: " << GetLastError() << std::endl;
            return "";
        }
        baseDirectory = std::string(buffer, length);
        baseDirectory = baseDirectory.substr(0, baseDirectory.find_last_of("\\/"));
        baseDirectory += R"(\META-INF\AIR\extensions\br.com.redesurftank.aneawesomeutils)";
    }

    return baseDirectory + R"(\META-INF\ANE\)" + arch + R"(\AwesomeAneUtils.dll)";
}

bool loadNativeLibrary() {
    if (library) {
        std::cerr << "Library is already loaded." << std::endl;
        return true;
    }

    auto libraryPath = GetLibraryLocation(__argc, __argv);
    writeLog(("Loading native library from: " + libraryPath).c_str());

    HMODULE handle = LoadLibraryA(libraryPath.c_str());
    if (!handle) {
        std::cerr << "Could not load library: " << GetLastError() << std::endl;
        writeLog("Could not load library");
        return false;
    }

    library.reset(handle); // Pass handle directly, not &handle
    writeLog("Library loaded successfully");
    return true;
}

void *getFunctionPointer(const char *functionName) {
    if (!library && !loadNativeLibrary()) {
        return nullptr;
    }

    void *func = GetProcAddress(library.get(), functionName); // Use library.get() instead of *library
    if (!func) {
        std::cerr << "Could not load function: " << GetLastError() << std::endl;
        writeLog("Could not load function");
    } else {
        writeLog(("Function loaded: " + std::string(functionName)).c_str());
    }

    return func;
}

int __cdecl csharpLibrary_awesomeUtils_initialize(
    const void *urlLoaderSuccessCallBack,
    const void *urlLoaderProgressCallBack,
    const void *urlLoaderFailureCallBack,
    const void *webSocketConnectCallBack,
    const void *webSocketErrorCallBack,
    const void *webSocketDataCallBack,
    const void *writeLogCallBack) {
    writeLog("initialize called");
    using InitializeFunction = int(__cdecl *)(const void *, const void *, const void *, const void *, const void *, const void *, const void *);
    auto func = reinterpret_cast<InitializeFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_initialize"));

    if (!func) {
        writeLog("Could not load initialize function");
        return -1;
    }

    int result = func(
        urlLoaderSuccessCallBack,
        urlLoaderProgressCallBack,
        urlLoaderFailureCallBack,
        webSocketConnectCallBack,
        webSocketErrorCallBack,
        webSocketDataCallBack,
        writeLogCallBack);

    writeLog(("initialize result: " + std::to_string(result)).c_str());
    return result;
}

char * __cdecl csharpLibrary_awesomeUtils_uuid() {
    writeLog("uuid called");
    using UuidFunction = char *(__cdecl *)();
    auto func = reinterpret_cast<UuidFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_uuid"));

    if (!func) {
        writeLog("Could not load uuid function");
        return nullptr;
    }

    char *result = func();
    writeLog("uuid result: ");
    writeLog(result);
    return result;
}

char * __cdecl csharpLibrary_awesomeUtils_deviceUniqueId() {
    writeLog("deviceUniqueId called");
    using DeviceUniqueIdFunction = char *(__cdecl *)();
    auto func = reinterpret_cast<DeviceUniqueIdFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_deviceUniqueId"));

    if (!func) {
        writeLog("Could not load deviceUniqueId function");
        return nullptr;
    }

    char *result = func();
    writeLog("deviceUniqueId result: ");
    writeLog(result);
    return result;
}

char * __cdecl csharpLibrary_awesomeUtils_loadUrl(const char *url, const char *method, const char *variables, const char *headers) {
    writeLog("loadUrl called");
    using LoadUrlFunction = char *(__cdecl *)(const char *, const char *, const char *, const char *);
    auto func = reinterpret_cast<LoadUrlFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_loadUrl"));

    if (!func) {
        writeLog("Could not load loadUrl function");
        return nullptr;
    }

    char *result = func(url, method, variables, headers);
    writeLog("loadUrl result: ");
    writeLog(result);
    return result;
}

DataArray __cdecl csharpLibrary_awesomeUtils_getLoaderResult(const void *guidPointer) {
    writeLog("getLoaderResult called");
    using GetLoaderResultFunction = DataArray(__cdecl *)(const void *);
    auto func = reinterpret_cast<GetLoaderResultFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_getLoaderResult"));

    if (!func) {
        writeLog("Could not load getLoaderResult function");
        return {};
    }

    DataArray result = func(guidPointer);
    writeLog("getLoaderResult result: ");
    writeLog(std::to_string(result.Size).c_str());
    return result;
}

char * __cdecl csharpLibrary_awesomeUtils_createWebSocket() {
    writeLog("createWebSocket called");
    using CreateWebSocketFunction = char *(__cdecl *)();
    auto func = reinterpret_cast<CreateWebSocketFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_createWebSocket"));

    if (!func) {
        writeLog("Could not load createWebSocket function");
        return nullptr;
    }

    char *result = func();
    writeLog("createWebSocket result: ");
    writeLog(result);
    return result;
}

int __cdecl csharpLibrary_awesomeUtils_connectWebSocket(const void *guidPointer, const char *host, const char *headers) {
    writeLog("connectWebSocket called");
    using ConnectWebSocketFunction = int(__cdecl *)(const void *, const char *, const char *);
    auto func = reinterpret_cast<ConnectWebSocketFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_connectWebSocket"));

    if (!func) {
        writeLog("Could not load connectWebSocket function");
        return -1;
    }

    int result = func(guidPointer, host, headers);
    writeLog(("connectWebSocket result: " + std::to_string(result)).c_str());
    return result;
}

char * __cdecl csharpLibrary_awesomeUtils_getReceivedHeaders(const void *guidPointer) {
    writeLog("getReceivedHeaders called");
    using GetReceivedHeadersFunction = char *(__cdecl *)(const void *);
    auto func = reinterpret_cast<GetReceivedHeadersFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_getReceivedHeaders"));

    if (!func) {
        writeLog("Could not load getReceivedHeaders function");
        return nullptr;
    }

    char *result = func(guidPointer);
    writeLog("getReceivedHeaders result: ");
    writeLog(result);
    return result;
}

int __cdecl csharpLibrary_awesomeUtils_sendWebSocketMessage(const void *guidPointer, const void *data, int length) {
    writeLog("sendWebSocketMessage called");
    using SendWebSocketMessageFunction = int(__cdecl *)(const void *, const void *, int);
    auto func = reinterpret_cast<SendWebSocketMessageFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_sendWebSocketMessage"));

    if (!func) {
        writeLog("Could not load sendWebSocketMessage function");
        return -1;
    }

    int result = func(guidPointer, data, length);
    writeLog(("sendWebSocketMessage result: " + std::to_string(result)).c_str());
    return result;
}

int __cdecl csharpLibrary_awesomeUtils_closeWebSocket(const void *guidPointer, int closeCode) {
    writeLog("closeWebSocket called");
    using CloseWebSocketFunction = int(__cdecl *)(const void *, int);
    auto func = reinterpret_cast<CloseWebSocketFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_closeWebSocket"));

    if (!func) {
        writeLog("Could not load closeWebSocket function");
        return -1;
    }

    int result = func(guidPointer, closeCode);
    writeLog(("closeWebSocket result: " + std::to_string(result)).c_str());
    return result;
}

DataArray __cdecl csharpLibrary_awesomeUtils_getWebSocketMessage(const void *guidPointer) {
    writeLog("getWebSocketMessage called");
    using GetWebSocketMessageFunction = DataArray(__cdecl *)(const void *);
    auto func = reinterpret_cast<GetWebSocketMessageFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_getWebSocketMessage"));

    if (!func) {
        writeLog("Could not load getWebSocketMessage function");
        return {};
    }

    DataArray result = func(guidPointer);
    writeLog("getWebSocketMessage result: ");
    writeLog(std::to_string(result.Size).c_str());
    return result;
}

void __cdecl csharpLibrary_awesomeUtils_addStaticHost(const char *host, const char *ip) {
    writeLog("addStaticHost called");
    using AddStaticHostFunction = void(__cdecl *)(const char *, const char *);
    auto func = reinterpret_cast<AddStaticHostFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_addStaticHost"));

    if (!func) {
        writeLog("Could not load addStaticHost function");
        return;
    }

    func(host, ip);
}

void __cdecl csharpLibrary_awesomeUtils_removeStaticHost(const char *host) {
    writeLog("removeStaticHost called");
    using RemoveStaticHostFunction = void(__cdecl *)(const char *);
    auto func = reinterpret_cast<RemoveStaticHostFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_removeStaticHost"));

    if (!func) {
        writeLog("Could not load removeStaticHost function");
        return;
    }

    func(host);
}

bool __cdecl csharpLibrary_awesomeUtils_isRunningOnEmulator() {
    writeLog("isRunningOnEmulator called");
    using IsRunningOnEmulatorFunction = bool(__cdecl *)();
    auto func = reinterpret_cast<IsRunningOnEmulatorFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_isRunningOnEmulator"));

    if (!func) {
        writeLog("Could not load isRunningOnEmulator function");
        return false;
    }

    bool result = func();
    writeLog(("isRunningOnEmulator result: " + std::to_string(result)).c_str());
    return result;
}

DataArray __cdecl csharpLibrary_awesomeUtils_decompressByteArray(const void *data, int length) {
    writeLog("decompressByteArray called");
    using DecompressByteArrayFunction = DataArray(__cdecl *)(const void *, int);
    auto func = reinterpret_cast<DecompressByteArrayFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_decompressByteArray"));

    if (!func) {
        writeLog("Could not load decompressByteArray function");
        return {};
    }

    DataArray result = func(data, length);
    writeLog("decompressByteArray result: ");
    writeLog(std::to_string(result.Size).c_str());
    return result;
}

DataArray __cdecl csharpLibrary_awesomeUtils_readFileToByteArray(const char *filePath) {
    writeLog("readFileToByteArray called");
    using ReadFileToByteArrayFunction = DataArray(__cdecl *)(const char *);
    auto func = reinterpret_cast<ReadFileToByteArrayFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_readFileToByteArray"));

    if (!func) {
        writeLog("Could not load readFileToByteArray function");
        return {};
    }

    DataArray result = func(filePath);
    writeLog("readFileToByteArray result: ");
    writeLog(std::to_string(result.Size).c_str());
    return result;
}