#include "AneAwesomeUtilsCsharp.h"
#include <string>
#include <windows.h>
#include <winternl.h>
#include <FlashRuntimeExtensions.h>
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>
#include <cstring>

#include "log.h"
#include "WindowsFilterInputs.h"

constexpr int EXPORT_FUNCTIONS_COUNT = 22;
static bool alreadyInitialized = false;
static auto exportedFunctions = new FRENamedFunction[EXPORT_FUNCTIONS_COUNT];

static FREContext g_ctx = nullptr;
static std::mutex dispatchMutex;
static std::atomic<bool> g_finalized{false};
static std::atomic<bool> g_inited{false};
static std::atomic<bool> g_subclassed{false};
static std::atomic<bool> g_hooksStarted{false};

static std::string viewToString(const uint8_t *p, uint32_t len) {
    if (!p || len == 0) return {};
    return {reinterpret_cast<const char *>(p), len};
}

static const uint8_t *const EMPTY_CSTR = reinterpret_cast<const uint8_t *>("");

static void dispatchWebSocketEvent(const char *guid, const char *code, const char *level) {
    if (g_finalized.load(std::memory_order_acquire)) return;
    FREContext ctx = g_ctx;
    if (!ctx) return;
    std::string fullCode = std::string("web-socket;") + code + ";" + guid;
    std::lock_guard lock(dispatchMutex);
    FREDispatchStatusEventAsync(ctx, reinterpret_cast<const uint8_t *>(fullCode.c_str()), reinterpret_cast<const uint8_t *>(level));
}

static void dispatchUrlLoaderEvent(const char *guid, const char *code, const char *level) {
    if (g_finalized.load(std::memory_order_acquire)) return;
    FREContext ctx = g_ctx;
    if (!ctx) return;
    std::string fullCode = std::string("url-loader;") + code + ";" + guid;
    std::lock_guard lock(dispatchMutex);
    FREDispatchStatusEventAsync(ctx, reinterpret_cast<const uint8_t *>(fullCode.c_str()), reinterpret_cast<const uint8_t *>(level));
}

static void __cdecl webSocketConnectCallBack(const char *guid, const char *headersEncoded) {
    writeLog("connectCallback called");
    dispatchWebSocketEvent(guid, "connected", headersEncoded);
}

static void __cdecl webSocketDataCallBack(const char *guid) {
    writeLog("dataCallback called");
    dispatchWebSocketEvent(guid, "nextMessage", "");
}

static void __cdecl webSocketErrorCallBack(const char *guid, int closeCode, const char *reason, int responseCode, const char *headersEncoded) {
    writeLog("disconnectCallback called");
    std::string closeCodeReason = std::to_string(closeCode) + ";" + std::string(reason ? reason : "") + ";" + std::to_string(responseCode) + ";" + std::string(headersEncoded ? headersEncoded : "");
    writeLog(closeCodeReason.c_str());
    dispatchWebSocketEvent(guid, "disconnected", closeCodeReason.c_str());
}

static void __cdecl urlLoaderSuccessCallBack(const char *id) {
    writeLog("Calling SuccessCallback");
    std::string id_str(id ? id : "");
    writeLog(("ID: " + id_str).c_str());
    dispatchUrlLoaderEvent(id_str.c_str(), "success", "");
    writeLog("Dispatched success event");
}

static void __cdecl urlLoaderProgressCallBack(const char *id, const char *message) {
    dispatchUrlLoaderEvent(id ? id : "", "progress", message ? message : "");
}

static void __cdecl urlLoaderErrorCallBack(const char *id, const char *message) {
    dispatchUrlLoaderEvent(id ? id : "", "error", message ? message : "");
}

static void writeLogCallback(const char *message) {
    writeLog(message ? message : "");
}

static FREObject awesomeUtils_initialize(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("initialize called");
    if (g_inited.exchange(true, std::memory_order_acq_rel)) {
        FREObject resultBool;
        FRENewObjectFromBool(true, &resultBool);
        return resultBool;
    }
    g_finalized.store(false, std::memory_order_release);
    g_ctx = ctx;

    auto initResult = csharpLibrary_awesomeUtils_initialize(
        (void *) &urlLoaderSuccessCallBack,
        (void *) &urlLoaderProgressCallBack,
        (void *) &urlLoaderErrorCallBack,
        (void *) &webSocketConnectCallBack,
        (void *) &webSocketErrorCallBack,
        (void *) &webSocketDataCallBack,
        (void *) &writeLogCallback
    );

    FREObject resultBool;
    if (FRENewObjectFromBool(initResult == 1, &resultBool) != FRE_OK) {
        writeLog("Failed to create bool object");
        return nullptr;
    }
    return resultBool;
}

