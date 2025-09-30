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
    void __cdecl csharpLibrary_awesomeUtils_finalize();
    DataArray __cdecl csharpLibrary_awesomeUtils_deviceUniqueId();
    DataArray __cdecl csharpLibrary_awesomeUtils_loadUrl(const void* url, int urlLen, const void* method, int methodLen, const void* variables, int variablesLen, const void* headers, int headersLen);
    DataArray __cdecl csharpLibrary_awesomeUtils_getLoaderResult(const void* guidPointer, int guidLen);
    DataArray __cdecl csharpLibrary_awesomeUtils_createWebSocket();
    int __cdecl csharpLibrary_awesomeUtils_connectWebSocket(const void* guidPointer, int guidLen, const void* uri, int uriLen, const void* headers, int headersLen);
    int __cdecl csharpLibrary_awesomeUtils_sendWebSocketMessage(const void* guidPointer, int guidLen, const void* data, int length);
    int __cdecl csharpLibrary_awesomeUtils_closeWebSocket(const void* guidPointer, int guidLen, int closeCode);
    DataArray __cdecl csharpLibrary_awesomeUtils_getWebSocketMessage(const void* guidPointer, int guidLen);
    void __cdecl csharpLibrary_awesomeUtils_addStaticHost(const void* host, int hostLen, const void* ip, int ipLen);
    void __cdecl csharpLibrary_awesomeUtils_removeStaticHost(const void* host, int hostLen);
    int __cdecl csharpLibrary_awesomeUtils_isRunningOnEmulator();
    DataArray __cdecl csharpLibrary_awesomeUtils_decompressByteArray(const void* data, int length);
    DataArray __cdecl csharpLibrary_awesomeUtils_readFileToByteArray(const void* filePath, int pathLen);
    void* __cdecl csharpLibrary_awesomeUtils_mapXmlToObject(const void* xmlChar, int xmlLen, void* freeNewObject, void* freeNewBool, void* freeNewInt, void* freeNewUint, void* freeNewDouble, void* freeNewUtf8, void* freeSetObjProperty);
    void __cdecl csharpLibrary_awesomeUtils_disposeDataArrayBytes(uint8_t* dataPointer);
}
#endif //ANEAWESOMEUTILSCSHARP_H
