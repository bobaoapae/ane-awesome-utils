#include "AneAwesomeUtilsCsharp.h"
#include <string>
#include <windows.h>
#include <FlashRuntimeExtensions.h>
#include "log.h"

static bool alreadyInitialized = false;
static FRENamedFunction *exportedFunctions = new FRENamedFunction[11];
static FREContext context;

static void dispatchWebSocketEvent(const char *guid, const char *code, const char *level) {
    std::string fullCode = std::string("web-socket;") + code + std::string(";") + guid;
    FREDispatchStatusEventAsync(context, reinterpret_cast<const uint8_t *>(fullCode.c_str()), reinterpret_cast<const uint8_t *>(level));
}

static void dispatchUrlLoaderEvent(const char *guid, const char *code, const char *level) {
    std::string fullCode = std::string("url-loader;") + code + std::string(";") + guid;
    FREDispatchStatusEventAsync(context, reinterpret_cast<const uint8_t *>(fullCode.c_str()), reinterpret_cast<const uint8_t *>(level));
}

static void __cdecl webSocketConnectCallBack(char *guid) {
    writeLog("connectCallback called");
    dispatchWebSocketEvent(guid, "connected", "");
}

static void __cdecl webSocketDataCallBack(char *guid) {
    writeLog("dataCallback called");
    dispatchWebSocketEvent(guid, "nextMessage", "");
}

static void __cdecl webSocketErrorCallBack(char *guid, int closeCode, const char *reason) {
    writeLog("disconnectCallback called");

    auto closeCodeReason = std::to_string(closeCode) + ";" + std::string(reason);

    writeLog(closeCodeReason.c_str());

    dispatchWebSocketEvent(guid, "disconnected", closeCodeReason.c_str());
}

static void __cdecl urlLoaderSuccessCallBack(const char *id) {
    writeLog("Calling SuccessCallback");

    // Prepare data outside the lock to minimize locked duration
    std::string id_str(id);

    writeLog(("ID: " + id_str).c_str());

    dispatchUrlLoaderEvent(id_str.c_str(), "success", "");

    // Notify and log the success event outside the lock
    writeLog("Dispatched success event");
}

static void __cdecl urlLoaderProgressCallBack(const char *id, const char *message) {
    dispatchUrlLoaderEvent(std::string(id).c_str(), "progress", message);
}

static void __cdecl urlLoaderErrorCallBack(const char *id, const char *message) {
    dispatchUrlLoaderEvent(std::string(id).c_str(), "error", message);
}

static void writeLogCallback(const char *message) {
    writeLog(message);
}

// Exported functions:
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
    FRENewObjectFromBool(initResult == 1, &resultBool);
    return resultBool;
}

static FREObject awesomeUtils_createWebSocket(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("createWebSocket called");

    // Capture idWebSocket immediately as a string
    const char *idWebSocketPtr = csharpLibrary_awesomeUtils_createWebSocket();

    FREObject resultStr;
    FRENewObjectFromUTF8(strlen(idWebSocketPtr), reinterpret_cast<const uint8_t *>(idWebSocketPtr), &resultStr);
    return resultStr;
}

static FREObject awesomeUtils_connectWebSocket(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("connectWebSocket called");
    if (argc < 2) return nullptr;

    uint32_t idLength;
    const uint8_t *id;
    FREGetObjectAsUTF8(argv[0], &idLength, &id);
    auto idChar = reinterpret_cast<const char *>(id);

    uint32_t uriLength;
    const uint8_t *uri;
    FREGetObjectAsUTF8(argv[1], &uriLength, &uri);

    auto uriChar = reinterpret_cast<const char *>(uri);

    writeLog("Calling connect to uri: ");
    writeLog(uriChar);

    csharpLibrary_awesomeUtils_connectWebSocket(idChar, uriChar);

    return nullptr;
}

static FREObject awesomeUtils_closeWebSocket(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("closeWebSocket called");
    if (argc < 1) return nullptr;

    uint32_t idLength;
    const uint8_t *id;
    FREGetObjectAsUTF8(argv[0], &idLength, &id);
    auto idChar = reinterpret_cast<const char *>(id);

    uint32_t closeCode = 1000; // Default close code
    FREGetObjectAsUint32(argv[0], &closeCode);

    csharpLibrary_awesomeUtils_closeWebSocket(idChar, static_cast<int>(closeCode));
    return nullptr;
}

static FREObject awesomeUtils_sendWebSocketMessage(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("sendMessageWebSocket called");
    if (argc < 3) return nullptr;

    uint32_t idLength;
    const uint8_t *id;
    FREGetObjectAsUTF8(argv[0], &idLength, &id);
    auto idChar = reinterpret_cast<const char *>(id);

    uint32_t messageType;
    FREGetObjectAsUint32(argv[1], &messageType);

    FREObjectType objectType;
    FREGetObjectType(argv[2], &objectType);

    if (objectType == FRE_TYPE_STRING) {
        //TODO: Implement string message
    } else if (objectType == FRE_TYPE_BYTEARRAY) {
        FREByteArray byteArray;
        FREAcquireByteArray(argv[2], &byteArray);

        csharpLibrary_awesomeUtils_sendWebSocketMessage(idChar, byteArray.bytes, static_cast<int>(byteArray.length));

        FREReleaseByteArray(argv[2]);
    }


    return nullptr;
}

