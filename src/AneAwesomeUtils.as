package {
import air.net.WebSocket;

import flash.events.Event;
import flash.events.IOErrorEvent;
import flash.events.StatusEvent;
import flash.events.WebSocketEvent;
import flash.external.ExtensionContext;
import flash.net.URLVariables;
import flash.system.Capabilities;
import flash.utils.ByteArray;
import flash.utils.Dictionary;

public class AneAwesomeUtils {

    public static function get isSupported():Boolean {
        var plataform:String = Capabilities.version.substr(0, 3);
        switch (plataform) {
            case "AND":
                return true;
            case "WIN":
                return true;
            case "MAC":
                return true;
            case "IOS":
                return true;
            default:
                return false;
        }
    }

    private static var _instance:AneAwesomeUtils;

    public static function get instance():AneAwesomeUtils {
        if (!_instance) {
            _instance = new AneAwesomeUtils();
        }
        return _instance;
    }

    private var _extContext:ExtensionContext;
    private var _websockets:Dictionary;
    private var _loaders:Dictionary;
    private var _successInit:Boolean;

    function AneAwesomeUtils() {
        _extContext = ExtensionContext.createExtensionContext("br.com.redesurftank.aneawesomeutils", "");
        if (!_extContext) {
            throw new Error("ANE not loaded properly. Please check if the extension is added to your AIR project.");
        }
        _extContext.addEventListener("status", onStatusEvent);
        _websockets = new Dictionary();
        _loaders = new Dictionary();
    }

    public function initialize():Boolean {
        var result:Boolean = _extContext.call("awesomeUtils_initialize") as Boolean;
        _successInit = result;
        return result;
    }

    public function uuid():String {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        return _extContext.call("awesomeUtils_uuid") as String;
    }

    private function getWebSocket(id:String):AneWebSocket {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        return _websockets[id];
    }

    public function createWebSocket():AneWebSocket {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        var id:String = _extContext.call("awesomeUtils_createWebSocket") as String;
        var ws:AneWebSocket = new AneWebSocket(id);
        _websockets[id] = ws;
        return ws;
    }

    public function sendWebSocketMessage(id:String, type:uint, data:*):void {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        _extContext.call("awesomeUtils_sendWebSocketMessage", id, type, data);
    }

    public function closeWebSocket(id:String, code:int = 1000):void {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        _extContext.call("awesomeUtils_closeWebSocket", id, code);
    }

    public function connectWebSocket(id:String, url:String):void {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        _extContext.call("awesomeUtils_connectWebSocket", id, url);
    }

    public function removeWebSocket(ws:AneWebSocket):void {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        delete _websockets[ws.id];
    }

    public function addStaticHostResolution(host:String, ip:String):void {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        _extContext.call("awesomeUtils_addStaticHost", host, ip);
    }

    public function removeStaticHostResolution(host:String):void {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        _extContext.call("awesomeUtils_removeStaticHost", host);
    }

    public function loadUrl(url:String, method:String = "GET", variables:Object = null, headers:Object = null, onResult:Function = null, onError:Function = null, onProgress:Function = null):void {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        if (headers is Dictionary) {
            var headersDict:Dictionary = headers as Dictionary;
            var headersObj:Object = {};
            for (var key:Object in headersDict) {
                headersObj[key] = String(headersDict[key]);
            }
            headers = headersObj;
        }
        if (variables is URLVariables) {
            var variablesURL:URLVariables = variables as URLVariables;
            var variablesObj:Object = {};
            for (key in variablesURL) {
                variablesObj[key] = String(variablesURL[key]);
            }
            variables = variablesObj;
        }
        var variablesJson:String = variables ? JSON.stringify(variables) : "";
        var headersJson:String = headers ? JSON.stringify(headers) : "";
        var loaderId:String = _extContext.call("awesomeUtils_loadUrl", url, method, variablesJson, headersJson) as String;
        if (!loaderId) {
            if (onError) {
                onError(new Error("Error loading URL"));
            }
            return;
        }
        _loaders[loaderId] = {onResult: onResult, onError: onError, onProgress: onProgress};
    }

    private function onStatusEvent(param1:StatusEvent):void {
        var dataSplit:Array = param1.level.split(";");
        var type:String = dataSplit[0];
        switch (type) {
            case "url-loader":
                handleUrlLoaderEvent(param1);
                break;
            case "web-socket":
                handleWebSocketEvent(param1);
                break;
            default:
                throw new Error("Unknown event type: " + type);
        }
    }

    private function handleUrlLoaderEvent(param1:StatusEvent):void {
        var dataSplit:Array = param1.level.split(";");
        var loaderId:String = dataSplit[1];
        var loader:Object = _loaders[loaderId];
        if (!loader) {
            return;
        }
        switch (param1.code) {
            case "progress": {
                if (loader.onProgress) {
                    loader.onProgress(dataSplit[2]);
                }
                break;
            }
            case "error": {
                if (loader.onError) {
                    loader.onError(new Error(dataSplit[2]));
                }
                delete _loaders[loaderId];
                break;
            }
            case "success": {
                if (loader.onResult) {
                    var result:ByteArray = _extContext.call("awesomeUtils_getLoaderResult", loaderId) as ByteArray;
                    if (!result) {
                        if (loader.onError) {
                            loader.onError(new Error("Error getting result"));
                        }
                    } else {
                        result.position = 0;
                        loader.onResult(result);
                    }
                }
                delete _loaders[loaderId];
                break;
            }
        }
    }

    private function handleWebSocketEvent(param1:StatusEvent):void {
        var dataSplit:Array = param1.level.split(";");
        var ws:AneWebSocket = getWebSocket(dataSplit[1]);
        switch (param1.code) {
            case "connected":
                ws.dispatchEvent(new Event("connect"));
                break;
            case "nextMessage":
                var bytes:ByteArray = _extContext.call("awesomeUtils_getWebSocketByteArrayMessage") as ByteArray;
                if (!bytes)
                    break;
                bytes.position = 0;
                ws.dispatchEvent(new WebSocketEvent("websocketData", WebSocket.fmtBINARY, bytes));
                break;
            case "disconnected":
                var parameters:Array = param1.level.split(";");
                ws.closeReason = int(parameters[0]);
                ws.dispatchEvent(new Event("close"));
                break;
            case "error":
                ws.dispatchEvent(new IOErrorEvent("ioError", false, false, param1.level));
                break;
            default:
                throw new Error("TODO: handle StatusEvent code = [" + param1.code + "], level = [" + param1.level + "]");
        }
    }
}
}