static FREObject awesomeUtils_createWebSocket(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("createWebSocket called");
    if (g_finalized.load(std::memory_order_acquire)) return nullptr;
    auto da = csharpLibrary_awesomeUtils_createWebSocket();
    if (da.Size == 0 || da.DataPointer == nullptr) {
        writeLog("createWebSocket returned empty");
        return nullptr;
    }
    auto daDeleter = std::unique_ptr<uint8_t, decltype(&csharpLibrary_awesomeUtils_disposeDataArrayBytes)>(da.DataPointer, &csharpLibrary_awesomeUtils_disposeDataArrayBytes);
    FREObject resultStr;
    if (FRENewObjectFromUTF8(static_cast<uint32_t>(da.Size), da.DataPointer, &resultStr) != FRE_OK) {
        writeLog("Failed to create string object");
        return nullptr;
    }
    return resultStr;
}

static FREObject awesomeUtils_connectWebSocket(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("connectWebSocket called");
    if (g_finalized.load(std::memory_order_acquire)) return nullptr;
    if (argc < 2) return nullptr;

    FREObjectType idType;
    if (FREGetObjectType(argv[0], &idType) != FRE_OK || idType != FRE_TYPE_STRING) {
        writeLog("Invalid id type");
        return nullptr;
    }

    uint32_t idLength = 0;
    const uint8_t *id = EMPTY_CSTR;
    FREGetObjectAsUTF8(argv[0], &idLength, &id);
    if (!id) id = EMPTY_CSTR;

    FREObjectType uriType;
    if (FREGetObjectType(argv[1], &uriType) != FRE_OK || uriType != FRE_TYPE_STRING) {
        writeLog("Invalid uri type");
        return nullptr;
    }

    uint32_t uriLength = 0;
    const uint8_t *uri = EMPTY_CSTR;
    FREGetObjectAsUTF8(argv[1], &uriLength, &uri);
    if (!uri) uri = EMPTY_CSTR;

    uint32_t headersLength = 0;
    const uint8_t *headers = EMPTY_CSTR;
    if (argc > 2) {
        FREObjectType headersType;
        if (FREGetObjectType(argv[2], &headersType) != FRE_OK || headersType != FRE_TYPE_STRING) {
            writeLog("Invalid headers type");
            return nullptr;
        }
        FREGetObjectAsUTF8(argv[2], &headersLength, &headers);
        if (!headers) {
            headers = EMPTY_CSTR;
            headersLength = 0;
        }
    }

    writeLog(("Calling connect to uri: " + viewToString(uri, uriLength)).c_str());

    csharpLibrary_awesomeUtils_connectWebSocket(
        id, static_cast<int>(idLength),
        uri, static_cast<int>(uriLength),
        headers, static_cast<int>(headersLength)
    );

    return nullptr;
}

static FREObject awesomeUtils_closeWebSocket(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("closeWebSocket called");
    if (g_finalized.load(std::memory_order_acquire)) return nullptr;
    if (argc < 1) return nullptr;

    FREObjectType idType;
    if (FREGetObjectType(argv[0], &idType) != FRE_OK || idType != FRE_TYPE_STRING) {
        writeLog("Invalid id type");
        return nullptr;
    }

    uint32_t idLength = 0;
    const uint8_t *id = EMPTY_CSTR;
    FREGetObjectAsUTF8(argv[0], &idLength, &id);
    if (!id) id = EMPTY_CSTR;

    uint32_t closeCode = 1000;
    if (argc > 1) {
        FREObjectType codeType;
        if (FREGetObjectType(argv[1], &codeType) != FRE_OK || codeType != FRE_TYPE_NUMBER) {
            writeLog("Invalid closeCode type");
            return nullptr;
        }
        if (FREGetObjectAsUint32(argv[1], &closeCode) != FRE_OK) {
            writeLog("Failed to get closeCode");
            return nullptr;
        }
    }

    csharpLibrary_awesomeUtils_closeWebSocket(id, static_cast<int>(idLength), static_cast<int>(closeCode));
    return nullptr;
}

static FREObject awesomeUtils_sendWebSocketMessage(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("sendMessageWebSocket called");
    if (g_finalized.load(std::memory_order_acquire)) return nullptr;
    if (argc < 3) return nullptr;

    FREObjectType idType;
    if (FREGetObjectType(argv[0], &idType) != FRE_OK || idType != FRE_TYPE_STRING) {
        writeLog("Invalid id type");
        return nullptr;
    }

    uint32_t idLength = 0;
    const uint8_t *id = EMPTY_CSTR;
    FREGetObjectAsUTF8(argv[0], &idLength, &id);
    if (!id) id = EMPTY_CSTR;

    FREObjectType typeType;
    if (FREGetObjectType(argv[1], &typeType) != FRE_OK || typeType != FRE_TYPE_NUMBER) {
        writeLog("Invalid messageType type");
        return nullptr;
    }

    uint32_t messageType;
    if (FREGetObjectAsUint32(argv[1], &messageType) != FRE_OK) {
        writeLog("Failed to get messageType");
        return nullptr;
    }

    FREObjectType objectType;
    if (FREGetObjectType(argv[2], &objectType) != FRE_OK) {
        writeLog("Failed to get object type");
        return nullptr;
    }

    if (objectType == FRE_TYPE_STRING) {
        writeLog("String message not implemented");
        return nullptr;
    } else if (objectType == FRE_TYPE_BYTEARRAY) {
        FREByteArray byteArray;
        if (FREAcquireByteArray(argv[2], &byteArray) != FRE_OK) {
            writeLog("Failed to acquire byte array");
            return nullptr;
        }

        csharpLibrary_awesomeUtils_sendWebSocketMessage(
            id, static_cast<int>(idLength),
            byteArray.bytes, static_cast<int>(byteArray.length)
        );

        FREReleaseByteArray(argv[2]);
    } else {
        writeLog("Invalid message object type");
        return nullptr;
    }

    return nullptr;
}

