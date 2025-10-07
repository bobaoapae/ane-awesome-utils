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

    // Flag de descarte: 0 = ativo, 1 = disposed
    private static int _isDisposed;

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static bool IsDisposed() => Volatile.Read(ref _isDisposed) != 0;

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static void MarkDisposed() => Interlocked.Exchange(ref _isDisposed, 1);

    // ====== SafeInvoke overloads por tipo de delegate (corrige o erro de conversão para Action<...>) ======
    private static void SafeInvoke(WriteLogCallBackDelegate invoker, IntPtr p1)
    {
        if (IsDisposed() || invoker == null) return;
        try { invoker(p1); } catch { }
    }
    private static void SafeInvoke(UrlLoaderSuccessCallBackDelegate invoker, IntPtr p1)
    {
        if (IsDisposed() || invoker == null) return;
        try { invoker(p1); } catch { }
    }
    private static void SafeInvoke(UrlLoaderFailureCallBackDelegate invoker, IntPtr p1, IntPtr p2)
    {
        if (IsDisposed() || invoker == null) return;
        try { invoker(p1, p2); } catch { }
    }
    private static void SafeInvoke(UrlLoaderProgressCallBackDelegate invoker, IntPtr p1, IntPtr p2)
    {
        if (IsDisposed() || invoker == null) return;
        try { invoker(p1, p2); } catch { }
    }
    private static void SafeInvoke(WebSocketConnectCallBackDelegate invoker, IntPtr p1, IntPtr p2)
    {
        if (IsDisposed() || invoker == null) return;
        try { invoker(p1, p2); } catch { }
    }
    private static void SafeInvoke(WebSocketErrorCallBackDelegate invoker, IntPtr a, int b, IntPtr c, int d, IntPtr e)
    {
        if (IsDisposed() || invoker == null) return;
        try { invoker(a, b, c, d, e); } catch { }
    }
    private static void SafeInvoke(WebSocketDataCallBackDelegate invoker, IntPtr p1)
    {
        if (IsDisposed() || invoker == null) return;
        try { invoker(p1); } catch { }
    }

    private static void ClearCallbacks()
    {
        _urlLoaderSuccessCallBackDelegate = null;
        _urlLoaderFailureCallBackDelegate = null;
        _urlLoaderProgressCallBackDelegate = null;
        _webSocketConnectCallBackDelegate = null;
        _webSocketErrorCallBackDelegate = null;
        _webSocketDataCallBackDelegate = null;
        _writeLogCallBackDelegate = null;

        _urlLoaderSuccessWrapper = null;
        _urlLoaderFailureWrapper = null;
        _urlLoaderProgressWrapper = null;
        _webSocketConnectWrapper = null;
        _webSocketErrorWrapper = null;
        _webSocketDataWrapper = null;
        _writeLogWrapper = null;
    }

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
                if (IsDisposed() || _writeLogCallBackDelegate == null) return;
                IntPtr ptr = IntPtr.Zero;
                try { ptr = Utf8Alloc(message); SafeInvoke(_writeLogCallBackDelegate, ptr); }
                finally { SafeFreeHGlobal(ptr); }
            };

            _urlLoaderSuccessWrapper = guid =>
            {
                if (IsDisposed() || _urlLoaderSuccessCallBackDelegate == null || string.IsNullOrEmpty(guid)) return;
                IntPtr p = IntPtr.Zero;
                try { p = Utf8Alloc(guid); SafeInvoke(_urlLoaderSuccessCallBackDelegate, p); }
                finally { SafeFreeHGlobal(p); }
            };

            _urlLoaderFailureWrapper = (guid, error) =>
            {
                if (IsDisposed() || _urlLoaderFailureCallBackDelegate == null || string.IsNullOrEmpty(guid)) return;
                IntPtr p1 = IntPtr.Zero, p2 = IntPtr.Zero;
                try { p1 = Utf8Alloc(guid); p2 = Utf8Alloc(error ?? string.Empty); SafeInvoke(_urlLoaderFailureCallBackDelegate, p1, p2); }
                finally { SafeFreeHGlobal(p1); SafeFreeHGlobal(p2); }
            };

            _urlLoaderProgressWrapper = (guid, progress) =>
            {
                if (IsDisposed() || _urlLoaderProgressCallBackDelegate == null || string.IsNullOrEmpty(guid)) return;
                IntPtr p1 = IntPtr.Zero, p2 = IntPtr.Zero;
                try { p1 = Utf8Alloc(guid); p2 = Utf8Alloc(progress ?? string.Empty); SafeInvoke(_urlLoaderProgressCallBackDelegate, p1, p2); }
                finally { SafeFreeHGlobal(p1); SafeFreeHGlobal(p2); }
            };

            _webSocketConnectWrapper = (guid, headers) =>
            {
                if (IsDisposed() || _webSocketConnectCallBackDelegate == null || string.IsNullOrEmpty(guid)) return;
                IntPtr p1 = IntPtr.Zero, p2 = IntPtr.Zero;
                try { p1 = Utf8Alloc(guid); p2 = Utf8Alloc(headers ?? string.Empty); SafeInvoke(_webSocketConnectCallBackDelegate, p1, p2); }
                finally { SafeFreeHGlobal(p1); SafeFreeHGlobal(p2); }
            };

            _webSocketErrorWrapper = (guid, errorCode, error, responseCode, headersEncoded) =>
            {
                if (IsDisposed() || _webSocketErrorCallBackDelegate == null || string.IsNullOrEmpty(guid)) return;
                IntPtr p1 = IntPtr.Zero, p2 = IntPtr.Zero, p3 = IntPtr.Zero;
                try
                {
                    p1 = Utf8Alloc(guid);
                    p2 = Utf8Alloc(error ?? string.Empty);
                    p3 = Utf8Alloc(headersEncoded ?? string.Empty);
                    SafeInvoke(_webSocketErrorCallBackDelegate, p1, errorCode, p2, responseCode, p3);
                }
                finally { SafeFreeHGlobal(p1); SafeFreeHGlobal(p2); SafeFreeHGlobal(p3); }
            };

            _webSocketDataWrapper = guid =>
            {
                if (IsDisposed() || _webSocketDataCallBackDelegate == null || string.IsNullOrEmpty(guid)) return;
                IntPtr p1 = IntPtr.Zero;
                try { p1 = Utf8Alloc(guid); SafeInvoke(_webSocketDataCallBackDelegate, p1); }
                finally { SafeFreeHGlobal(p1); }
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

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_finalize", CallConvs = [typeof(CallConvCdecl)])]
    public static void FinalizeAne()
    {
        // 1) bloquear novas invocações
        MarkDisposed();
        // 3) desconectar/limpar websockets
        foreach (var (guid, ws) in WebSocketClients.ToList())
        {
            try { ws.Disconnect(1000); } catch { }
            try { ws.Dispose(); } catch { }
            WebSocketClients.TryRemove(guid, out _);
        }

        // 4) zerar delegates
        ClearCallbacks();
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_createWebSocket", CallConvs = [typeof(CallConvCdecl)])]
    public static DataArray CreateWebSocket()
    {
        if (IsDisposed()) return new DataArray();

        var guid = Guid.NewGuid();
        var guidString = guid.ToString();
        var alreadyDispatchError = false;
        var lockError = new Lock();

        var webSocketClient = new WebSocketClient(
            responseHeaders =>
            {
                if (IsDisposed()) return;
                var headersEncoded64 = Convert.ToBase64String(Encoding.UTF8.GetBytes(JsonSerializer.Serialize(responseHeaders, JsonDictionaryHeaderContext.Default.DictionaryStringString)));
                _webSocketConnectWrapper?.Invoke(guidString, headersEncoded64);
            },
            () => { if (!IsDisposed()) _webSocketDataWrapper?.Invoke(guidString); },
            (errorCode, error, responseCode, responseHeaders) =>
            {
                using (lockError.EnterScope())
                {
                    if (alreadyDispatchError) return;
                    var headersEncoded64 = Convert.ToBase64String(Encoding.UTF8.GetBytes(JsonSerializer.Serialize(responseHeaders, JsonDictionaryHeaderContext.Default.DictionaryStringString)));
                    if (!IsDisposed()) _webSocketErrorWrapper?.Invoke(guidString, errorCode, error, responseCode, headersEncoded64);
                    if (WebSocketClients.TryRemove(guid, out var removed)) removed.Dispose();
                    alreadyDispatchError = true;
                }
            },
            _writeLogWrapper);

        if (!WebSocketClients.TryAdd(guid, webSocketClient))
        {
            webSocketClient.Dispose();
            return new DataArray();
        }

        return CreateDataArrayFromString(guidString);
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_connectWebSocket", CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe int ConnectWebSocket(IntPtr guidPointer, int guidLen, IntPtr pointerUri, int uriLen, IntPtr pointerHeaders, int headersLen)
    {
        if (IsDisposed()) return 0;
        try
        {
            var guidString = Encoding.UTF8.GetString((byte*)guidPointer, guidLen);
            if (!Guid.TryParse(guidString, out var guid)) return 0;
            if (!WebSocketClients.TryGetValue(guid, out var client)) return 0;

            var uri = Encoding.UTF8.GetString((byte*)pointerUri, uriLen);
            var headers = Encoding.UTF8.GetString((byte*)pointerHeaders, headersLen);
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
    public static unsafe int SendWebSocketMessage(IntPtr guidPointer, int guidLen, IntPtr pointerData, int length)
    {
        if (IsDisposed()) return 0;
        try
        {
            var guidString = Encoding.UTF8.GetString((byte*)guidPointer, guidLen);
            if (!Guid.TryParse(guidString, out var guid)) return 0;
            if (!WebSocketClients.TryGetValue(guid, out var client)) return 0;

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
    public static unsafe int CloseWebSocket(IntPtr guidPointer, int guidLen, int closeCode)
    {
        if (IsDisposed()) return 0;
        try
        {
            var guidString = Encoding.UTF8.GetString((byte*)guidPointer, guidLen);
            if (!Guid.TryParse(guidString, out var guid)) return 0;
            if (!WebSocketClients.TryGetValue(guid, out var client)) return 0;

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
    public static unsafe DataArray GetWebSocketMessage(IntPtr guidPointer, int guidLen)
    {
        if (IsDisposed()) return new DataArray();
        try
        {
            var guidString = Encoding.UTF8.GetString((byte*)guidPointer, guidLen);
            if (!Guid.TryParse(guidString, out var guid)) return new DataArray();
            if (!WebSocketClients.TryGetValue(guid, out var client)) return new DataArray();

            if (!client.TryGetNextMessage(out var message)) return new DataArray();
            return CreateDataArrayFromBytes(message);
        }
        catch (Exception e)
        {
            LogAll(e, _writeLogWrapper);
            return new DataArray();
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_addStaticHost", CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe void AddStaticHost(IntPtr hostPtr, int hostLen, IntPtr ipPtr, int ipLen)
    {
        if (IsDisposed()) return;
        try
        {
            var host = Encoding.UTF8.GetString((byte*)hostPtr, hostLen);
            var ip = Encoding.UTF8.GetString((byte*)ipPtr, ipLen);
            DnsInternalResolver.Instance.AddStaticHost(host, ip);
        }
        catch
        {
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_removeStaticHost", CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe void RemoveStaticHost(IntPtr hostPtr, int hostLen)
    {
        if (IsDisposed()) return;
        try
        {
            var host = Encoding.UTF8.GetString((byte*)hostPtr, hostLen);
            DnsInternalResolver.Instance.RemoveStaticHost(host);
        }
        catch
        {
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_loadUrl", CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe DataArray LoadUrl(IntPtr urlPtr, int urlLen, IntPtr methodPtr, int methodLen, IntPtr variablesPtr, int variablesLen, IntPtr headersPtr, int headersLen)
    {
        if (IsDisposed()) return new DataArray();
        try
        {
            var url = Encoding.UTF8.GetString((byte*)urlPtr, urlLen);
            var method = Encoding.UTF8.GetString((byte*)methodPtr, methodLen);
            var variables = Encoding.UTF8.GetString((byte*)variablesPtr, variablesLen);
            var headers = Encoding.UTF8.GetString((byte*)headersPtr, headersLen);

            var variablesDictionary = string.IsNullOrEmpty(variables) ? new Dictionary<string, string>() : JsonSerializer.Deserialize(variables, JsonDictionaryHeaderContext.Default.DictionaryStringString);
            var headersDictionary = string.IsNullOrEmpty(headers) ? new Dictionary<string, string>() : JsonSerializer.Deserialize(headers, JsonDictionaryHeaderContext.Default.DictionaryStringString);

            var randomId = LoaderManager.Instance.StartLoad(url, method, variablesDictionary, headersDictionary);
            return CreateDataArrayFromString(randomId);
        }
        catch (Exception e)
        {
            LogAll(e, _writeLogWrapper);
            return new DataArray();
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_getLoaderResult", CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe DataArray GetLoaderResult(IntPtr guidPointer, int guidLen)
    {
        if (IsDisposed()) return new DataArray();
        try
        {
            var guidString = Encoding.UTF8.GetString((byte*)guidPointer, guidLen);
            if (!Guid.TryParse(guidString, out var guid)) return new DataArray();
            if (!LoaderManager.Instance.TryGetResult(guid, out var data)) return new DataArray();

            return CreateDataArrayFromBytes(data);
        }
        catch (Exception e)
        {
            LogAll(e, _writeLogWrapper);
            return new DataArray();
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_deviceUniqueId", CallConvs = [typeof(CallConvCdecl)])]
    public static DataArray GetDeviceUniqueId()
    {
        if (IsDisposed()) return new DataArray();
        try
        {
            var s = HardwareID.GetDeviceUniqueIdHash(e => LogAll(e, _writeLogWrapper));
            return CreateDataArrayFromString(s);
        }
        catch
        {
            return new DataArray();
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_isRunningOnEmulator", CallConvs = [typeof(CallConvCdecl)])]
    public static int IsRunningOnEmulator()
    {
        if (IsDisposed()) return 0;
        try
        {
            return VMDetector.IsRunningInVM() ? 1 : 0;
        }
        catch
        {
            return 0;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_decompressByteArray", CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe DataArray DecompressByteArray(IntPtr data, int length)
    {
        if (IsDisposed()) return new DataArray();
        try
        {
            using var srcStream = new UnmanagedMemoryStream((byte*)data.ToPointer(), length);
            using var ms = CompressUtil.Uncompress(srcStream);
            var buffer = ms.GetBuffer();
            var dataSpan = new ReadOnlySpan<byte>(buffer, 0, (int)ms.Length);
            return CreateDataArrayFromBytes(dataSpan);
        }
        catch (Exception e)
        {
            LogAll(e, _writeLogWrapper);
            return new DataArray();
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_readFileToByteArray", CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe DataArray ReadFileToByteArray(IntPtr path, int pathLen)
    {
        if (IsDisposed()) return new DataArray();
        try
        {
            var filePath = Encoding.UTF8.GetString((byte*)path, pathLen);
            if (string.IsNullOrEmpty(filePath)) return new DataArray();

            if (!File.Exists(filePath)) return new DataArray();

            using var fs = new FileStream(filePath, FileMode.Open, FileAccess.Read);
            using var ms = new MemoryStream();
            fs.CopyTo(ms);
            var buffer = ms.GetBuffer();
            var dataSpan = new ReadOnlySpan<byte>(buffer, 0, (int)ms.Length);
            return CreateDataArrayFromBytes(dataSpan);
        }
        catch (Exception e)
        {
            LogAll(e, _writeLogWrapper);
            return new DataArray();
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_mapXmlToObject", CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe IntPtr MapXmlToObject(
        IntPtr xmlPtr, int xmlLen,
        IntPtr pFRENewObject,
        IntPtr pFRENewObjectFromBool,
        IntPtr pFRENewObjectFromInt32,
        IntPtr pFRENewObjectFromUint32,
        IntPtr pFRENewObjectFromDouble,
        IntPtr pFRENewObjectFromUTF8,
        IntPtr pFRESetObjectProperty)
    {
        if (IsDisposed()) return IntPtr.Zero;
        var result = IntPtr.Zero;
        try
        {
            var xml = Encoding.UTF8.GetString((byte*)xmlPtr, xmlLen);
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

                var classPtr = Utf8Alloc("Object");
                freNewObject(classPtr, 0, IntPtr.Zero, out var obj, out _);
                Marshal.FreeHGlobal(classPtr);
                if (obj == IntPtr.Zero) return IntPtr.Zero;

                foreach (var attr in elem.Attributes())
                {
                    var namePtr = Utf8Alloc(attr.Name.LocalName);
                    var valObj = ValueToFre(attr.Value, freNewFromBool, freNewFromInt32, freNewFromUint32, freNewFromDouble, freNewFromUtf8);
                    freSetProp(obj, namePtr, valObj, out _);
                    Marshal.FreeHGlobal(namePtr);
                }

                var groups = elem.Elements().GroupBy(e => e.Name.LocalName);
                foreach (var g in groups)
                {
                    var propPtr = Utf8Alloc(g.Key);
                    IntPtr propVal;
                    if (g.Count() == 1)
                    {
                        propVal = Convert(g.First());
                    }
                    else
                    {
                        var arrClassPtr = Utf8Alloc("Array");
                        IntPtr arr;
                        freNewObject(arrClassPtr, 0, IntPtr.Zero, out arr, out _);
                        Marshal.FreeHGlobal(arrClassPtr);
                        if (arr == IntPtr.Zero) { Marshal.FreeHGlobal(propPtr); continue; }

                        uint idx = 0;
                        foreach (var child in g)
                        {
                            var childObj = Convert(child);
                            var idxPtr = Utf8Alloc(idx.ToString());
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
            LogAll(e, _writeLogWrapper);
            return result;
        }
    }

    private static IntPtr ValueToFre(string val, FRENewObjectFromBoolDelegate fromBool, FRENewObjectFromInt32Delegate fromInt, FRENewObjectFromUint32Delegate fromUint, FRENewObjectFromDoubleDelegate fromDouble,
        FRENewObjectFromUTF8Delegate fromUtf8)
    {
        if (bool.TryParse(val, out var b))
        {
            fromBool((uint)(b ? 1 : 0), out var objB);
            return objB;
        }

        // Check for leading zero numeric strings to treat as string
        var isLeadingZeroNumeric = val != null && val.Length > 1 && val[0] == '0' && val.All(char.IsDigit);

        if (!isLeadingZeroNumeric && int.TryParse(val, out var i))
        {
            fromInt(i, out var objI);
            return objI;
        }

        if (!isLeadingZeroNumeric && uint.TryParse(val, out var u))
        {
            fromUint(u, out var objU);
            return objU;
        }

        if (!isLeadingZeroNumeric && double.TryParse(val, out var d))
        {
            fromDouble(d, out var objD);
            return objD;
        }

        var bytes = Encoding.UTF8.GetBytes(val ?? string.Empty);
        var buf = Marshal.AllocHGlobal(bytes.Length + 1);
        Marshal.Copy(bytes, 0, buf, bytes.Length);
        Marshal.WriteByte(buf + bytes.Length, 0);
        fromUtf8((uint)bytes.Length, buf, out var strObj);
        Marshal.FreeHGlobal(buf);
        return strObj;
    }

    private static IntPtr Utf8Alloc(string s)
    {
        var bytes = Encoding.UTF8.GetBytes((s ?? string.Empty) + '\0');
        var ptr = Marshal.AllocHGlobal(bytes.Length);
        Marshal.Copy(bytes, 0, ptr, bytes.Length);
        return ptr;
    }

    [UnmanagedCallersOnly(EntryPoint = "csharpLibrary_awesomeUtils_disposeDataArrayBytes", CallConvs = [typeof(CallConvCdecl)])]
    public static void DisposeDataArray(IntPtr dataPointer)
    {
        try
        {
            if (dataPointer != IntPtr.Zero)
            {
                Marshal.FreeCoTaskMem(dataPointer);
            }
        }
        catch
        {
        }
    }

    private static void SafeFreeHGlobal(IntPtr ptr)
    {
        try
        {
            if (ptr != IntPtr.Zero) Marshal.FreeHGlobal(ptr);
        }
        catch
        {
        }
    }

    private static DataArray CreateDataArrayFromString(string s)
    {
        if (string.IsNullOrEmpty(s)) return new DataArray();
        var bytes = Encoding.UTF8.GetBytes(s);
        return CreateDataArrayFromBytes(bytes);
    }

    private static unsafe DataArray CreateDataArrayFromBytes(ReadOnlySpan<byte> data)
    {
        var result = new DataArray();
        if (data.IsEmpty) return result;
        result.Size = data.Length;
        result.DataPointer = Marshal.AllocCoTaskMem(result.Size);
        data.CopyTo(new Span<byte>((void*)result.DataPointer, result.Size));
        return result;
    }

    private static void LogAll(Exception exception, Action<string> callback)
    {
        if (exception == null) return;
        try
        {
            var sb = new StringBuilder();
            sb.AppendLine($"Exception: {exception.Message}");
            sb.AppendLine($"Stack Trace: {exception.StackTrace}");
            var inner = exception.InnerException;
            while (inner != null)
            {
                sb.AppendLine($"Inner Exception: {inner.Message}");
                sb.AppendLine($"Inner Stack Trace: {inner.StackTrace}");
                inner = inner.InnerException;
            }

            callback?.Invoke(sb.ToString());
        }
        catch
        {
        }
    }
}