static FREObject awesomeUtils_getWebSocketByteArrayMessage(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("getByteArrayMessage called");

    if (argc < 1) return nullptr;

    uint32_t idLength;
    const uint8_t *id;
    FREGetObjectAsUTF8(argv[0], &idLength, &id);
    auto idChar = reinterpret_cast<const char *>(id);

    auto nextMessageResult = csharpLibrary_awesomeUtils_getWebSocketMessage(idChar);

    if (nextMessageResult.Size == 0) {
        writeLog("no messages found");
        return nullptr;
    }

    FREObject byteArrayObject = nullptr;
    FREByteArray byteArray;
    byteArray.length = nextMessageResult.Size;
    byteArray.bytes = nextMessageResult.DataPointer;

    FRENewByteArray(&byteArray, &byteArrayObject);

    free(nextMessageResult.DataPointer);

    return byteArrayObject;
}

static FREObject awesomeUtils_loadUrl(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("Calling loadUrl");

    uint32_t stringLength;
    const uint8_t *url;
    FREGetObjectAsUTF8(argv[0], &stringLength, &url);
    writeLog(("URL: " + std::string(reinterpret_cast<const char *>(url))).c_str());

    uint32_t methodLength;
    const uint8_t *method;
    FREGetObjectAsUTF8(argv[1], &methodLength, &method);
    writeLog(("Method: " + std::string(reinterpret_cast<const char *>(method))).c_str());

    uint32_t variableLength;
    const uint8_t *variable;
    FREGetObjectAsUTF8(argv[2], &variableLength, &variable);
    writeLog(("Variable: " + std::string(reinterpret_cast<const char *>(variable))).c_str());

    uint32_t headersLength;
    const uint8_t *headers;
    FREGetObjectAsUTF8(argv[3], &headersLength, &headers);
    writeLog(("Headers: " + std::string(reinterpret_cast<const char *>(headers))).c_str());

    char *result = csharpLibrary_awesomeUtils_loadUrl(reinterpret_cast<const char *>(url), reinterpret_cast<const char *>(method), reinterpret_cast<const char *>(variable), reinterpret_cast<const char *>(headers));

    if (!result) {
        writeLog("startLoader returned null");
        return nullptr;
    }

    auto resultString = std::string(result);

    writeLog(("Result: " + resultString).c_str());

    FREObject resultStr;
    FRENewObjectFromUTF8(resultString.length(), reinterpret_cast<const uint8_t *>(resultString.c_str()), &resultStr);
    free(result);
    return resultStr;
}

static FREObject awesomeUtils_getLoaderResult(FREContext ctx, void *functionData, uint32_t argc, FREObject argv[]) {
    writeLog("Calling getResult");

    // Extract UUID from arguments
    uint32_t uuidLength;
    const uint8_t *uuid;
    FREGetObjectAsUTF8(argv[0], &uuidLength, &uuid);
    auto uuidChar = reinterpret_cast<const char *>(uuid);

    auto result = csharpLibrary_awesomeUtils_getLoaderResult(uuidChar);

    // Handle the result outside the lock
    FREObject byteArrayObject = nullptr;
    if (result.Size > 0) {
        FREByteArray byteArray;
        byteArray.length = result.Size;
        byteArray.bytes = result.DataPointer;
        FRENewByteArray(&byteArray, &byteArrayObject);
        free(result.DataPointer);
    }
    return byteArrayObject;
}


static FREObject awesomeUtils_addStaticHost(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("addStaticHost called");
    if (argc < 2) return nullptr;

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

    csharpLibrary_awesomeUtils_addStaticHost(reinterpret_cast<const char *>(host), reinterpret_cast<const char *>(ip));
    return nullptr;
}

static FREObject awesomeUtils_removeStaticHost(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("removeStaticHost called");
    if (argc < 1) return nullptr;

    uint32_t hostLength;
    const uint8_t *host;
    FREGetObjectAsUTF8(argv[0], &hostLength, &host);

    writeLog("Calling removeStaticHost with host: ");
    writeLog(reinterpret_cast<const char *>(host));

    csharpLibrary_awesomeUtils_removeStaticHost(reinterpret_cast<const char *>(host));
    return nullptr;
}

static FREObject awesomeUtils_getDeviceUniqueId(FREContext ctx, void *funcData, uint32_t argc, FREObject argv[]) {
    writeLog("getDeviceId called");
    auto id = csharpLibrary_awesomeUtils_deviceUniqueId();
    FREObject resultStr;
    FRENewObjectFromUTF8(strlen(id), reinterpret_cast<const uint8_t *>(id), &resultStr);
    return resultStr;
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
        exportedFunctions[4].name = (const uint8_t *) "awesomeUtils_connectWebSocket";
        exportedFunctions[4].function = awesomeUtils_connectWebSocket;
        exportedFunctions[5].name = (const uint8_t *) "awesomeUtils_addStaticHost";
        exportedFunctions[5].function = awesomeUtils_addStaticHost;
        exportedFunctions[6].name = (const uint8_t *) "awesomeUtils_removeStaticHost";
        exportedFunctions[6].function = awesomeUtils_removeStaticHost;
        exportedFunctions[7].name = (const uint8_t *) "awesomeUtils_loadUrl";
        exportedFunctions[7].function = awesomeUtils_loadUrl;
        exportedFunctions[8].name = (const uint8_t *) "awesomeUtils_getLoaderResult";
        exportedFunctions[8].function = awesomeUtils_getLoaderResult;
        exportedFunctions[9].name = (const uint8_t *) "awesomeUtils_getWebSocketByteArrayMessage";
        exportedFunctions[9].function = awesomeUtils_getWebSocketByteArrayMessage;
        exportedFunctions[10].name = (const uint8_t *) "awesomeUtils_getDeviceUniqueId";
        exportedFunctions[10].function = awesomeUtils_getDeviceUniqueId;
        context = ctx;
    }
    if (numFunctionsToSet) *numFunctionsToSet = 11;
    if (functionsToSet) *functionsToSet = exportedFunctions;
}

static void AneAwesomeUtilsSupportFinalizer(FREContext ctx) {
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
    closeLog();
}
} // end of extern "C"