static FREObject awesomeUtils_getWebSocketByteArrayMessage(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("getByteArrayMessage called");
    if (g_finalized.load(std::memory_order_acquire)) return nullptr;
    if (argc < 1) return nullptr;

    FREObjectType idType;
    if (FREGetObjectType(argv[0], &idType) != FRE_OK || idType != FRE_TYPE_STRING) {
        writeLog("Invalid id type");
        return nullptr;
    }

    uint32_t idLength = 0;
    const uint8_t *id = EMPTY_CSTR;
    FREGetObjectAsUTF8(argv[0], &idLength, &id);
    if (!id) id = EMPTY_CSTR;

    auto nextMessageResult = csharpLibrary_awesomeUtils_getWebSocketMessage(id, static_cast<int>(idLength));

    if (nextMessageResult.Size == 0 || nextMessageResult.DataPointer == nullptr) {
        writeLog("no messages found");
        return nullptr;
    }

    auto daDeleter = std::unique_ptr<uint8_t, decltype(&csharpLibrary_awesomeUtils_disposeDataArrayBytes)>(nextMessageResult.DataPointer, &csharpLibrary_awesomeUtils_disposeDataArrayBytes);

    FREObject byteArrayObject;
    if (FRENewObject(reinterpret_cast<const uint8_t *>("flash.utils::ByteArray"), 0, nullptr, &byteArrayObject, nullptr) != FRE_OK) {
        writeLog("Failed to create ByteArray");
        return nullptr;
    }

    FREObject length;
    if (FRENewObjectFromUint32(static_cast<uint32_t>(nextMessageResult.Size), &length) != FRE_OK) {
        writeLog("Failed to create length object");
        return nullptr;
    }
    if (FRESetObjectProperty(byteArrayObject, reinterpret_cast<const uint8_t *>("length"), length, nullptr) != FRE_OK) {
        writeLog("Failed to set length property");
        return nullptr;
    }

    FREByteArray ba;
    if (FREAcquireByteArray(byteArrayObject, &ba) != FRE_OK) {
        writeLog("Failed to acquire target byte array");
        return nullptr;
    }
    std::memcpy(ba.bytes, nextMessageResult.DataPointer, nextMessageResult.Size);
    FREReleaseByteArray(byteArrayObject);

    return byteArrayObject;
}

static FREObject awesomeUtils_loadUrl(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("Calling loadUrl");
    if (g_finalized.load(std::memory_order_acquire)) return nullptr;
    if (argc < 4) return nullptr;

    FREObjectType urlType;
    if (FREGetObjectType(argv[0], &urlType) != FRE_OK || urlType != FRE_TYPE_STRING) {
        writeLog("Invalid url type");
        return nullptr;
    }
    FREObjectType methodType;
    if (FREGetObjectType(argv[1], &methodType) != FRE_OK || methodType != FRE_TYPE_STRING) {
        writeLog("Invalid method type");
        return nullptr;
    }
    FREObjectType variableType;
    if (FREGetObjectType(argv[2], &variableType) != FRE_OK || variableType != FRE_TYPE_STRING) {
        writeLog("Invalid variable type");
        return nullptr;
    }
    FREObjectType headersType;
    if (FREGetObjectType(argv[3], &headersType) != FRE_OK || headersType != FRE_TYPE_STRING) {
        writeLog("Invalid headers type");
        return nullptr;
    }

    uint32_t urlLength = 0;
    const uint8_t *url = EMPTY_CSTR;
    FREGetObjectAsUTF8(argv[0], &urlLength, &url);
    if (!url) url = EMPTY_CSTR;
    writeLog(("URL: " + viewToString(url, urlLength)).c_str());

    uint32_t methodLength = 0;
    const uint8_t *method = EMPTY_CSTR;
    FREGetObjectAsUTF8(argv[1], &methodLength, &method);
    if (!method) method = EMPTY_CSTR;
    writeLog(("Method: " + viewToString(method, methodLength)).c_str());

    uint32_t variableLength = 0;
    const uint8_t *variable = EMPTY_CSTR;
    FREGetObjectAsUTF8(argv[2], &variableLength, &variable);
    if (!variable) variable = EMPTY_CSTR;
    writeLog(("Variable: " + viewToString(variable, variableLength)).c_str());

    uint32_t headersLength = 0;
    const uint8_t *headers = EMPTY_CSTR;
    FREGetObjectAsUTF8(argv[3], &headersLength, &headers);
    if (!headers) headers = EMPTY_CSTR;
    writeLog(("Headers: " + viewToString(headers, headersLength)).c_str());

    auto da = csharpLibrary_awesomeUtils_loadUrl(
        url, static_cast<int>(urlLength),
        method, static_cast<int>(methodLength),
        variable, static_cast<int>(variableLength),
        headers, static_cast<int>(headersLength)
    );

    if (da.Size == 0 || da.DataPointer == nullptr) {
        writeLog("startLoader returned empty");
        return nullptr;
    }

    auto daDeleter = std::unique_ptr<uint8_t, decltype(&csharpLibrary_awesomeUtils_disposeDataArrayBytes)>(da.DataPointer, &csharpLibrary_awesomeUtils_disposeDataArrayBytes);

    std::string resultString(reinterpret_cast<char *>(da.DataPointer), static_cast<size_t>(da.Size));
    writeLog(("Result: " + resultString).c_str());

    FREObject resultStr;
    if (FRENewObjectFromUTF8(static_cast<uint32_t>(da.Size), da.DataPointer, &resultStr) != FRE_OK) {
        writeLog("Failed to create string object");
        return nullptr;
    }
    return resultStr;
}

