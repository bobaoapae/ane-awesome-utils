package {
import aneAwesomeUtils.Base64;
import aneAwesomeUtils.ILogging;

import flash.events.EventDispatcher;
import flash.events.StatusEvent;
import flash.external.ExtensionContext;
import flash.net.URLVariables;
import flash.system.Capabilities;
import flash.utils.ByteArray;
import flash.filesystem.File;
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

    private static function IsWindows():Boolean {
        var plataform:String = Capabilities.version.substr(0, 3);
        return plataform == "WIN";
    }

    private static function IsMac():Boolean {
        var plataform:String = Capabilities.version.substr(0, 3);
        return plataform == "MAC";
    }

    private static function IsIOS():Boolean {
        var plataform:String = Capabilities.version.substr(0, 3);
        return plataform == "IOS";
    }

    private static function IsAndroid():Boolean {
        var plataform:String = Capabilities.version.substr(0, 3);
        return plataform == "AND";
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
    private var _callbackEmulatorDetected:Function;
    private var _successInit:Boolean;
    private var _logging:ILogging;
    private var _networkAvailable:Boolean = true;
    private var _networkEventDispatcher:EventDispatcher;
    private var _logReadCallbacks:Object;
    private var _unexpectedShutdownCallback:Function;
    private var _bundleCallbacks:Object;

    function AneAwesomeUtils() {
        _extContext = ExtensionContext.createExtensionContext("br.com.redesurftank.aneawesomeutils", "");
        if (!_extContext) {
            throw new Error("ANE not loaded properly. Please check if the extension is added to your AIR project.");
        }
        _extContext.addEventListener("status", onStatusEvent);
        _websockets = new Dictionary();
        _loaders = new Dictionary();
        _networkEventDispatcher = new EventDispatcher();
    }

    private function doLogging(level:String, message:String):void {
        if (_logging) {
            _logging.onLog(level, message);
        } else {
            trace("AneAwesomeUtils: " + level + ": " + message);
        }
    }

    public function set logging(value:ILogging):void {
        _logging = value;
    }

    public function get successInit():Boolean {
        return _successInit;
    }

    public function initialize():Boolean {
        try {
            var result:Boolean = _extContext.call("awesomeUtils_initialize") as Boolean;
            _successInit = result;
        } catch (e:Error) {
            trace("Error initializing ANE: " + e.message + " " + e.getStackTrace());
        }
        return result;
    }

    public function dispose():void {
        if (_extContext) {
            _extContext.removeEventListener("status", onStatusEvent);
            _extContext.dispose();
            _extContext = null;
        }
        _websockets = null;
        _loaders = null;
        _successInit = false;
        _logging = null;
        _networkEventDispatcher = null;
        _logReadCallbacks = null;
        _unexpectedShutdownCallback = null;
        _bundleCallbacks = null;
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

    public function connectWebSocket(id:String, url:String, headers:Dictionary):void {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        var headersObj:Object = {};
        for (var key:Object in headers) {
            headersObj[key] = String(headers[key]);
        }
        var headersJson:String = JSON.stringify(headersObj);
        _extContext.call("awesomeUtils_connectWebSocket", id, url, headersJson);
    }

    public function removeWebSocket(ws:AneWebSocket):void {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        delete _websockets[ws.id];
    }

    public function addClientCertificate(host:String, p12Data:String, password:String):void {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        _extContext.call("awesomeUtils_addClientCertificate", host, p12Data, password);
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
        var i:int = url.indexOf("?");
        if (i > -1) {
            var qs:String = url.substring(i + 1);
            url = url.substring(0, i);
            //if url start with & remove the first &
            if (qs.charAt(0) == "&") {
                qs = qs.substring(1);
            }
            var queryVars:URLVariables;
            try {
                queryVars = new URLVariables(qs);
            } catch (e:Error) {
                doLogging("ERROR", "Error parsing query string: " + e.message + " - query string: " + qs + " - stack: " + e.getStackTrace());
                queryVars = new URLVariables();
            }
            if (!variables) {
                variables = {};
            }
            if (variables is URLVariables) {
                var tmp:Object = {};
                for (var k:String in variables) {
                    tmp[k] = String(variables[k]);
                }
                variables = tmp;
            }
            for (var k2:String in queryVars) {
                variables[k2] = String(queryVars[k2]);
            }
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


    /**
     * Like loadUrl but sends a raw ByteArray as the request body.
     * Useful for PUT/POST with binary data (zip files, images, etc).
     */
    public function loadUrlWithBody(url:String, method:String, body:ByteArray, contentType:String = "application/octet-stream", headers:Object = null, onResult:Function = null, onError:Function = null):void {
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
        var headersJson:String = headers ? JSON.stringify(headers) : "";
        body.position = 0;
        var loaderId:String = _extContext.call("awesomeUtils_loadUrlWithBody", url, method, headersJson, body, contentType) as String;
        if (!loaderId) {
            if (onError) {
                onError(new Error("Error loading URL with body"));
            }
            return;
        }
        _loaders[loaderId] = {onResult: onResult, onError: onError, onProgress: null};
    }

    public function getDeviceUniqueId():String {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        return _extContext.call("awesomeUtils_getDeviceUniqueId") as String;
    }

    /**
     * This method is used to check if the app is running on an emulator.
     * On Android, it checks if the device is an emulator.
     * On Windows, it checks if the device is a Virtual Machine.
     * Currently, this method is only implemented for Windows and Android. On other platforms, it will return false.
     * @return
     */
    public function isRunningOnEmulator():Boolean {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        var isCurrentPlatformSupported:Boolean = IsAndroid() || IsWindows();
        if (!isCurrentPlatformSupported) {
            return false;
        }
        return _extContext.call("awesomeUtils_isRunningOnEmulator") as Boolean;
    }

    /**
     * This method is used to check if the app is running on an emulator.
     * On Android, it checks if the device is an emulator.
     * Currently, this method is only implemented for Android. On other platforms, it will return false.
     * @return
     */
    public function isRunningOnEmulatorAsync(callback:Function):void {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        var isCurrentPlatformSupported:Boolean = IsAndroid();
        if (!isCurrentPlatformSupported) {
            if (callback) {
                callback(false);
            }
        }
        _callbackEmulatorDetected = callback;
        _extContext.call("awesomeUtils_isRunningOnEmulatorAsync");
    }

    public function decompressByteArray(source:ByteArray, target:ByteArray):void {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        _extContext.call("awesomeUtils_decompressByteArray", source, target);
    }

    public function readFileToByteArray(path:String, target:ByteArray):void {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        _extContext.call("awesomeUtils_readFileToByteArray", path, target);
        target.position = 0;
    }

    // --- Native Logging methods ---

    /**
     * Set a callback to be called when an unexpected shutdown is detected.
     * The callback receives a JSON string with info about old log files:
     * [{"date":"2026-03-25","size":1234,"path":"/full/path/ane-log-2026-03-25.txt"}, ...]
     * @param callback Function(oldLogsJson:String):void
     */
    public function set onUnexpectedShutdown(callback:Function):void {
        _unexpectedShutdownCallback = callback;
    }

    /**
     * Initialize the native logging system.
     * On Windows/Mac, separates logs by profile (for multi-account).
     * On Android/iOS, profile is always "default".
     * @param profile Profile name for log separation (default: "default")
     * @return The full path to the log directory, or null on failure
     */
    public function initLog(profile:String = "default"):String {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        var storagePath:String = File.applicationStorageDirectory.nativePath;
        doLogging("INFO", "Initializing native logging with profile: " + profile + " storagePath: " + storagePath);
        return _extContext.call("awesomeUtils_initLog", profile, storagePath) as String;
    }

    /**
     * Write a log message to the native log file.
     * Also outputs to platform-specific system log (logcat on Android, os_log on Apple, stdout on Windows).
     * @param level Log level: "DEBUG", "INFO", "WARN", "ERROR"
     * @param tag Component/module tag
     * @param message The log message
     */
    public function writeLogMessage(level:String, tag:String, message:String):void {
        if (!_successInit) return;
        _extContext.call("awesomeUtils_writeLog", level, tag, message);
    }

    /**
     * Get list of all log files for the current profile.
     * @return JSON string: [{"date":"2026-03-25","size":1234,"path":"/full/path"}, ...]
     */
    public function getLogFileList():String {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        return _extContext.call("awesomeUtils_getLogFiles") as String;
    }

    /**
     * Asynchronously read log file(s) as a ByteArray.
     * @param date Date string "YYYY-MM-DD" to read a specific day, or null to read all logs concatenated
     * @param onResult Called with ByteArray when read completes
     * @param onError Called with Error if read fails (optional)
     */
    public function readLogFile(date:String = null, onResult:Function = null, onError:Function = null):void {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        _logReadCallbacks = {onResult: onResult, onError: onError};
        _extContext.call("awesomeUtils_readLogFile", date ? date : "");
    }

    /**
     * Delete log file(s).
     * @param date Date string "YYYY-MM-DD" to delete a specific day, or null to delete all logs
     * @return true if deletion succeeded
     */
    public function deleteLogFile(date:String = null):Boolean {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        return _extContext.call("awesomeUtils_deleteLogFile", date ? date : "") as Boolean;
    }

    /**
     * Package a crash session's log + device metadata into a single ZIP on disk.
     * The native side XOR-decrypts the log, writes crash.log + metadata.json into
     * a ZIP under {appStorage}/crash-bundles/, and asynchronously dispatches
     * either "log;bundleReady" (with the zip path) or "log;bundleError".
     *
     * Typical flow: app detects unexpected shutdown → calls this → receives the
     * path in onSuccess → reads the ZIP bytes (via readFileToByteArray) → uploads
     * them as application/zip → calls deleteCrashBundle(path) on success.
     *
     * @param date        "YYYY-MM-DD" for a specific day, or null/empty for "all logs on the latest day"
     * @param appVersion  version string included in metadata.json
     * @param sessionId   optional session identifier surfaced in metadata.json
     * @param onSuccess   Function(zipPath:String):void
     * @param onError     Function(error:Error):void
     */
    public function packageCrashBundle(date:String, appVersion:String, sessionId:String,
                                        onSuccess:Function = null, onError:Function = null):void {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        _bundleCallbacks = {onSuccess: onSuccess, onError: onError};
        _extContext.call("awesomeUtils_packageCrashBundle",
                         date ? date : "",
                         appVersion ? appVersion : "",
                         sessionId ? sessionId : "");
    }

    /**
     * Delete a crash bundle ZIP by absolute path. Call after successful upload.
     * Synchronous; returns true on success.
     */
    public function deleteCrashBundle(zipPath:String):Boolean {
        if (!_successInit || !zipPath) return false;
        return _extContext.call("awesomeUtils_deleteCrashBundle", zipPath) as Boolean;
    }

    /**
     * Notify that the app went to background. Records the timestamp so that
     * if the OS kills the process after a long time, it won't be reported
     * as an unexpected shutdown. Short background kills (user force-close)
     * are still reported.
     * Only relevant on Android/iOS where the OS may kill background apps.
     */
    public function notifyBackground():void {
        if (!_successInit) return;
        if (!IsAndroid() && !IsIOS()) return;
        _extContext.call("awesomeUtils_notifyBackground");
    }

    /**
     * Notify that the app returned to foreground. Clears the background
     * timestamp so that crashes in foreground are always reported.
     * Only relevant on Android/iOS where the OS may kill background apps.
     */
    public function notifyForeground():void {
        if (!_successInit) return;
        if (!IsAndroid() && !IsIOS()) return;
        _extContext.call("awesomeUtils_notifyForeground");
    }

    // --- Windows-specific methods ---

    public function preventCaptureScreen():Boolean {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        if (!IsWindows()) {
            trace("Warning: preventCaptureScreen is only supported on Windows.");
            return false;
        }
        var result:Boolean = _extContext.call("awesomeUtils_preventCapture") as Boolean;
        if (!result) {
            trace("Warning: preventCaptureScreen failed. This may not be supported on this platform or device.");
        }
        return result;
    }

    public function isPreventCaptureEnabled():Boolean {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        if (!IsWindows()) {
            trace("Warning: isPreventCaptureEnabled is only supported on Windows.");
            return false;
        }
        return _extContext.call("awesomeUtils_isPreventCaptureEnabled") as Boolean;
    }

    public function filterWindowsInputs(filteredKeys:Array = null):Boolean {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        if (!IsWindows()) {
            trace("Warning: filterWindowsInputs is only supported on Windows.");
            return false;
        }
        var result:Boolean = _extContext.call("awesomeUtils_filterWindowsInputs", filteredKeys) as Boolean;
        if (!result) {
            trace("Warning: filterWindowsInputs failed. This may not be supported on this platform or device.");
        }
        return result;
    }

    public function stopWindowsFilterInputs():Boolean {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        if (!IsWindows()) {
            trace("Warning: stopWindowsFilterInputs is only supported on Windows.");
            return false;
        }
        var result:Boolean = _extContext.call("awesomeUtils_stopFilterWindowsInputs") as Boolean;
        if (!result) {
            trace("Warning: stopWindowsFilterInputs failed. This may not be supported on this platform or device.");
        }
        return result;
    }

    public function blockWindowsLeaveMouseEvent():Boolean {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        if (!IsWindows()) {
            trace("Warning: blockWindowsLeaveMouseEvent is only supported on Windows.");
            return false;
        }
        var result:Boolean = _extContext.call("awesomeUtils_blockWindowsLeaveMouseEvent") as Boolean;
        if (!result) {
            trace("Warning: blockWindowsLeaveMouseEvent failed. This may not be supported on this platform or device.");
        }
        return result;
    }

    public function isCheatEngineSpeedHackDetected():Boolean {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        if (!IsWindows()) {
            trace("Warning: isCheatEngineSpeedHackDetected is only supported on Windows.");
            return false;
        }
        var result:Boolean = _extContext.call("awesomeUtils_isCheatEngineSpeedHackDetected") as Boolean;
        return result;
    }

    public function forceBlueScreenOfDead():void {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        if (!IsWindows()) {
            trace("Warning: forceBlueScreenOfDead is only supported on Windows.");
            return;
        }
        _extContext.call("awesomeUtils_forceBlueScreenOfDead");
    }

    // --- Android-specific methods ---

    public function isBatteryOptimizationIgnored():Boolean {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        if (!IsAndroid()) {
            return true;
        }
        return _extContext.call("awesomeUtils_isBatteryOptimizationIgnored") as Boolean;
    }

    public function requestBatteryOptimizationExclusion():void {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        if (!IsAndroid()) {
            return;
        }
        _extContext.call("awesomeUtils_requestBatteryOptimizationExclusion");
    }

    public function configureConnection(pingInterval:int, connectTimeout:int, readTimeout:int, writeTimeout:int):void {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        if (!IsAndroid()) {
            return;
        }
        _extContext.call("awesomeUtils_configureConnection", pingInterval, connectTimeout, readTimeout, writeTimeout);
    }

    public function releaseConnectionResources():void {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        if (!IsAndroid()) {
            return;
        }
        _extContext.call("awesomeUtils_releaseConnectionResources");
    }

    public function get networkAvailable():Boolean {
        return _networkAvailable;
    }

    public function addNetworkStateChangeListener(listener:Function):void {
        _networkEventDispatcher.addEventListener("networkStateChange", listener);
    }

    public function removeNetworkStateChangeListener(listener:Function):void {
        _networkEventDispatcher.removeEventListener("networkStateChange", listener);
    }

    public function mapXmlToObject(xmlString:String):Object {
        if (!_successInit) {
            throw new Error("ANE not initialized properly. Please check if the extension is added to your AIR project.");
        }
        var result:Object = _extContext.call("awesomeUtils_mapXmlToObject", xmlString);
        return result;
    }

    private function onStatusEvent(param1:StatusEvent):void {
        var dataSplit:Array = param1.code.split(";");
        var type:String = dataSplit[0];
        var restCode:String = param1.code.substr(type.length + 1);
        param1 = new StatusEvent("status", false, false, restCode, param1.level);
        switch (type) {
            case "url-loader":
                handleUrlLoaderEvent(param1);
                break;
            case "web-socket":
                handleWebSocketEvent(param1);
                break;
            case "emulator-detected":
                if (_callbackEmulatorDetected != null) {
                    var isEmulator:Boolean = param1.level == "true";
                    _callbackEmulatorDetected(isEmulator);
                    _callbackEmulatorDetected = null;
                }
                break;
            case "log":
                handleLogEvent(param1);
                break;
            case "network":
                var state:String = dataSplit[1];
                _networkAvailable = (state == "available");
                _networkEventDispatcher.dispatchEvent(new StatusEvent("networkStateChange", false, false, state));
                break;
            default:
                throw new Error("Unknown event type: " + type);
        }
    }

    private function handleUrlLoaderEvent(param1:StatusEvent):void {
        var codeSplit:Array = param1.code.split(";");
        var dataSplit:Array = param1.level.split(";");
        var loaderId:String = codeSplit[1];
        var loader:Object = _loaders[loaderId];
        if (!loader) {
            return;
        }
        switch (codeSplit[0]) {
            case "progress": {
                if (loader.onProgress) {
                    loader.onProgress(dataSplit[0], dataSplit[1]);
                }
                break;
            }
            case "error": {
                if (loader.onError) {
                    loader.onError(new Error(dataSplit[0]));
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
                        if (result.length == 4) {
                            var header1:int = result.readUnsignedByte();
                            var header2:int = result.readUnsignedByte();
                            var header3:int = result.readUnsignedByte();
                            var header4:int = result.readUnsignedByte();
                            if (header1 == 0 && header2 == 1 && header3 == 2 && header4 == 3) {
                                loader.onResult(new ByteArray());
                                break;
                            }
                            result.position = 0;
                        }
                        loader.onResult(result);
                    }
                }
                delete _loaders[loaderId];
                break;
            }
        }
    }

    private function handleWebSocketEvent(param1:StatusEvent):void {
        var codeSplit:Array = param1.code.split(";");
        var dataSplit:Array = param1.level.split(";");
        var webSocketId:String = codeSplit[1];
        var ws:AneWebSocket = getWebSocket(webSocketId);
        if (!ws) {
            return;
        }
        switch (codeSplit[0]) {
            case "connected":
                var headersBase64:String = dataSplit[0];
                var headers:Dictionary = decodeHeaders(headersBase64);
                ws.AneAwesomeUtilsInternal::onConnect(headers);
                break;
            case "nextMessage":
                var bytes:ByteArray = _extContext.call("awesomeUtils_getWebSocketByteArrayMessage", webSocketId) as ByteArray;
                if (!bytes)
                    break;
                bytes.position = 0;
                ws.AneAwesomeUtilsInternal::onData(bytes);
                break;
            case "disconnected":
                var responseCode:int = int(dataSplit[2]);
                headersBase64 = dataSplit[3];
                headers = decodeHeaders(headersBase64);
                ws.AneAwesomeUtilsInternal::onClose(dataSplit[0], responseCode, headers);
                break;
            case "error":
                ws.AneAwesomeUtilsInternal::onIoError(param1.level);
                break;
            default:
                throw new Error("TODO: handle StatusEvent code = [" + param1.code + "], level = [" + param1.level + "]");
        }
    }

    private function handleLogEvent(param1:StatusEvent):void {
        var codeSplit:Array = param1.code.split(";");
        switch (codeSplit[0]) {
            case "unexpectedShutdown":
                if (_unexpectedShutdownCallback != null) {
                    _unexpectedShutdownCallback(param1.level);
                }
                break;
            case "readComplete":
                if (_logReadCallbacks && _logReadCallbacks.onResult) {
                    var result:ByteArray = _extContext.call("awesomeUtils_getLogResult") as ByteArray;
                    if (result) {
                        result.position = 0;
                        _logReadCallbacks.onResult(result);
                    } else if (_logReadCallbacks.onError) {
                        _logReadCallbacks.onError(new Error("Failed to get log result"));
                    }
                }
                _logReadCallbacks = null;
                break;
            case "readError":
                if (_logReadCallbacks && _logReadCallbacks.onError) {
                    _logReadCallbacks.onError(new Error(param1.level));
                }
                _logReadCallbacks = null;
                break;
            case "bundleReady":
                if (_bundleCallbacks && _bundleCallbacks.onSuccess) {
                    _bundleCallbacks.onSuccess(param1.level);
                }
                _bundleCallbacks = null;
                break;
            case "bundleError":
                if (_bundleCallbacks && _bundleCallbacks.onError) {
                    _bundleCallbacks.onError(new Error(param1.level));
                }
                _bundleCallbacks = null;
                break;
        }
    }

    private function decodeHeaders(headersEncoded:String):Dictionary {
        var decodedHeaders:String = Base64.decode(headersEncoded);
        var headersObj:Object = JSON.parse(decodedHeaders);
        var headers:Dictionary = new Dictionary();
        for (var key:String in headersObj) {
            headers[key] = headersObj[key];
        }
        return headers;
    }

    // ------------------------------------------------------------------
    // Deep profiler capture — Windows-first .aneprof backend.
    //
    //   profilerStart(outputPath, options): Boolean
    //       Opens a native .aneprof event log. This path does not force
    //       AIR Scout telemetry or require .telemetry.cfg.
    //
    //       options.timing                       default true
    //       options.memory                       default false
    //       options.render                       default false, D3D/DXGI render summaries
    //       options.snapshots                    default true
    //       options.frameEvents                  AS3 test bridge option;
    //                                            ignored by native start
    //       options.snapshotIntervalMs           default 0 (manual only)
    //       options.maxLiveAllocationsPerSnapshot default 4096
    //       options.headerJson                   optional full header JSON
    //
    //   profilerStop():                 Boolean
    //   profilerSnapshot(label=null):   Boolean
    //   profilerMarker(name, value):    Boolean
    //   profilerRecordFrame(index, durationMs, allocationCount=0,
    //                       allocationBytes=0, label=null): Boolean
    //   profilerRequestGc():            Boolean
    //   profilerGetStatus():            Object
    //       memoryLeakDiagnosticsReady is true only while memory + free/realloc
    //       hooks are installed for a live capture.
    //       as3LeakDiagnosticsReady is true when the AS3 IMemorySampler hook is
    //       installed directly or chained in front of AIR/Scout/flash.sampler.
    //       Chained/direct-slot diagnostics are exposed as as3ObjectHook* and
    //       as3Sampler* fields.
    //
    // State enum: 0=Idle 1=Starting 2=Recording 3=Stopping 4=Error.
    // Windows x86/x64 are supported today. Android/Mac/iOS return false.
    // ------------------------------------------------------------------

    public static const PROFILER_STATE_IDLE:uint      = 0;
    public static const PROFILER_STATE_STARTING:uint  = 1;
    public static const PROFILER_STATE_RECORDING:uint = 2;
    public static const PROFILER_STATE_STOPPING:uint  = 3;
    public static const PROFILER_STATE_ERROR:uint     = 4;

    public function profilerStart(outputPath:String, options:Object = null):Boolean {
        if (!_successInit) return false;
        if (!IsWindows()) return false;

        var timing:Boolean = options != null && options.timing !== undefined
                ? Boolean(options.timing) : true;
        var memory:Boolean = options != null && options.memory !== undefined
                ? Boolean(options.memory) : false;
        var render:Boolean = options != null && options.render !== undefined
                ? Boolean(options.render) : false;
        var snapshots:Boolean = options != null && options.snapshots !== undefined
                ? Boolean(options.snapshots) : true;
        var snapshotIntervalMs:uint = options != null && options.snapshotIntervalMs !== undefined
                ? uint(options.snapshotIntervalMs) : 0;
        var maxLive:uint = options != null && options.maxLiveAllocationsPerSnapshot !== undefined
                ? uint(options.maxLiveAllocationsPerSnapshot) : 4096;
        var headerJson:String = options != null && options.headerJson !== undefined
                ? String(options.headerJson)
                : buildProfilerHeaderJson(options, timing, memory, render, snapshots,
                                          snapshotIntervalMs, maxLive);

        var ok:Boolean = _extContext.call("awesomeUtils_profilerStart",
                                          outputPath,
                                          headerJson,
                                          timing,
                                          memory,
                                          snapshots,
                                          maxLive,
                                          snapshotIntervalMs,
                                          render) as Boolean;
        if (!ok) return false;
        return true;
    }

    public function profilerStop():Boolean {
        if (!_successInit) return false;
        if (!IsWindows()) return false;
        var ok:Boolean = _extContext.call("awesomeUtils_profilerStop") as Boolean;
        return ok;
    }

    public function profilerSnapshot(label:String = null):Boolean {
        if (!_successInit) return false;
        if (!IsWindows()) return false;
        return _extContext.call("awesomeUtils_profilerSnapshot", label == null ? "" : label) as Boolean;
    }

    public function profilerGetStatus():Object {
        if (!_successInit) return null;
        if (!IsWindows()) return null;
        return _extContext.call("awesomeUtils_profilerGetStatus") as Object;
    }

    public function profilerMarker(name:String, value:* = null):Boolean {
        if (!_successInit) return false;
        if (IsWindows()) {
            return _extContext.call("awesomeUtils_profilerMarker",
                                    name,
                                    profilerValueToJson(value)) as Boolean;
        }
        return true;
    }

    public function profilerRequestGc():Boolean {
        if (!_successInit) return false;
        if (!IsWindows()) return false;
        return _extContext.call("awesomeUtils_profilerRequestGc") as Boolean;
    }

    public function profilerRecordFrame(frameIndex:Number,
                                        durationMs:Number,
                                        allocationCount:uint = 0,
                                        allocationBytes:Number = 0,
                                        label:String = null):Boolean {
        if (!_successInit) return false;
        if (!IsWindows()) return false;
        var durationNs:Number = Math.max(0, durationMs) * 1000000;
        return _extContext.call("awesomeUtils_profilerRecordFrame",
                                frameIndex,
                                durationNs,
                                allocationCount,
                                allocationBytes,
                                label == null ? "" : label) as Boolean;
    }

    public function profilerTakeMarker(name:String):Boolean {
        return profilerMarker(name, 1);
    }

    internal function profilerRecordAllocForTest(ptr:Number, size:Number):Boolean {
        if (!_successInit) return false;
        if (!IsWindows()) return false;
        return _extContext.call("awesomeUtils_profilerRecordAlloc", ptr, size) as Boolean;
    }

    internal function profilerRecordFreeForTest(ptr:Number):Boolean {
        if (!_successInit) return false;
        if (!IsWindows()) return false;
        return _extContext.call("awesomeUtils_profilerRecordFree", ptr) as Boolean;
    }

    public function profilerSpan(name:String, fn:Function):void {
        profilerMarker(name + ".start");
        try {
            fn();
        } finally {
            profilerMarker(name + ".end");
        }
    }

    private function buildProfilerHeaderJson(options:Object,
                                             timing:Boolean,
                                             memory:Boolean,
                                             render:Boolean,
                                             snapshots:Boolean,
                                             snapshotIntervalMs:uint,
                                             maxLive:uint):String {
        var header:Object = {
            format: "aneprof",
            formatVersion: 1,
            backend: "deep-native",
            platform: "windows",
            airSdk: "51.1.3.10",
            timing: timing,
            memory: memory,
            render: render,
            snapshots: snapshots,
            snapshotIntervalMs: snapshotIntervalMs,
            maxLiveAllocationsPerSnapshot: maxLive
        };
        if (options != null && options.metadata !== undefined) {
            header.metadata = options.metadata;
        }
        return JSON.stringify(header);
    }

    private function profilerValueToJson(value:*):String {
        if (value === undefined || value === null) return "null";
        try {
            return JSON.stringify(value);
        } catch (e:Error) {
            return JSON.stringify(String(value));
        }
        return "null";
    }
}
}
