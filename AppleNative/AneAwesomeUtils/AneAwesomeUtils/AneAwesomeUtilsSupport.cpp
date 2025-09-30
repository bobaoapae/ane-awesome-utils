#include "AneAwesomeUtilsCsharp.h"
#include <queue>
#include <string>
typedef void* NSWindow; // don't need this..
#include <FlashRuntimeExtensions.h>  // Adobe AIR runtime includes
#include "log.hpp"
#include <TargetConditionals.h> // Ensure TARGET_OS_IOS is defined
#if TARGET_OS_IOS
#include "DeviceUtils.h"
#endif

constexpr int EXPORT_FUNCTIONS_COUNT = 15;
static bool alreadyInitialized = false;
static auto exportedFunctions = new FRENamedFunction[EXPORT_FUNCTIONS_COUNT];
static FREContext context;
static std::mutex dispatchMutex;

static void dispatchWebSocketEvent(const char *guid, const char *code, const char *level) {
    std::string fullCode = std::string("web-socket;") + code + ";" + guid;
    std::lock_guard lock(dispatchMutex);
    FREDispatchStatusEventAsync(context, reinterpret_cast<const uint8_t *>(fullCode.c_str()), reinterpret_cast<const uint8_t *>(level));
}

static void dispatchUrlLoaderEvent(const char *guid, const char *code, const char *level) {
    std::string fullCode = std::string("url-loader;") + code + ";" + guid;
    std::lock_guard lock(dispatchMutex);
    FREDispatchStatusEventAsync(context, reinterpret_cast<const uint8_t *>(fullCode.c_str()), reinterpret_cast<const uint8_t *>(level));
}

static void __cdecl webSocketConnectCallBack(char *guid, const char *headersEncoded) {
    writeLog("connectCallback called");
    dispatchWebSocketEvent(guid, "connected", headersEncoded);
}

static void __cdecl webSocketDataCallBack(char *guid) {
    writeLog("dataCallback called");
    dispatchWebSocketEvent(guid, "nextMessage", "");
}

static void __cdecl webSocketErrorCallBack(char *guid, int closeCode, const char *reason, int responseCode, const char *headersEncoded) {
    writeLog("disconnectCallback called");
    std::string closeCodeReason = std::to_string(closeCode) + ";" + std::string(reason) + ";" + std::to_string(responseCode) + ";" + std::string(headersEncoded);
    writeLog(closeCodeReason.c_str());
    dispatchWebSocketEvent(guid, "disconnected", closeCodeReason.c_str());
}

static void __cdecl urlLoaderSuccessCallBack(const char *id) {
    writeLog("Calling SuccessCallback");
    std::string id_str(id);
    writeLog(("ID: " + id_str).c_str());
    dispatchUrlLoaderEvent(id_str.c_str(), "success", "");
    writeLog("Dispatched success event");
}

static void __cdecl urlLoaderProgressCallBack(const char *id, const char *message) {
    dispatchUrlLoaderEvent(id, "progress", message);
}

static void __cdecl urlLoaderErrorCallBack(const char *id, const char *message) {
    dispatchUrlLoaderEvent(id, "error", message);
}

static void writeLogCallback(const char *message) {
    writeLog(message);
}

static FREObject awesomeUtils_initialize(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("initialize called");
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
    auto da = csharpLibrary_awesomeUtils_createWebSocket();
    if (da.Size == 0 || da.DataPointer == nullptr) {
        writeLog("createWebSocket returned empty");
        return nullptr;
    }
    auto daDeleter = std::unique_ptr<uint8_t, decltype(&csharpLibrary_awesomeUtils_disposeDataArrayBytes)>(da.DataPointer, &csharpLibrary_awesomeUtils_disposeDataArrayBytes);
    FREObject resultStr;
    if (FRENewObjectFromUTF8(da.Size, da.DataPointer, &resultStr) != FRE_OK) {
        writeLog("Failed to create string object");
        return nullptr;
    }
    return resultStr;
}

