﻿using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using UrlLoaderNativeLibrary;

namespace AwesomeAneUtils;

public static class ExportFunctions
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void UrlLoaderSuccessCallBackDelegate(IntPtr pointerGuid, IntPtr pointerArray, int length);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void UrlLoaderFailureCallBackDelegate(IntPtr pointerGuid, IntPtr pointerMessage);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void UrlLoaderProgressCallBackDelegate(IntPtr pointerGuid, IntPtr pointerMessage);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void WebSocketConnectCallBackDelegate(IntPtr pointerGuid);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void WebSocketErrorCallBackDelegate(IntPtr pointerGuid, int closeCode, IntPtr pointerMessage);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void WebSocketDataCallBackDelegate(IntPtr pointerGuid, IntPtr pointerArray, int length);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void WriteLogCallBackDelegate(IntPtr pointerMessage);

    private static UrlLoaderSuccessCallBackDelegate _urlLoaderSuccessCallBackDelegate;
    private static UrlLoaderFailureCallBackDelegate _urlLoaderFailureCallBackDelegate;
    private static UrlLoaderProgressCallBackDelegate _urlLoaderProgressCallBackDelegate;
    private static WebSocketConnectCallBackDelegate _webSocketConnectCallBackDelegate;
    private static WebSocketErrorCallBackDelegate _webSocketErrorCallBackDelegate;
    private static WebSocketDataCallBackDelegate _webSocketDataCallBackDelegate;
    private static WriteLogCallBackDelegate _writeLogCallBackDelegate;

    private static Action<string, byte[]> _urlLoaderSuccessWrapper;
    private static Action<string, string> _urlLoaderFailureWrapper;
    private static Action<string, string> _urlLoaderProgressWrapper;
    private static Action<string> _webSocketConnectWrapper;
    private static Action<string, int, string> _webSocketErrorWrapper;
    private static Action<string, byte[]> _webSocketDataWrapper;
    private static Action<string> _writeLogWrapper;

    private static readonly ConcurrentDictionary<Guid, WebSocketClient> WebSocketClients = new();

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_initialize", CallConvs = [typeof(CallConvCdecl)])]
    public static int Initialize(
        IntPtr urlLoaderSuccessCallBack,
        IntPtr urlLoaderProgressCallBack,
        IntPtr urlLoaderFailureCallBack,
        IntPtr webSocketConnectCallBack,
        IntPtr webSocketErrorCallBack,
        IntPtr webSocketDataCallBack,
        IntPtr writeLogCallBack)
    {
        try
        {
            _urlLoaderSuccessCallBackDelegate = Marshal.GetDelegateForFunctionPointer<UrlLoaderSuccessCallBackDelegate>(urlLoaderSuccessCallBack);
            _urlLoaderFailureCallBackDelegate = Marshal.GetDelegateForFunctionPointer<UrlLoaderFailureCallBackDelegate>(urlLoaderProgressCallBack);
            _urlLoaderProgressCallBackDelegate = Marshal.GetDelegateForFunctionPointer<UrlLoaderProgressCallBackDelegate>(urlLoaderFailureCallBack);
            _webSocketConnectCallBackDelegate = Marshal.GetDelegateForFunctionPointer<WebSocketConnectCallBackDelegate>(webSocketConnectCallBack);
            _webSocketErrorCallBackDelegate = Marshal.GetDelegateForFunctionPointer<WebSocketErrorCallBackDelegate>(webSocketErrorCallBack);
            _webSocketDataCallBackDelegate = Marshal.GetDelegateForFunctionPointer<WebSocketDataCallBackDelegate>(webSocketDataCallBack);
            _writeLogCallBackDelegate = Marshal.GetDelegateForFunctionPointer<WriteLogCallBackDelegate>(writeLogCallBack);
        }
        catch
        {
            return -2;
        }

        try
        {
            _writeLogWrapper = (message) =>
            {
                IntPtr ptr1 = Marshal.StringToCoTaskMemAnsi(message);

                _writeLogCallBackDelegate(ptr1);

                Marshal.FreeCoTaskMem(ptr1);
            };

            _urlLoaderSuccessWrapper = (guid, bytes) =>
            {
                IntPtr ptr1 = Marshal.StringToCoTaskMemAnsi(guid);
                IntPtr ptr2 = Marshal.AllocCoTaskMem(bytes.Length);
                Marshal.Copy(bytes, 0, ptr2, bytes.Length);

                _urlLoaderSuccessCallBackDelegate(ptr1, ptr2, bytes.Length);

                Marshal.FreeCoTaskMem(ptr1);
                Marshal.FreeCoTaskMem(ptr2);
            };

            _urlLoaderFailureWrapper = (guid, error) =>
            {
                IntPtr ptr1 = Marshal.StringToCoTaskMemAnsi(guid);
                IntPtr ptr2 = Marshal.StringToCoTaskMemAnsi(error.ToString());

                _urlLoaderFailureCallBackDelegate(ptr1, ptr2);

                Marshal.FreeCoTaskMem(ptr1);
                Marshal.FreeCoTaskMem(ptr2);
            };

            _urlLoaderProgressWrapper = (guid, progress) =>
            {
                IntPtr ptr1 = Marshal.StringToCoTaskMemAnsi(guid);
                IntPtr ptr2 = Marshal.StringToCoTaskMemAnsi(progress.ToString());

                _urlLoaderProgressCallBackDelegate(ptr1, ptr2);

                Marshal.FreeCoTaskMem(ptr1);
                Marshal.FreeCoTaskMem(ptr2);
            };

            _webSocketConnectWrapper = (guid) =>
            {
                IntPtr ptr1 = Marshal.StringToCoTaskMemAnsi(guid);

                _webSocketConnectCallBackDelegate(ptr1);

                Marshal.FreeCoTaskMem(ptr1);
            };

            _webSocketErrorWrapper = (guid, errorCode, error) =>
            {
                IntPtr ptr1 = Marshal.StringToCoTaskMemAnsi(guid);
                IntPtr ptr2 = Marshal.StringToCoTaskMemAnsi(error);

                _webSocketErrorCallBackDelegate(ptr1, errorCode, ptr2);

                Marshal.FreeCoTaskMem(ptr1);
                Marshal.FreeCoTaskMem(ptr2);
            };

            _webSocketDataWrapper = (guid, data) =>
            {
                IntPtr ptr1 = Marshal.StringToCoTaskMemAnsi(guid);
                IntPtr ptr2 = Marshal.AllocCoTaskMem(data.Length);
                Marshal.Copy(data, 0, ptr2, data.Length);

                _webSocketDataCallBackDelegate(ptr1, ptr2, data.Length);

                Marshal.FreeCoTaskMem(ptr1);
                Marshal.FreeCoTaskMem(ptr2);
            };
        }
        catch
        {
            return -3;
        }

        try
        {
            LoaderManager.Instance.Initialize(_urlLoaderSuccessWrapper, _urlLoaderFailureWrapper, _urlLoaderProgressWrapper, _writeLogWrapper);
        }
        catch
        {
            return -4;
        }

        return 1;
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_uuid", CallConvs = [typeof(CallConvCdecl)])]
    public static IntPtr GetUuid()
    {
        var guid = Guid.NewGuid();
        var stringPtr = Marshal.StringToCoTaskMemAnsi(guid.ToString());
        return stringPtr;
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_createWebSocket", CallConvs = [typeof(CallConvCdecl)])]
    public static IntPtr CreateWebSocket()
    {
        var guid = Guid.NewGuid();
        var guidString = guid.ToString();
        var stringPtr = Marshal.StringToCoTaskMemAnsi(guidString);

        var webSocketClient = new WebSocketClient(
            guid,
            () => { _webSocketConnectWrapper(guidString); },
            (data) => { _webSocketDataWrapper(guidString, data.Array); },
            (errorCode, error) =>
            {
                _webSocketErrorWrapper(guidString, errorCode, error);
                Marshal.FreeCoTaskMem(stringPtr);
            },
            _writeLogWrapper);

        if (!WebSocketClients.TryAdd(guid, webSocketClient))
        {
            Marshal.FreeCoTaskMem(stringPtr);
            return IntPtr.Zero;
        }

        return stringPtr;
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_connectWebSocket", CallConvs = [typeof(CallConvCdecl)])]
    public static int ConnectWebSocket(IntPtr guidPointer, IntPtr pointerUri)
    {
        try
        {
            var guidString = Marshal.PtrToStringAnsi(guidPointer);
            if (!Guid.TryParse(guidString, out var guid))
            {
                return 0;
            }

            if (!WebSocketClients.TryGetValue(guid, out var client))
            {
                return 0;
            }

            var uri = Marshal.PtrToStringAnsi(pointerUri);
            client.Connect(uri);
            return 1;
        }
        catch (Exception e)
        {
            LogAll(e, _writeLogWrapper);
            return 0;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_sendWebSocketMessage", CallConvs = [typeof(CallConvCdecl)])]
    public static int SendWebSocketMessage(IntPtr guidPointer, IntPtr pointerData, int length)
    {
        try
        {
            var guidString = Marshal.PtrToStringAnsi(guidPointer);
            if (!Guid.TryParse(guidString, out var guid))
            {
                return 0;
            }

            if (!WebSocketClients.TryGetValue(guid, out var client))
            {
                return 0;
            }

            var data = new byte[length];
            Marshal.Copy(pointerData, data, 0, length);
            client.Send(data);
            return 1;
        }
        catch (Exception e)
        {
            LogAll(e, _writeLogWrapper);
            return 0;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_closeWebSocket", CallConvs = [typeof(CallConvCdecl)])]
    public static int CloseWebSocket(IntPtr guidPointer, int closeCode)
    {
        try
        {
            var guidString = Marshal.PtrToStringAnsi(guidPointer);
            if (!Guid.TryParse(guidString, out var guid))
            {
                return 0;
            }

            if (!WebSocketClients.TryGetValue(guid, out var client))
            {
                return 0;
            }

            client.Disconnect(closeCode);
            client.Dispose();
            return 1;
        }
        catch (Exception e)
        {
            LogAll(e, _writeLogWrapper);
            return 0;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_addStaticHost", CallConvs = [typeof(CallConvCdecl)])]
    public static void AddStaticHost(IntPtr hostPtr, IntPtr ipPtr)
    {
        try
        {
            var host = Marshal.PtrToStringAnsi(hostPtr);
            var ip = Marshal.PtrToStringAnsi(ipPtr);

            DnsInternalResolver.Instance.AddStaticHost(host, ip);
        }
        catch
        {
            // ignored
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_removeStaticHost", CallConvs = [typeof(CallConvCdecl)])]
    public static void RemoveStaticHost(IntPtr hostPtr)
    {
        try
        {
            var host = Marshal.PtrToStringAnsi(hostPtr);

            DnsInternalResolver.Instance.RemoveStaticHost(host);
        }
        catch
        {
            // ignored
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_loadUrl", CallConvs = [typeof(CallConvCdecl)])]
    public static IntPtr LoadUrl(IntPtr urlPtr, IntPtr methodPtr, IntPtr variablesPtr, IntPtr headersPtr)
    {
        try
        {
            var url = Marshal.PtrToStringAnsi(urlPtr);
            var method = Marshal.PtrToStringAnsi(methodPtr);
            var variables = Marshal.PtrToStringAnsi(variablesPtr);
            var headers = Marshal.PtrToStringAnsi(headersPtr);

            var variablesDictionary = string.IsNullOrEmpty(variables) ? new Dictionary<string, string>() : JsonSerializer.Deserialize(variables, JsonDictionaryHeaderContext.Default.DictionaryStringString);
            var headersDictionary = string.IsNullOrEmpty(headers) ? new Dictionary<string, string>() : JsonSerializer.Deserialize(headers, JsonDictionaryHeaderContext.Default.DictionaryStringString);

            var randomId = LoaderManager.Instance.StartLoad(url, method, variablesDictionary, headersDictionary);
            return Marshal.StringToCoTaskMemAnsi(randomId);
        }
        catch
        {
            return IntPtr.Zero;
        }
    }

    private static void LogAll(Exception exception, Action<string> callback)
    {
        if (exception == null)
            return;

        try
        {
            var logBuilder = new System.Text.StringBuilder();

            // Log the main exception
            logBuilder.AppendLine($"Exception: {exception.Message}");
            logBuilder.AppendLine($"Stack Trace: {exception.StackTrace}");

            var inner = exception.InnerException;
            while (inner != null)
            {
                logBuilder.AppendLine($"Inner Exception: {inner.Message}");
                logBuilder.AppendLine($"Inner Stack Trace: {inner.StackTrace}");
                inner = inner.InnerException;
            }

            // Call _writeLog once with the complete log string
            callback(logBuilder.ToString());
        }
        catch (Exception)
        {
            // ignored
        }
    }
}