static FREObject awesomeUtils_getLoaderResult(FREContext ctx, void *functionData, uint32_t argc, FREObject argv[]) {
    writeLog("Calling getResult");
    if (g_finalized.load(std::memory_order_acquire)) return nullptr;
    if (argc < 1) return nullptr;

    FREObjectType uuidType;
    if (FREGetObjectType(argv[0], &uuidType) != FRE_OK || uuidType != FRE_TYPE_STRING) {
        writeLog("Invalid uuid type");
        return nullptr;
    }

    uint32_t uuidLength = 0;
    const uint8_t *uuid = EMPTY_CSTR;
    FREGetObjectAsUTF8(argv[0], &uuidLength, &uuid);
    if (!uuid) uuid = EMPTY_CSTR;

    auto result = csharpLibrary_awesomeUtils_getLoaderResult(uuid, static_cast<int>(uuidLength));

    if (result.Size == 0 || result.DataPointer == nullptr) {
        return nullptr;
    }

    auto daDeleter = std::unique_ptr<uint8_t, decltype(&csharpLibrary_awesomeUtils_disposeDataArrayBytes)>(result.DataPointer, &csharpLibrary_awesomeUtils_disposeDataArrayBytes);

    FREObject byteArrayObject;
    if (FRENewObject(reinterpret_cast<const uint8_t *>("flash.utils::ByteArray"), 0, nullptr, &byteArrayObject, nullptr) != FRE_OK) {
        writeLog("Failed to create ByteArray");
        return nullptr;
    }

    FREObject length;
    if (FRENewObjectFromUint32(static_cast<uint32_t>(result.Size), &length) != FRE_OK) {
        writeLog("Failed to create length object");
        return nullptr;
    }
    if (FRESetObjectProperty(byteArrayObject, reinterpret_cast<const uint8_t *>("length"), length, nullptr) != FRE_OK) {
        writeLog("Failed to set length property");
        return nullptr;
    }

    FREByteArray byteArray;
    if (FREAcquireByteArray(byteArrayObject, &byteArray) != FRE_OK) {
        writeLog("Failed to acquire byte array");
        return nullptr;
    }
    std::memcpy(byteArray.bytes, result.DataPointer, result.Size);
    FREReleaseByteArray(byteArrayObject);

    return byteArrayObject;
}

static FREObject awesomeUtils_addStaticHost(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("addStaticHost called");
    if (g_finalized.load(std::memory_order_acquire)) return nullptr;
    if (argc < 2) return nullptr;

    FREObjectType hostType;
    if (FREGetObjectType(argv[0], &hostType) != FRE_OK || hostType != FRE_TYPE_STRING) {
        writeLog("Invalid host type");
        return nullptr;
    }
    FREObjectType ipType;
    if (FREGetObjectType(argv[1], &ipType) != FRE_OK || ipType != FRE_TYPE_STRING) {
        writeLog("Invalid ip type");
        return nullptr;
    }

    uint32_t hostLength = 0;
    const uint8_t *host = EMPTY_CSTR;
    FREGetObjectAsUTF8(argv[0], &hostLength, &host);
    if (!host) host = EMPTY_CSTR;

    uint32_t ipLength = 0;
    const uint8_t *ip = EMPTY_CSTR;
    FREGetObjectAsUTF8(argv[1], &ipLength, &ip);
    if (!ip) ip = EMPTY_CSTR;

    writeLog(("Calling addStaticHost with host: " + viewToString(host, hostLength) + " and ip: " + viewToString(ip, ipLength)).c_str());

    csharpLibrary_awesomeUtils_addStaticHost(host, static_cast<int>(hostLength), ip, static_cast<int>(ipLength));

    return nullptr;
}

static FREObject awesomeUtils_removeStaticHost(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("removeStaticHost called");
    if (g_finalized.load(std::memory_order_acquire)) return nullptr;
    if (argc < 1) return nullptr;

    FREObjectType hostType;
    if (FREGetObjectType(argv[0], &hostType) != FRE_OK || hostType != FRE_TYPE_STRING) {
        writeLog("Invalid host type");
        return nullptr;
    }

    uint32_t hostLength = 0;
    const uint8_t *host = EMPTY_CSTR;
    FREGetObjectAsUTF8(argv[0], &hostLength, &host);
    if (!host) host = EMPTY_CSTR;

    writeLog(("Calling removeStaticHost with host: " + viewToString(host, hostLength)).c_str());

    csharpLibrary_awesomeUtils_removeStaticHost(host, static_cast<int>(hostLength));

    return nullptr;
}

