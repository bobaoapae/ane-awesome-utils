//
//  AneAwesomeUtilsCsharp.h
//  AneAwesomeUtils
//
//  Created by João Vitor Borges on 28/10/24.
//

#ifndef AneAwesomeUtilsCsharp_h
#define AneAwesomeUtilsCsharp_h
extern "C" {
    __cdecl int csharpLibrary_awesomeUtils_initialize(
                                                      const void* urlLoaderSuccessCallBack,
                                                      const void *urlLoaderProgressCallBack,
                                                      const void *urlLoaderFailureCallBack,
                                                      const void *webSocketConnectCallBack,
                                                      const void *webSocketErrorCallBack,
                                                      const void *webSocketDataCallBack,
                                                      const void *writeLogCallBack);
    __cdecl char* csharpLibrary_awesomeUtils_uuid();
    __cdecl char* csharpLibrary_awesomeUtils_deviceUniqueId();
    __cdecl char* csharpLibrary_awesomeUtils_loadUrl(const char* url, const char* method, const char* variables, const char* headers);
    __cdecl char* csharpLibrary_awesomeUtils_createWebSocket();
    __cdecl int csharpLibrary_awesomeUtils_connectWebSocket(const void* guidPointer, const char* host);
    __cdecl int csharpLibrary_awesomeUtils_sendWebSocketMessage(const void* guidPointer, const void* data, int length);
    __cdecl int csharpLibrary_awesomeUtils_closeWebSocket(const void* guidPointer, int closeCode);
    __cdecl void csharpLibrary_awesomeUtils_addStaticHost(const char* host, const char* ip);
    __cdecl void csharpLibrary_awesomeUtils_removeStaticHost(const char* host);
}

#endif /* AneAwesomeUtilsCsharp_h */