static FREObject awesomeUtils_connectWebSocket(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("connectWebSocket called");
    if (argc < 2) return nullptr;

    FREObjectType idType;
    if (FREGetObjectType(argv[0], &idType) != FRE_OK || idType != FRE_TYPE_STRING) {
        writeLog("Invalid id type");
        return nullptr;
    }

    uint32_t idLength;
    const uint8_t *id;
    FREGetObjectAsUTF8(argv[0], &idLength, &id);

    FREObjectType uriType;
    if (FREGetObjectType(argv[1], &uriType) != FRE_OK || uriType != FRE_TYPE_STRING) {
        writeLog("Invalid uri type");
        return nullptr;
    }

    uint32_t uriLength;
    const uint8_t *uri;
    FREGetObjectAsUTF8(argv[1], &uriLength, &uri);

    uint32_t headersLength = 0;
    const uint8_t *headers = nullptr;
    if (argc > 2) {
        FREObjectType headersType;
        if (FREGetObjectType(argv[2], &headersType) != FRE_OK || headersType != FRE_TYPE_STRING) {
            writeLog("Invalid headers type");
            return nullptr;
        }

        FREGetObjectAsUTF8(argv[2], &headersLength, &headers);
    }

    writeLog("Calling connect to uri: ");
    writeLog(reinterpret_cast<const char *>(uri));

    csharpLibrary_awesomeUtils_connectWebSocket(id, static_cast<int>(idLength), uri, static_cast<int>(uriLength), headers, static_cast<int>(headersLength));

    return nullptr;
}

static FREObject awesomeUtils_closeWebSocket(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("closeWebSocket called");
    if (argc < 1) return nullptr;

    FREObjectType idType;
    if (FREGetObjectType(argv[0], &idType) != FRE_OK || idType != FRE_TYPE_STRING) {
        writeLog("Invalid id type");
        return nullptr;
    }

    uint32_t idLength;
    const uint8_t *id;
    FREGetObjectAsUTF8(argv[0], &idLength, &id);

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
    if (argc < 3) return nullptr;

    FREObjectType idType;
    if (FREGetObjectType(argv[0], &idType) != FRE_OK || idType != FRE_TYPE_STRING) {
        writeLog("Invalid id type");
        return nullptr;
    }

    uint32_t idLength;
    const uint8_t *id;
    FREGetObjectAsUTF8(argv[0], &idLength, &id);

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

        csharpLibrary_awesomeUtils_sendWebSocketMessage(id, static_cast<int>(idLength), byteArray.bytes, static_cast<int>(byteArray.length));

        FREReleaseByteArray(argv[2]);
    } else {
        writeLog("Invalid message object type");
        return nullptr;
    }

    return nullptr;
}