static FREObject awesomeUtils_getDeviceUniqueId(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("getDeviceId called");
    if (g_finalized.load(std::memory_order_acquire)) return nullptr;
    auto da = csharpLibrary_awesomeUtils_deviceUniqueId();
    if (da.Size == 0 || da.DataPointer == nullptr) return nullptr;
    auto daDeleter = std::unique_ptr<uint8_t, decltype(&csharpLibrary_awesomeUtils_disposeDataArrayBytes)>(da.DataPointer, &csharpLibrary_awesomeUtils_disposeDataArrayBytes);
    FREObject resultStr;
    if (FRENewObjectFromUTF8(static_cast<uint32_t>(da.Size), da.DataPointer, &resultStr) != FRE_OK) {
        writeLog("Failed to create string object");
        return nullptr;
    }
    return resultStr;
}

static FREObject awesomeUtils_isRunningOnEmulator(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("isRunningOnEmulator called");
    if (g_finalized.load(std::memory_order_acquire)) return nullptr;
    auto result = csharpLibrary_awesomeUtils_isRunningOnEmulator();
    FREObject resultBool;
    if (FRENewObjectFromBool(result == 1, &resultBool) != FRE_OK) {
        writeLog("Failed to create bool object");
        return nullptr;
    }
    return resultBool;
}

static FREObject awesomeUtils_decompressByteArray(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("decompressByteArray called");
    if (g_finalized.load(std::memory_order_acquire)) return nullptr;
    if (argc < 2) return nullptr;

    FREObjectType inType;
    if (FREGetObjectType(argv[0], &inType) != FRE_OK || inType != FRE_TYPE_BYTEARRAY) {
        writeLog("Invalid input byte array type");
        return nullptr;
    }
    FREObjectType outType;
    if (FREGetObjectType(argv[1], &outType) != FRE_OK || outType != FRE_TYPE_BYTEARRAY) {
        writeLog("Invalid output byte array type");
        return nullptr;
    }

    FREByteArray inBA;
    if (FREAcquireByteArray(argv[0], &inBA) != FRE_OK) {
        writeLog("Failed to acquire input byte array");
        return nullptr;
    }
    auto result = csharpLibrary_awesomeUtils_decompressByteArray(inBA.bytes, static_cast<int>(inBA.length));
    FREReleaseByteArray(argv[0]);

    if (result.Size == 0 || result.DataPointer == nullptr) {
        writeLog("no decompressed data found");
        return nullptr;
    }

    auto daDeleter = std::unique_ptr<uint8_t, decltype(&csharpLibrary_awesomeUtils_disposeDataArrayBytes)>(result.DataPointer, &csharpLibrary_awesomeUtils_disposeDataArrayBytes);

    FREObject length;
    if (FRENewObjectFromUint32(static_cast<uint32_t>(result.Size), &length) != FRE_OK) {
        writeLog("Failed to create length object");
        return nullptr;
    }
    if (FRESetObjectProperty(argv[1], reinterpret_cast<const uint8_t *>("length"), length, nullptr) != FRE_OK) {
        writeLog("Failed to set length property");
        return nullptr;
    }

    FREByteArray targetBA;
    if (FREAcquireByteArray(argv[1], &targetBA) != FRE_OK) {
        writeLog("Failed to acquire target byte array");
        return nullptr;
    }
    std::memcpy(targetBA.bytes, result.DataPointer, result.Size);
    FREReleaseByteArray(argv[1]);

    return nullptr;
}

static FREObject awesomeUtils_readFileToByteArray(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("readFileToByteArray called");
    if (g_finalized.load(std::memory_order_acquire)) return nullptr;
    if (argc < 2) return nullptr;

    FREObjectType pathType;
    if (FREGetObjectType(argv[0], &pathType) != FRE_OK || pathType != FRE_TYPE_STRING) {
        writeLog("Invalid file path type");
        return nullptr;
    }
    FREObjectType outType;
    if (FREGetObjectType(argv[1], &outType) != FRE_OK || outType != FRE_TYPE_BYTEARRAY) {
        writeLog("Invalid output byte array type");
        return nullptr;
    }

    uint32_t filePathLength = 0;
    const uint8_t *filePath = EMPTY_CSTR;
    FREGetObjectAsUTF8(argv[0], &filePathLength, &filePath);
    if (!filePath) filePath = EMPTY_CSTR;

    writeLog(("Calling readFileToByteArray with filePath: " + viewToString(filePath, filePathLength)).c_str());

    auto result = csharpLibrary_awesomeUtils_readFileToByteArray(filePath, static_cast<int>(filePathLength));

    if (result.Size == 0 || result.DataPointer == nullptr) {
        writeLog("no file data found");
        return nullptr;
    }

    auto daDeleter = std::unique_ptr<uint8_t, decltype(&csharpLibrary_awesomeUtils_disposeDataArrayBytes)>(result.DataPointer, &csharpLibrary_awesomeUtils_disposeDataArrayBytes);

    FREObject length;
    if (FRENewObjectFromUint32(static_cast<uint32_t>(result.Size), &length) != FRE_OK) {
        writeLog("Failed to create length object");
        return nullptr;
    }
    if (FRESetObjectProperty(argv[1], reinterpret_cast<const uint8_t *>("length"), length, nullptr) != FRE_OK) {
        writeLog("Failed to set length property");
        return nullptr;
    }

    FREByteArray targetBA;
    if (FREAcquireByteArray(argv[1], &targetBA) != FRE_OK) {
        writeLog("Failed to acquire target byte array");
        return nullptr;
    }
    std::memcpy(targetBA.bytes, result.DataPointer, result.Size);
    FREReleaseByteArray(argv[1]);

    return nullptr;
}

