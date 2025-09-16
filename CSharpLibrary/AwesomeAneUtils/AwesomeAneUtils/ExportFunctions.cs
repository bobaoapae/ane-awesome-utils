using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Xml.Linq;

namespace AwesomeAneUtils;

public static class ExportFunctions
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void UrlLoaderSuccessCallBackDelegate(IntPtr pointerGuid);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void UrlLoaderFailureCallBackDelegate(IntPtr pointerGuid, IntPtr pointerMessage);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void UrlLoaderProgressCallBackDelegate(IntPtr pointerGuid, IntPtr pointerMessage);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void WebSocketConnectCallBackDelegate(IntPtr pointerGuid, IntPtr pointerHeaders);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void WebSocketErrorCallBackDelegate(IntPtr pointerGuid, int closeCode, IntPtr pointerMessage, int responseCode, IntPtr pointerHeadersEncoded);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void WebSocketDataCallBackDelegate(IntPtr pointerGuid);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void WriteLogCallBackDelegate(IntPtr pointerMessage);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate uint FRENewObjectDelegate(IntPtr className, uint argc, IntPtr argv, out IntPtr obj, out IntPtr exception);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate uint FRENewObjectFromBoolDelegate(uint value, out IntPtr obj);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate uint FRENewObjectFromInt32Delegate(int value, out IntPtr obj);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate uint FRENewObjectFromUint32Delegate(uint value, out IntPtr obj);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate uint FRENewObjectFromDoubleDelegate(double value, out IntPtr obj);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate uint FRENewObjectFromUTF8Delegate(uint length, IntPtr value, out IntPtr obj);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate uint FRESetObjectPropertyDelegate(IntPtr obj, IntPtr propName, IntPtr propValue, out IntPtr exception);

    private static UrlLoaderSuccessCallBackDelegate _urlLoaderSuccessCallBackDelegate;
    private static UrlLoaderFailureCallBackDelegate _urlLoaderFailureCallBackDelegate;
    private static UrlLoaderProgressCallBackDelegate _urlLoaderProgressCallBackDelegate;
    private static WebSocketConnectCallBackDelegate _webSocketConnectCallBackDelegate;
    private static WebSocketErrorCallBackDelegate _webSocketErrorCallBackDelegate;
    private static WebSocketDataCallBackDelegate _webSocketDataCallBackDelegate;
    private static WriteLogCallBackDelegate _writeLogCallBackDelegate;

    private static Action<string> _urlLoaderSuccessWrapper;
    private static Action<string, string> _urlLoaderFailureWrapper;
    private static Action<string, string> _urlLoaderProgressWrapper;
    private static Action<string, string> _webSocketConnectWrapper;
    private static Action<string, int, string, int, string> _webSocketErrorWrapper;
    private static Action<string> _webSocketDataWrapper;
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
            _urlLoaderProgressCallBackDelegate = Marshal.GetDelegateForFunctionPointer<UrlLoaderProgressCallBackDelegate>(urlLoaderProgressCallBack);
            _urlLoaderFailureCallBackDelegate = Marshal.GetDelegateForFunctionPointer<UrlLoaderFailureCallBackDelegate>(urlLoaderFailureCallBack);
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
            _writeLogWrapper = message =>
            {
                IntPtr ptr1 = Marshal.StringToCoTaskMemAnsi(message);

                _writeLogCallBackDelegate(ptr1);

                SafeFreeCoTaskMem(ptr1);
            };

            _urlLoaderSuccessWrapper = (guid) =>
            {
                IntPtr ptr1 = Marshal.StringToCoTaskMemAnsi(guid);

                _urlLoaderSuccessCallBackDelegate(ptr1);

                SafeFreeCoTaskMem(ptr1);
            };

            _urlLoaderFailureWrapper = (guid, error) =>
            {
                IntPtr ptr1 = Marshal.StringToCoTaskMemAnsi(guid);
                IntPtr ptr2 = Marshal.StringToCoTaskMemAnsi(error.ToString());

                _urlLoaderFailureCallBackDelegate(ptr1, ptr2);

                SafeFreeCoTaskMem(ptr1);
                SafeFreeCoTaskMem(ptr2);
            };

            _urlLoaderProgressWrapper = (guid, progress) =>
            {
                IntPtr ptr1 = Marshal.StringToCoTaskMemAnsi(guid);
                IntPtr ptr2 = Marshal.StringToCoTaskMemAnsi(progress.ToString());

                _urlLoaderProgressCallBackDelegate(ptr1, ptr2);

                SafeFreeCoTaskMem(ptr1);
                SafeFreeCoTaskMem(ptr2);
            };

            _webSocketConnectWrapper = (guid, headers) =>
            {
                IntPtr ptr1 = Marshal.StringToCoTaskMemAnsi(guid);
                IntPtr ptr2 = Marshal.StringToCoTaskMemAnsi(headers);

                _webSocketConnectCallBackDelegate(ptr1, ptr2);

                SafeFreeCoTaskMem(ptr1);
                SafeFreeCoTaskMem(ptr2);
            };

            _webSocketErrorWrapper = (guid, errorCode, error, responseCode, headersEncoded) =>
            {
                IntPtr ptr1 = Marshal.StringToCoTaskMemAnsi(guid);
                IntPtr ptr2 = Marshal.StringToCoTaskMemAnsi(error);
                IntPtr ptr3 = Marshal.StringToCoTaskMemAnsi(headersEncoded);

                _webSocketErrorCallBackDelegate(ptr1, errorCode, ptr2, responseCode, ptr3);

                SafeFreeCoTaskMem(ptr1);
                SafeFreeCoTaskMem(ptr2);
                SafeFreeCoTaskMem(ptr3);
            };

            _webSocketDataWrapper = (guid) =>
            {
                IntPtr ptr1 = Marshal.StringToCoTaskMemAnsi(guid);

                _webSocketDataCallBackDelegate(ptr1);

                SafeFreeCoTaskMem(ptr1);
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
        var alreadyDispatchError = false;
        var lockError = new Lock();

        var webSocketClient = new WebSocketClient(
            (responseHeaders) =>
            {
                var headersEncoded64 = Convert.ToBase64String(Encoding.UTF8.GetBytes(JsonSerializer.Serialize(responseHeaders, JsonDictionaryHeaderContext.Default.DictionaryStringString)));
                _webSocketConnectWrapper(guidString, headersEncoded64);
            },
            () => { _webSocketDataWrapper(guidString); },
            (errorCode, error, responseCode, responseHeaders) =>
            {
                using (lockError.EnterScope())
                {
                    if (alreadyDispatchError)
                        return;
                    var headersEncoded64 = Convert.ToBase64String(Encoding.UTF8.GetBytes(JsonSerializer.Serialize(responseHeaders, JsonDictionaryHeaderContext.Default.DictionaryStringString)));
                    _webSocketErrorWrapper(guidString, errorCode, error, responseCode, headersEncoded64);
                    SafeFreeCoTaskMem(stringPtr);
                    if (WebSocketClients.TryRemove(guid, out var removed))
                        removed.Dispose();
                    alreadyDispatchError = true;
                }
            },
            _writeLogWrapper);

        if (!WebSocketClients.TryAdd(guid, webSocketClient))
        {
            webSocketClient.Dispose();
            SafeFreeCoTaskMem(stringPtr);
            return IntPtr.Zero;
        }

        return stringPtr;
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_connectWebSocket", CallConvs = [typeof(CallConvCdecl)])]
    public static int ConnectWebSocket(IntPtr guidPointer, IntPtr pointerUri, IntPtr pointerHeaders)
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

            var headers = Marshal.PtrToStringAnsi(pointerHeaders);
            var headersDictionary = string.IsNullOrEmpty(headers) ? new Dictionary<string, string>() : JsonSerializer.Deserialize(headers, JsonDictionaryHeaderContext.Default.DictionaryStringString);

            client.Connect(uri, headersDictionary);
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
            return 1;
        }
        catch (Exception e)
        {
            LogAll(e, _writeLogWrapper);
            return 0;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_getWebSocketMessage", CallConvs = [typeof(CallConvCdecl)])]
    public static DataArray GetWebSocketMessage(IntPtr guidPointer)
    {
        var result = new DataArray();

        try
        {
            var guidString = Marshal.PtrToStringAnsi(guidPointer);

            if (!Guid.TryParse(guidString, out var guid))
            {
                return result;
            }

            if (!WebSocketClients.TryGetValue(guid, out var client))
            {
                return result;
            }

            if (!client.TryGetNextMessage(out var message))
            {
                return result;
            }

            result.Size = message.Length;

            result.DataPointer = Marshal.AllocHGlobal(result.Size);
            Marshal.Copy(message, 0, result.DataPointer, result.Size);

            return result;
        }
        catch (Exception e)
        {
            LogAll(e, _writeLogWrapper);
            return result;
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

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_getLoaderResult", CallConvs = [typeof(CallConvCdecl)])]
    public static DataArray GetLoaderResult(IntPtr guidPointer)
    {
        var result = new DataArray();

        try
        {
            var guidString = Marshal.PtrToStringAnsi(guidPointer);

            if (!Guid.TryParse(guidString, out var guid))
            {
                return result;
            }

            if (!LoaderManager.Instance.TryGetResult(guid, out var data))
            {
                return result;
            }

            result.Size = data.Length;

            result.DataPointer = Marshal.AllocHGlobal(result.Size);
            Marshal.Copy(data, 0, result.DataPointer, result.Size);

            return result;
        }
        catch (Exception e)
        {
            LogAll(e, _writeLogWrapper);
            return result;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_deviceUniqueId", CallConvs = [typeof(CallConvCdecl)])]
    public static IntPtr get_deviceUniqueId()
    {
        try
        {
            return Marshal.StringToHGlobalAnsi(HardwareID.GetDeviceUniqueIdHash(e => LogAll(e, _writeLogWrapper)));
        }
        catch
        {
            return IntPtr.Zero;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_isRunningOnEmulator", CallConvs = [typeof(CallConvCdecl)])]
    public static bool IsRunningOnEmulator()
    {
        try
        {
            var isEmulator = VMDetector.IsRunningInVM();
            return isEmulator;
        }
        catch
        {
            return false;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_decompressByteArray", CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe DataArray DecompressByteArray(IntPtr data, int length)
    {
        var result = new DataArray();

        try
        {
            // cria um stream que lê direto de data
            using var srcStream = new UnmanagedMemoryStream((byte*)data.ToPointer(), length);
            // chama a nova sobrecarga
            using var ms = CompressUtil.Uncompress(srcStream);

            result.Size = (int)ms.Length;
            result.DataPointer = Marshal.AllocHGlobal(result.Size);
            // copiar do MemoryStream para unmanaged
            var buffer = ms.GetBuffer(); // não aloca: retorna o array interno
            Marshal.Copy(buffer, 0, result.DataPointer, result.Size);

            return result;
        }
        catch (Exception e)
        {
            LogAll(e, _writeLogWrapper);
            return result;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_readFileToByteArray", CallConvs = [typeof(CallConvCdecl)])]
    public static DataArray ReadFileToByteArray(IntPtr path)
    {
        var result = new DataArray();

        try
        {
            var filePath = Marshal.PtrToStringUTF8(path);
            if (string.IsNullOrEmpty(filePath))
                return result;

            if (!File.Exists(filePath))
                return result;

            using var fs = new FileStream(filePath, FileMode.Open, FileAccess.Read);
            using var ms = new MemoryStream();
            fs.CopyTo(ms);

            result.Size = (int)ms.Length;
            result.DataPointer = Marshal.AllocHGlobal(result.Size);
            // copiar do MemoryStream para unmanaged
            var buffer = ms.GetBuffer(); // não aloca: retorna o array interno
            Marshal.Copy(buffer, 0, result.DataPointer, result.Size);

            return result;
        }
        catch (Exception e)
        {
            LogAll(e, _writeLogWrapper);
            return result;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_mapXmlToObject", CallConvs = new[] { typeof(CallConvCdecl) })]
    public static IntPtr MapXmlToObject(
        IntPtr xmlPtr, 
        IntPtr pFRENewObject, 
        IntPtr pFRENewObjectFromBool, 
        IntPtr pFRENewObjectFromInt32, 
        IntPtr pFRENewObjectFromUint32, 
        IntPtr pFRENewObjectFromDouble, 
        IntPtr pFRENewObjectFromUTF8,
        IntPtr pFRESetObjectProperty)
    {
        var result = IntPtr.Zero;
        try
        {
            var xml = Marshal.PtrToStringUTF8(xmlPtr);
            if (string.IsNullOrEmpty(xml)) return result;

            var doc = XDocument.Parse(xml);
            var root = doc.Root;

            var freNewObject = Marshal.GetDelegateForFunctionPointer<FRENewObjectDelegate>(pFRENewObject);
            var freNewFromBool = Marshal.GetDelegateForFunctionPointer<FRENewObjectFromBoolDelegate>(pFRENewObjectFromBool);
            var freNewFromInt32 = Marshal.GetDelegateForFunctionPointer<FRENewObjectFromInt32Delegate>(pFRENewObjectFromInt32);
            var freNewFromUint32 = Marshal.GetDelegateForFunctionPointer<FRENewObjectFromUint32Delegate>(pFRENewObjectFromUint32);
            var freNewFromDouble = Marshal.GetDelegateForFunctionPointer<FRENewObjectFromDoubleDelegate>(pFRENewObjectFromDouble);
            var freNewFromUtf8 = Marshal.GetDelegateForFunctionPointer<FRENewObjectFromUTF8Delegate>(pFRENewObjectFromUTF8);
            var freSetProp = Marshal.GetDelegateForFunctionPointer<FRESetObjectPropertyDelegate>(pFRESetObjectProperty);

            IntPtr Convert(XElement elem)
            {
                if (!elem.HasElements && !elem.HasAttributes)
                {
                    return ValueToFre(elem.Value.Trim(), freNewFromBool, freNewFromInt32, freNewFromUint32, freNewFromDouble, freNewFromUtf8);
                }

                var classPtr = Utf8Ptr("Object");
                freNewObject(classPtr, 0, IntPtr.Zero, out var obj, out _);
                Marshal.FreeHGlobal(classPtr);
                if (obj == IntPtr.Zero) return IntPtr.Zero;

                foreach (var attr in elem.Attributes())
                {
                    var namePtr = Utf8Ptr(attr.Name.LocalName);
                    var valObj = ValueToFre(attr.Value, freNewFromBool, freNewFromInt32, freNewFromUint32, freNewFromDouble, freNewFromUtf8);
                    freSetProp(obj, namePtr, valObj, out _);
                    Marshal.FreeHGlobal(namePtr);
                }

                var groups = elem.Elements().GroupBy(e => e.Name.LocalName);
                foreach (var g in groups)
                {
                    var propPtr = Utf8Ptr(g.Key);
                    IntPtr propVal;
                    if (g.Count() == 1)
                    {
                        propVal = Convert(g.First());
                    }
                    else
                    {
                        var arrClassPtr = Utf8Ptr("Array");
                        IntPtr arr;
                        freNewObject(arrClassPtr, 0, IntPtr.Zero, out arr, out _);
                        Marshal.FreeHGlobal(arrClassPtr);
                        if (arr == IntPtr.Zero) continue;

                        uint idx = 0;
                        foreach (var child in g)
                        {
                            var childObj = Convert(child);
                            var idxPtr = Utf8Ptr(idx.ToString());
                            freSetProp(arr, idxPtr, childObj, out _);
                            Marshal.FreeHGlobal(idxPtr);
                            idx++;
                        }

                        propVal = arr;
                    }

                    freSetProp(obj, propPtr, propVal, out _);
                    Marshal.FreeHGlobal(propPtr);
                }

                return obj;
            }

            result = Convert(root);
            return result;
        }
        catch (Exception e)
        {
            LogAll(e, _writeLogWrapper); // Assuming log function
            return result;
        }
    }

    private static IntPtr ValueToFre(string val, FRENewObjectFromBoolDelegate fromBool, FRENewObjectFromInt32Delegate fromInt, FRENewObjectFromUint32Delegate fromUint, FRENewObjectFromDoubleDelegate fromDouble,
        FRENewObjectFromUTF8Delegate fromUtf8)
    {
        if (bool.TryParse(val, out var b))
        {
            IntPtr obj;
            fromBool((uint)(b ? 1 : 0), out obj);
            return obj;
        }

        if (int.TryParse(val, out var i))
        {
            IntPtr obj;
            fromInt(i, out obj);
            return obj;
        }

        if (uint.TryParse(val, out var u))
        {
            IntPtr obj;
            fromUint(u, out obj);
            return obj;
        }

        if (double.TryParse(val, out var d))
        {
            IntPtr obj;
            fromDouble(d, out obj);
            return obj;
        }

        var bytes = Encoding.UTF8.GetBytes(val!);
        var totalLength = bytes.Length + 1;
        var buf = Marshal.AllocHGlobal(totalLength);
        Marshal.Copy(bytes, 0, buf, bytes.Length);
        Marshal.WriteByte(buf + bytes.Length, 0);
        fromUtf8((uint)totalLength, buf, out var strObj);
        Marshal.FreeHGlobal(buf);
        return strObj;
    }

    private static IntPtr Utf8Ptr(string s)
    {
        var bytes = Encoding.UTF8.GetBytes(s + '\0');
        var ptr = Marshal.AllocHGlobal(bytes.Length);
        Marshal.Copy(bytes, 0, ptr, bytes.Length);
        return ptr;
    }

    private static void LogAll(Exception exception, Action<string> callback)
    {
        if (exception == null)
            return;

        try
        {
            var logBuilder = new StringBuilder();

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
    private static void LogAll(string exception, Action<string> callback)
    {
        if (exception == null)
            return;

        try
        {
            // Call _writeLog once with the complete log string
            callback(exception);
        }
        catch (Exception)
        {
            // ignored
        }
    }

    private static void SafeFreeCoTaskMem(IntPtr ptr)
    {
        try
        {
            Marshal.FreeCoTaskMem(ptr);
        }
        catch
        {
            //ignored
        }
    }
}