static FREObject awesomeUtils_getWebSocketByteArrayMessage(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("getByteArrayMessage called");
    if (argc < 1) return nullptr;

    FREObjectType idType;
    if (FREGetObjectType(argv[0], &idType) != FRE_OK || idType != FRE_TYPE_STRING) {
        writeLog("Invalid id type");
        return nullptr;
    }

    uint32_t idLength;
    const uint8_t *id;
    FREGetObjectAsUTF8(argv[0], &idLength, &id);

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
    if (FRENewObjectFromUint32(nextMessageResult.Size, &length) != FRE_OK) {
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
    memcpy(ba.bytes, nextMessageResult.DataPointer, nextMessageResult.Size);
    FREReleaseByteArray(byteArrayObject);

    return byteArrayObject;
}

static FREObject awesomeUtils_loadUrl(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("Calling loadUrl");
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

    uint32_t urlLength;
    const uint8_t *url;
    FREGetObjectAsUTF8(argv[0], &urlLength, &url);
    writeLog(("URL: " + std::string(reinterpret_cast<const char *>(url), urlLength)).c_str());

    uint32_t methodLength;
    const uint8_t *method;
    FREGetObjectAsUTF8(argv[1], &methodLength, &method);
    writeLog(("Method: " + std::string(reinterpret_cast<const char *>(method), methodLength)).c_str());

    uint32_t variableLength;
    const uint8_t *variable;
    FREGetObjectAsUTF8(argv[2], &variableLength, &variable);
    writeLog(("Variable: " + std::string(reinterpret_cast<const char *>(variable), variableLength)).c_str());

    uint32_t headersLength;
    const uint8_t *headers;
    FREGetObjectAsUTF8(argv[3], &headersLength, &headers);
    writeLog(("Headers: " + std::string(reinterpret_cast<const char *>(headers), headersLength)).c_str());

    auto da = csharpLibrary_awesomeUtils_loadUrl(url, static_cast<int>(urlLength), method, static_cast<int>(methodLength), variable, static_cast<int>(variableLength), headers, static_cast<int>(headersLength));

    if (da.Size == 0 || da.DataPointer == nullptr) {
        writeLog("startLoader returned empty");
        return nullptr;
    }

    auto daDeleter = std::unique_ptr<uint8_t, decltype(&csharpLibrary_awesomeUtils_disposeDataArrayBytes)>(da.DataPointer, &csharpLibrary_awesomeUtils_disposeDataArrayBytes);

    std::string resultString(reinterpret_cast<char *>(da.DataPointer), da.Size);
    writeLog(("Result: " + resultString).c_str());

    FREObject resultStr;
    if (FRENewObjectFromUTF8(da.Size, da.DataPointer, &resultStr) != FRE_OK) {
        writeLog("Failed to create string object");
        return nullptr;
    }
    return resultStr;
}

static FREObject awesomeUtils_getLoaderResult(FREContext ctx, void *functionData, uint32_t argc, FREObject argv[]) {
    writeLog("Calling getResult");
    if (argc < 1) return nullptr;

    FREObjectType uuidType;
    if (FREGetObjectType(argv[0], &uuidType) != FRE_OK || uuidType != FRE_TYPE_STRING) {
        writeLog("Invalid uuid type");
        return nullptr;
    }

    uint32_t uuidLength;
    const uint8_t *uuid;
    FREGetObjectAsUTF8(argv[0], &uuidLength, &uuid);

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
    if (FRENewObjectFromUint32(result.Size, &length) != FRE_OK) {
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
    memcpy(byteArray.bytes, result.DataPointer, result.Size);
    FREReleaseByteArray(byteArrayObject);

    return byteArrayObject;
}

static FREObject awesomeUtils_addStaticHost(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("addStaticHost called");
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

    uint32_t hostLength;
    const uint8_t *host;
    FREGetObjectAsUTF8(argv[0], &hostLength, &host);

    uint32_t ipLength;
    const uint8_t *ip;
    FREGetObjectAsUTF8(argv[1], &ipLength, &ip);

    writeLog("Calling addStaticHost with host: ");
    writeLog(reinterpret_cast<const char *>(host));
    writeLog(" and ip: ");
    writeLog(reinterpret_cast<const char *>(ip));

    csharpLibrary_awesomeUtils_addStaticHost(host, static_cast<int>(hostLength), ip, static_cast<int>(ipLength));

    return nullptr;
}

static FREObject awesomeUtils_removeStaticHost(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("removeStaticHost called");
    if (argc < 1) return nullptr;

    FREObjectType hostType;
    if (FREGetObjectType(argv[0], &hostType) != FRE_OK || hostType != FRE_TYPE_STRING) {
        writeLog("Invalid host type");
        return nullptr;
    }

    uint32_t hostLength;
    const uint8_t *host;
    FREGetObjectAsUTF8(argv[0], &hostLength, &host);

    writeLog("Calling removeStaticHost with host: ");
    writeLog(reinterpret_cast<const char *>(host));

    csharpLibrary_awesomeUtils_removeStaticHost(host, static_cast<int>(hostLength));

    return nullptr;
}

static FREObject awesomeUtils_getDeviceUniqueId(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("getDeviceId called");
#if TARGET_OS_IOS
    const char* uniqueIdCString = getDeviceUniqueId();
    
    FREObject freUniqueId;
    FRENewObjectFromUTF8((uint32_t)strlen(uniqueIdCString) + 1, (const uint8_t*)uniqueIdCString, &freUniqueId);
    return freUniqueId;
#else
    auto da = csharpLibrary_awesomeUtils_deviceUniqueId();
    if (da.Size == 0 || da.DataPointer == nullptr) return nullptr;
    auto daDeleter = std::unique_ptr<uint8_t, decltype(&csharpLibrary_awesomeUtils_disposeDataArrayBytes)>(da.DataPointer, &csharpLibrary_awesomeUtils_disposeDataArrayBytes);
    FREObject resultStr;
    if (FRENewObjectFromUTF8(da.Size, da.DataPointer, &resultStr) != FRE_OK) {
        writeLog("Failed to create string object");
        return nullptr;
    }
    return resultStr;
#endif
}

static FREObject awesomeUtils_isRunningOnEmulator(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("isRunningOnEmulator called");
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
    if (FRENewObjectFromUint32(result.Size, &length) != FRE_OK) {
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
    memcpy(targetBA.bytes, result.DataPointer, result.Size);
    FREReleaseByteArray(argv[1]);

    return nullptr;
}

static FREObject awesomeUtils_readFileToByteArray(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("readFileToByteArray called");
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

    uint32_t filePathLength;
    const uint8_t *filePath;
    FREGetObjectAsUTF8(argv[0], &filePathLength, &filePath);
    writeLog("Calling readFileToByteArray with filePath: ");
    writeLog(reinterpret_cast<const char *>(filePath));

    auto result = csharpLibrary_awesomeUtils_readFileToByteArray(filePath, static_cast<int>(filePathLength));

    if (result.Size == 0 || result.DataPointer == nullptr) {
        writeLog("no file data found");
        return nullptr;
    }

    auto daDeleter = std::unique_ptr<uint8_t, decltype(&csharpLibrary_awesomeUtils_disposeDataArrayBytes)>(result.DataPointer, &csharpLibrary_awesomeUtils_disposeDataArrayBytes);

    FREObject length;
    if (FRENewObjectFromUint32(result.Size, &length) != FRE_OK) {
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
    memcpy(targetBA.bytes, result.DataPointer, result.Size);
    FREReleaseByteArray(argv[1]);

    return nullptr;
}

static FREObject awesomeUtils_mapXmlToObject(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    if (argc < 1) return nullptr;

    FREObjectType xmlType;
    if (FREGetObjectType(argv[0], &xmlType) != FRE_OK || xmlType != FRE_TYPE_STRING) {
        writeLog("Invalid xml type");
        return nullptr;
    }

    uint32_t xmlLength;
    const uint8_t *xml;
    FREGetObjectAsUTF8(argv[0], &xmlLength, &xml);

    auto result = csharpLibrary_awesomeUtils_mapXmlToObject(xml, static_cast<int>(xmlLength),
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
        exportedFunctions[14].name = reinterpret_cast<const uint8_t *>("awesomeUtils_mapXmlToObject");
        exportedFunctions[14].function = awesomeUtils_mapXmlToObject;
        context = ctx;
    }
    if (numFunctionsToSet) *numFunctionsToSet = EXPORT_FUNCTIONS_COUNT;
    if (functionsToSet) *functionsToSet = exportedFunctions;
}

static void AneAwesomeUtilsSupportFinalizer(FREContext ctx) {
    csharpLibrary_awesomeUtils_finalize();
}

extern "C" {
__attribute__((visibility("default"))) void InitExtension(void **extDataToSet, FREContextInitializer *ctxInitializerToSet, FREContextFinalizer *ctxFinalizerToSet) {
    writeLog("InitExtension called");
    if (extDataToSet) *extDataToSet = nullptr;
    if (ctxInitializerToSet) *ctxInitializerToSet = AneAwesomeUtilsSupportInitializer;
    if (ctxFinalizerToSet) *ctxFinalizerToSet = AneAwesomeUtilsSupportFinalizer;
    writeLog("InitExtension completed");
}

__attribute__((visibility("default"))) void DestroyExtension(void *extData) {
    writeLog("DestroyExtension called");
    delete[] exportedFunctions;
    closeLog();
}
}