static FREObject awesomeUtils_preventCapture(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    if (g_finalized.load(std::memory_order_acquire)) return nullptr;
    HWND hWnd = GetActiveWindow();
    bool success = false;
    if (hWnd) {
        RTL_OSVERSIONINFOW rovi = {sizeof(rovi)};
        typedef LONG (WINAPI*RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
        auto rtlGetVersion = reinterpret_cast<RtlGetVersionPtr>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        if (rtlGetVersion)
            rtlGetVersion(&rovi);
        DWORD affinity = (rovi.dwMajorVersion == 10 && rovi.dwBuildNumber >= 17134)
                             ? WDA_EXCLUDEFROMCAPTURE
                             : WDA_MONITOR;
        success = SetWindowDisplayAffinity(hWnd, affinity) != FALSE;
    }
    FREObject resultBool;
    if (FRENewObjectFromBool(success, &resultBool) != FRE_OK) {
        writeLog("Failed to create bool object");
        return nullptr;
    }
    return resultBool;
}

static FREObject awesomeUtils_isPreventCaptureEnabled(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    if (g_finalized.load(std::memory_order_acquire)) return nullptr;
    HWND hWnd = GetActiveWindow();
    bool isPrevented = false;
    if (hWnd) {
        DWORD affinity = 0;
        if (GetWindowDisplayAffinity(hWnd, &affinity))
            isPrevented = (affinity & (WDA_MONITOR | WDA_EXCLUDEFROMCAPTURE)) != 0;
    }
    FREObject resultBool;
    if (FRENewObjectFromBool(isPrevented, &resultBool) != FRE_OK) {
        writeLog("Failed to create bool object");
        return nullptr;
    }
    return resultBool;
}

static FREObject awesomeUtils_filterWindowsInputs(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    if (g_finalized.load(std::memory_order_acquire)) return nullptr;
    bool g_filterAll = true;
    std::vector<DWORD> g_filteredKeys;
    if (argc > 0) {
        FREObjectType arrType;
        if (FREGetObjectType(argv[0], &arrType) != FRE_OK || arrType != FRE_TYPE_ARRAY) {
            writeLog("Invalid keys array type");
            return nullptr;
        }

        g_filterAll = false;
        uint32_t arrLen;
        if (FREGetArrayLength(argv[0], &arrLen) != FRE_OK) {
            writeLog("Failed to get array length");
            return nullptr;
        }
        g_filteredKeys.resize(arrLen);
        for (uint32_t i = 0; i < arrLen; ++i) {
            FREObject item;
            if (FREGetArrayElementAt(argv[0], i, &item) != FRE_OK) {
                writeLog("Failed to get array element");
                return nullptr;
            }
            FREObjectType itemType;
            if (FREGetObjectType(item, &itemType) != FRE_OK || itemType != FRE_TYPE_NUMBER) {
                writeLog("Invalid key type in array");
                return nullptr;
            }
            uint32_t key;
            if (FREGetObjectAsUint32(item, &key) != FRE_OK) {
                writeLog("Failed to get key value");
                return nullptr;
            }
            g_filteredKeys[i] = static_cast<DWORD>(key);
        }
    }
    if (!g_hooksStarted.exchange(true, std::memory_order_acq_rel)) {
        StartHooksIfNeeded(g_filterAll, g_filteredKeys);
    }
    FREObject resultBool;
    if (FRENewObjectFromBool(true, &resultBool) != FRE_OK) {
        writeLog("Failed to create bool object");
        return nullptr;
    }
    return resultBool;
}

static FREObject awesomeUtils_stopFilterWindowsInputs(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    if (g_hooksStarted.exchange(false, std::memory_order_acq_rel)) {
        StopHooks();
    }
    FREObject resultBool;
    if (FRENewObjectFromBool(true, &resultBool) != FRE_OK) {
        writeLog("Failed to create bool object");
        return nullptr;
    }
    return resultBool;
}

static FREObject awesomeUtils_blockWindowsLeaveMouseEvent(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    if (g_finalized.load(std::memory_order_acquire)) return nullptr;
    bool was = g_subclassed.exchange(true, std::memory_order_acq_rel);
    if (!was) {
        SubclassMainWindow();
    }
    FREObject resultBool;
    if (FRENewObjectFromBool(true, &resultBool) != FRE_OK) {
        writeLog("Failed to create bool object");
        return nullptr;
    }
    return resultBool;
}

static FREObject awesomeUtils_mapXmlToObject(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    if (g_finalized.load(std::memory_order_acquire)) return nullptr;
    if (argc < 1) return nullptr;

    FREObjectType xmlType;
    if (FREGetObjectType(argv[0], &xmlType) != FRE_OK || xmlType != FRE_TYPE_STRING) {
        writeLog("Invalid xml type");
        return nullptr;
    }

    uint32_t xmlLength = 0;
    const uint8_t *xml = EMPTY_CSTR;
    FREGetObjectAsUTF8(argv[0], &xmlLength, &xml);
    if (!xml) xml = EMPTY_CSTR;

    auto result = csharpLibrary_awesomeUtils_mapXmlToObject(
        xml, static_cast<int>(xmlLength),
        reinterpret_cast<void *>(&FRENewObject),
        reinterpret_cast<void *>(&FRENewObjectFromBool),
        reinterpret_cast<void *>(&FRENewObjectFromInt32),
        reinterpret_cast<void *>(&FRENewObjectFromUint32),
        reinterpret_cast<void *>(&FRENewObjectFromDouble),
        reinterpret_cast<void *>(&FRENewObjectFromUTF8),
        reinterpret_cast<void *>(&FRESetObjectProperty)
    );

    return result;
}

bool IsAddressHooked(uintptr_t address) {
    auto offsetValue = reinterpret_cast<BYTE *>(address);
    BYTE b = *offsetValue;
    return (b == 0xE8 || b == 0xE9 || b == 0x7E || b == 0x74 || b == 0xC3);
}

static FREObject awesomeUtils_isCheatEngineSpeedHackDetected(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    auto getTickCountAddr = reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetTickCount"));
    auto queryPerformanceCounterAddr = reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "QueryPerformanceCounter"));

    auto isTickCountHooked = IsAddressHooked(getTickCountAddr);
    auto isQueryPerformanceCounterHooked = IsAddressHooked(queryPerformanceCounterAddr);

    FREObject resultBool;
    if (FRENewObjectFromBool(isTickCountHooked || isQueryPerformanceCounterHooked, &resultBool) != FRE_OK) {
        writeLog("Failed to create bool object");
        return nullptr;
    }
    return resultBool;
}

