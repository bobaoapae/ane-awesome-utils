//
// Created by User on 29/10/2024.
//

#include "AneAwesomeUtilsCsharp.h"
#include <iostream>
#include <memory>
#include <string>
#include <Windows.h>
#include <log.h>
#include <unordered_map>
// Use a unique_ptr with a custom deleter to manage the library handle
static std::unique_ptr<std::remove_pointer<HMODULE>::type, decltype(&FreeLibrary)> library(nullptr, FreeLibrary);
static std::unordered_map<std::string, void *> functionCache;

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
    std::string name(functionName);
    auto it = functionCache.find(name);
    if (it != functionCache.end()) {
        return it->second;
    }

    if (!library && !loadNativeLibrary()) {
        return nullptr;
    }

    void *func = GetProcAddress(library.get(), functionName); // Use library.get() instead of *library
    if (!func) {
        std::cerr << "Could not load function: " << GetLastError() << std::endl;
        writeLog("Could not load function");
    } else {
        writeLog(("Function loaded: " + name).c_str());
        functionCache[name] = func;
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

void __cdecl csharpLibrary_awesomeUtils_finalize() {
    writeLog("finalize called");
    using FinalizeFunction = void(__cdecl *)();
    auto func = reinterpret_cast<FinalizeFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_finalize"));

    if (!func) {
        writeLog("Could not load finalize function");
        return;
    }

    func();
    writeLog("finalize completed");
}

DataArray __cdecl csharpLibrary_awesomeUtils_deviceUniqueId() {
    writeLog("deviceUniqueId called");
    using DeviceUniqueIdFunction = DataArray(__cdecl *)();
    auto func = reinterpret_cast<DeviceUniqueIdFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_deviceUniqueId"));

    if (!func) {
        writeLog("Could not load deviceUniqueId function");
        return {};
    }

    DataArray result = func();
    writeLog("deviceUniqueId result: ");
    if (result.DataPointer) {
        std::string resStr(reinterpret_cast<char *>(result.DataPointer), result.Size);
        writeLog(resStr.c_str());
    }
    return result;
}

DataArray __cdecl csharpLibrary_awesomeUtils_loadUrl(const void *urlData, int urlLen, const void *methodData, int methodLen, const void *variablesData, int variablesLen, const void *headersData, int headersLen) {
    writeLog("loadUrl called");
    using LoadUrlFunction = DataArray(__cdecl *)(const void *, int, const void *, int, const void *, int, const void *, int);
    auto func = reinterpret_cast<LoadUrlFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_loadUrl"));

    if (!func) {
        writeLog("Could not load loadUrl function");
        return {};
    }

    DataArray result = func(urlData, urlLen, methodData, methodLen, variablesData, variablesLen, headersData, headersLen);
    writeLog("loadUrl result: ");
    if (result.DataPointer) {
        std::string resStr(reinterpret_cast<char *>(result.DataPointer), result.Size);
        writeLog(resStr.c_str());
    }
    return result;
}

DataArray __cdecl csharpLibrary_awesomeUtils_getLoaderResult(const void *guidData, int guidLen) {
    writeLog("getLoaderResult called");
    using GetLoaderResultFunction = DataArray(__cdecl *)(const void *, int);
    auto func = reinterpret_cast<GetLoaderResultFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_getLoaderResult"));

    if (!func) {
        writeLog("Could not load getLoaderResult function");
        return {};
    }

    DataArray result = func(guidData, guidLen);
    writeLog("getLoaderResult result: ");
    writeLog(std::to_string(result.Size).c_str());
    return result;
}

DataArray __cdecl csharpLibrary_awesomeUtils_createWebSocket() {
    writeLog("createWebSocket called");
    using CreateWebSocketFunction = DataArray(__cdecl *)();
    auto func = reinterpret_cast<CreateWebSocketFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_createWebSocket"));

    if (!func) {
        writeLog("Could not load createWebSocket function");
        return {};
    }

    DataArray result = func();
    writeLog("createWebSocket result: ");
    if (result.DataPointer) {
        std::string resStr(reinterpret_cast<char *>(result.DataPointer), result.Size);
        writeLog(resStr.c_str());
    }
    return result;
}

int __cdecl csharpLibrary_awesomeUtils_connectWebSocket(const void *guidData, int guidLen, const void *uriData, int uriLen, const void *headersData, int headersLen) {
    writeLog("connectWebSocket called");
    using ConnectWebSocketFunction = int(__cdecl *)(const void *, int, const void *, int, const void *, int);
    auto func = reinterpret_cast<ConnectWebSocketFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_connectWebSocket"));

    if (!func) {
        writeLog("Could not load connectWebSocket function");
        return -1;
    }

    int result = func(guidData, guidLen, uriData, uriLen, headersData, headersLen);
    writeLog(("connectWebSocket result: " + std::to_string(result)).c_str());
    return result;
}

int __cdecl csharpLibrary_awesomeUtils_sendWebSocketMessage(const void *guidData, int guidLen, const void *data, int length) {
    writeLog("sendWebSocketMessage called");
    using SendWebSocketMessageFunction = int(__cdecl *)(const void *, int, const void *, int);
    auto func = reinterpret_cast<SendWebSocketMessageFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_sendWebSocketMessage"));

    if (!func) {
        writeLog("Could not load sendWebSocketMessage function");
        return -1;
    }

    int result = func(guidData, guidLen, data, length);
    writeLog(("sendWebSocketMessage result: " + std::to_string(result)).c_str());
    return result;
}

int __cdecl csharpLibrary_awesomeUtils_closeWebSocket(const void *guidData, int guidLen, int closeCode) {
    writeLog("closeWebSocket called");
    using CloseWebSocketFunction = int(__cdecl *)(const void *, int, int);
    auto func = reinterpret_cast<CloseWebSocketFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_closeWebSocket"));

    if (!func) {
        writeLog("Could not load closeWebSocket function");
        return -1;
    }

    int result = func(guidData, guidLen, closeCode);
    writeLog(("closeWebSocket result: " + std::to_string(result)).c_str());
    return result;
}

DataArray __cdecl csharpLibrary_awesomeUtils_getWebSocketMessage(const void *guidData, int guidLen) {
    writeLog("getWebSocketMessage called");
    using GetWebSocketMessageFunction = DataArray(__cdecl *)(const void *, int);
    auto func = reinterpret_cast<GetWebSocketMessageFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_getWebSocketMessage"));

    if (!func) {
        writeLog("Could not load getWebSocketMessage function");
        return {};
    }

    DataArray result = func(guidData, guidLen);
    writeLog("getWebSocketMessage result: ");
    writeLog(std::to_string(result.Size).c_str());
    return result;
}

void __cdecl csharpLibrary_awesomeUtils_addStaticHost(const void *hostData, int hostLen, const void *ipData, int ipLen) {
    writeLog("addStaticHost called");
    using AddStaticHostFunction = void(__cdecl *)(const void *, int, const void *, int);
    auto func = reinterpret_cast<AddStaticHostFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_addStaticHost"));

    if (!func) {
        writeLog("Could not load addStaticHost function");
        return;
    }

    func(hostData, hostLen, ipData, ipLen);
}

void __cdecl csharpLibrary_awesomeUtils_removeStaticHost(const void *hostData, int hostLen) {
    writeLog("removeStaticHost called");
    using RemoveStaticHostFunction = void(__cdecl *)(const void *, int);
    auto func = reinterpret_cast<RemoveStaticHostFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_removeStaticHost"));

    if (!func) {
        writeLog("Could not load removeStaticHost function");
        return;
    }

    func(hostData, hostLen);
}

int __cdecl csharpLibrary_awesomeUtils_isRunningOnEmulator() {
    writeLog("isRunningOnEmulator called");
    using IsRunningOnEmulatorFunction = int(__cdecl *)();
    auto func = reinterpret_cast<IsRunningOnEmulatorFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_isRunningOnEmulator"));

    if (!func) {
        writeLog("Could not load isRunningOnEmulator function");
        return 0;
    }

    int result = func();
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

DataArray __cdecl csharpLibrary_awesomeUtils_readFileToByteArray(const void *pathData, int pathLen) {
    writeLog("readFileToByteArray called");
    using ReadFileToByteArrayFunction = DataArray(__cdecl *)(const void *, int);
    auto func = reinterpret_cast<ReadFileToByteArrayFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_readFileToByteArray"));

    if (!func) {
        writeLog("Could not load readFileToByteArray function");
        return {};
    }

    DataArray result = func(pathData, pathLen);
    writeLog("readFileToByteArray result: ");
    writeLog(std::to_string(result.Size).c_str());
    return result;
}

void * __cdecl csharpLibrary_awesomeUtils_mapXmlToObject(const void *xmlData, int xmlLen, void *freeNewObject, void *freeNewBool, void *freeNewInt, void *freeNewUint, void *freeNewDouble, void *freeNewUtf8, void *freeSetObjProperty) {
    writeLog("mapXmlToObject called");
    using MapXmlToObjectFunction = void*(__cdecl *)(const void *, int, void *, void *, void *, void *, void *, void *, void *);
    auto func = reinterpret_cast<MapXmlToObjectFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_mapXmlToObject"));
    return func ? func(xmlData, xmlLen, freeNewObject, freeNewBool, freeNewInt, freeNewUint, freeNewDouble, freeNewUtf8, freeSetObjProperty) : nullptr;
}

void __cdecl csharpLibrary_awesomeUtils_disposeDataArrayBytes(uint8_t *dataPointer) {
    writeLog("disposeDataArrayBytes called");
    using DisposeDataArrayBytesFunction = void(__cdecl *)(uint8_t *);
    auto func = reinterpret_cast<DisposeDataArrayBytesFunction>(getFunctionPointer("csharpLibrary_awesomeUtils_disposeDataArrayBytes"));

    if (!func) {
        writeLog("Could not load disposeDataArrayBytes function");
        return;
    }

    func(dataPointer);
}
