//
// Created by User on 29/10/2024.
//

#ifndef ANEAWESOMEUTILSCSHARP_H
#define ANEAWESOMEUTILSCSHARP_H
#include "DataArray.h"

extern "C" {
    int __cdecl csharpLibrary_awesomeUtils_initialize(
                                                      const void* urlLoaderSuccessCallBack,
                                                      const void *urlLoaderProgressCallBack,
                                                      const void *urlLoaderFailureCallBack,
                                                      const void *webSocketConnectCallBack,
                                                      const void *webSocketErrorCallBack,
                                                      const void *webSocketDataCallBack,
                                                      const void *writeLogCallBack);
    char* __cdecl csharpLibrary_awesomeUtils_deviceUniqueId();
    char* __cdecl csharpLibrary_awesomeUtils_loadUrl(const char* url, const char* method, const char* variables, const char* headers);
    DataArray __cdecl csharpLibrary_awesomeUtils_getLoaderResult(const void* guidPointer);
    char* __cdecl csharpLibrary_awesomeUtils_createWebSocket();
    int __cdecl csharpLibrary_awesomeUtils_connectWebSocket(const void* guidPointer, const char* host, const char* headers);
    int __cdecl csharpLibrary_awesomeUtils_sendWebSocketMessage(const void* guidPointer, const void* data, int length);
    int __cdecl csharpLibrary_awesomeUtils_closeWebSocket(const void* guidPointer, int closeCode);
    DataArray __cdecl csharpLibrary_awesomeUtils_getWebSocketMessage(const void* guidPointer);
    void __cdecl csharpLibrary_awesomeUtils_addStaticHost(const char* host, const char* ip);
    void __cdecl csharpLibrary_awesomeUtils_removeStaticHost(const char* host);
    bool __cdecl csharpLibrary_awesomeUtils_isRunningOnEmulator();
    DataArray __cdecl csharpLibrary_awesomeUtils_decompressByteArray(const void* data, int length);
    DataArray __cdecl csharpLibrary_awesomeUtils_readFileToByteArray(const char* filePath);
    void* __cdecl csharpLibrary_awesomeUtils_mapXmlToObject(const char* xmlChar, void* freeNewObject, void* freeNewBool, void* freeNewInt, void* freeNewUint, void* freeNewDouble, void* freeNewUtf8, void* freeSetObjProperty);
}
#endif //ANEAWESOMEUTILSCSHARP_H