typedef NTSTATUS(NTAPI *pdef_NtRaiseHardError)(NTSTATUS ErrorStatus, ULONG NumberOfParameters, ULONG UnicodeStringParameterMask OPTIONAL, PULONG_PTR Parameters, ULONG ResponseOption, PULONG Response);
typedef NTSTATUS(NTAPI *pdef_RtlAdjustPrivilege)(ULONG Privilege, BOOLEAN Enable, BOOLEAN CurrentThread, PBOOLEAN Enabled);

static FREObject awesomeUtils_forceBlueScreenOfDead(FREContext ctx, void* funcData, uint32_t argc, FREObject argv[]) {
    BOOLEAN bEnabled;
    ULONG uResp;
    LPVOID lpFuncAddress = GetProcAddress(LoadLibraryA("ntdll.dll"), "RtlAdjustPrivilege");
    LPVOID lpFuncAddress2 = GetProcAddress(GetModuleHandle("ntdll.dll"), "NtRaiseHardError");
    pdef_RtlAdjustPrivilege NtCall = (pdef_RtlAdjustPrivilege)lpFuncAddress;
    pdef_NtRaiseHardError NtCall2 = (pdef_NtRaiseHardError)lpFuncAddress2;
    NTSTATUS NtRet = NtCall(19, TRUE, FALSE, &bEnabled);
    NtCall2(STATUS_FLOAT_MULTIPLE_FAULTS, 0, 0, 0, 6, &uResp);
    return nullptr;
}

static void AneAwesomeUtilsSupportInitializer(
    void *extData,
    const uint8_t *ctxType,
    FREContext ctx,
    uint32_t *numFunctionsToSet,
    const FRENamedFunction **functionsToSet
) {
    if (!alreadyInitialized) {
        alreadyInitialized = true;
        exportedFunctions[0].name = reinterpret_cast<const uint8_t *>("awesomeUtils_initialize");
        exportedFunctions[0].function = awesomeUtils_initialize;
        exportedFunctions[1].name = reinterpret_cast<const uint8_t *>("awesomeUtils_createWebSocket");
        exportedFunctions[1].function = awesomeUtils_createWebSocket;
        exportedFunctions[2].name = reinterpret_cast<const uint8_t *>("awesomeUtils_sendWebSocketMessage");
        exportedFunctions[2].function = awesomeUtils_sendWebSocketMessage;
        exportedFunctions[3].name = reinterpret_cast<const uint8_t *>("awesomeUtils_closeWebSocket");
        exportedFunctions[3].function = awesomeUtils_closeWebSocket;
        exportedFunctions[4].name = reinterpret_cast<const uint8_t *>("awesomeUtils_connectWebSocket");
        exportedFunctions[4].function = awesomeUtils_connectWebSocket;
        exportedFunctions[5].name = reinterpret_cast<const uint8_t *>("awesomeUtils_addStaticHost");
        exportedFunctions[5].function = awesomeUtils_addStaticHost;
        exportedFunctions[6].name = reinterpret_cast<const uint8_t *>("awesomeUtils_removeStaticHost");
        exportedFunctions[6].function = awesomeUtils_removeStaticHost;
        exportedFunctions[7].name = reinterpret_cast<const uint8_t *>("awesomeUtils_loadUrl");
        exportedFunctions[7].function = awesomeUtils_loadUrl;
        exportedFunctions[8].name = reinterpret_cast<const uint8_t *>("awesomeUtils_getLoaderResult");
        exportedFunctions[8].function = awesomeUtils_getLoaderResult;
        exportedFunctions[9].name = reinterpret_cast<const uint8_t *>("awesomeUtils_getWebSocketByteArrayMessage");
        exportedFunctions[9].function = awesomeUtils_getWebSocketByteArrayMessage;
        exportedFunctions[10].name = reinterpret_cast<const uint8_t *>("awesomeUtils_getDeviceUniqueId");
        exportedFunctions[10].function = awesomeUtils_getDeviceUniqueId;
        exportedFunctions[11].name = reinterpret_cast<const uint8_t *>("awesomeUtils_isRunningOnEmulator");
        exportedFunctions[11].function = awesomeUtils_isRunningOnEmulator;
        exportedFunctions[12].name = reinterpret_cast<const uint8_t *>("awesomeUtils_decompressByteArray");
        exportedFunctions[12].function = awesomeUtils_decompressByteArray;
        exportedFunctions[13].name = reinterpret_cast<const uint8_t *>("awesomeUtils_readFileToByteArray");
        exportedFunctions[13].function = awesomeUtils_readFileToByteArray;
        exportedFunctions[14].name = reinterpret_cast<const uint8_t *>("awesomeUtils_preventCapture");
        exportedFunctions[14].function = awesomeUtils_preventCapture;
        exportedFunctions[15].name = reinterpret_cast<const uint8_t *>("awesomeUtils_isPreventCaptureEnabled");
        exportedFunctions[15].function = awesomeUtils_isPreventCaptureEnabled;
        exportedFunctions[16].name = reinterpret_cast<const uint8_t *>("awesomeUtils_filterWindowsInputs");
        exportedFunctions[16].function = awesomeUtils_filterWindowsInputs;
        exportedFunctions[17].name = reinterpret_cast<const uint8_t *>("awesomeUtils_stopFilterWindowsInputs");
        exportedFunctions[17].function = awesomeUtils_stopFilterWindowsInputs;
        exportedFunctions[18].name = reinterpret_cast<const uint8_t *>("awesomeUtils_blockWindowsLeaveMouseEvent");
        exportedFunctions[18].function = awesomeUtils_blockWindowsLeaveMouseEvent;
        exportedFunctions[19].name = reinterpret_cast<const uint8_t *>("awesomeUtils_mapXmlToObject");
        exportedFunctions[19].function = awesomeUtils_mapXmlToObject;
        exportedFunctions[20].name = reinterpret_cast<const uint8_t *>("awesomeUtils_isCheatEngineSpeedHackDetected");
        exportedFunctions[20].function = awesomeUtils_isCheatEngineSpeedHackDetected;
        exportedFunctions[21].name = reinterpret_cast<const uint8_t *>("awesomeUtils_forceBlueScreenOfDead");
        exportedFunctions[21].function = awesomeUtils_forceBlueScreenOfDead;
    }
    g_ctx = ctx;

    if (numFunctionsToSet) *numFunctionsToSet = EXPORT_FUNCTIONS_COUNT;
    if (functionsToSet) *functionsToSet = exportedFunctions;
}

static void AneAwesomeUtilsSupportFinalizer(FREContext ctx) {
    if (g_hooksStarted.exchange(false, std::memory_order_acq_rel)) {
        StopHooks();
    }
    if (g_subclassed.exchange(false, std::memory_order_acq_rel)) {
        UnsubclassMainWindow();
    }
    g_finalized.store(true, std::memory_order_release);
    g_ctx = nullptr;
    if (g_inited.exchange(false, std::memory_order_acq_rel)) {
        csharpLibrary_awesomeUtils_finalize();
    }
}

extern "C" {
__declspec(dllexport) void InitExtension(void **extDataToSet, FREContextInitializer *ctxInitializerToSet, FREContextFinalizer *ctxFinalizerToSet) {
    writeLog("InitExtension called");
    if (extDataToSet) *extDataToSet = nullptr;
    if (ctxInitializerToSet) *ctxInitializerToSet = AneAwesomeUtilsSupportInitializer;
    if (ctxFinalizerToSet) *ctxFinalizerToSet = AneAwesomeUtilsSupportFinalizer;
    writeLog("InitExtension completed");
}

__declspec(dllexport) void DestroyExtension(void *extData) {
    writeLog("DestroyExtension called");
    if (exportedFunctions) {
        delete[] exportedFunctions;
        exportedFunctions = nullptr;
    }
    closeLog();
}
}
