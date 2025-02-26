package {
import air.net.WebSocket;

import flash.events.Event;
import flash.events.IOErrorEvent;
import flash.events.WebSocketEvent;
import flash.net.Socket;
import flash.utils.ByteArray;

use namespace AneAwesomeUtilsInternal;

public class AneWebSocket extends WebSocket {

    private var _id:String;
    private var _closeReason:int = -1;
    private var _dispatchedOnConnect:Boolean = false;
    private var _dispatchedOnClose:Boolean = false;

    public function AneWebSocket(id:String) {
        super();
        _id = id;
    }

    public function get id():String {
        return _id;
    }

    override public function startServer(param1:Socket):void {
        throw new Error("AndroidWebSocket cannot take over an existing AS3 Socket object");
    }

    override public function get closeReason():int {
        return _closeReason;
    }

    override public function sendMessage(param1:uint, param2:*):void {
        AneAwesomeUtils.instance.sendWebSocketMessage(_id, param1, param2);
    }

    override public function close(param1:uint = 1000):void {
        AneAwesomeUtils.instance.closeWebSocket(_id, param1);
    }

    override public function connect(param1:String, param2:Vector.<String> = null):void {
        AneAwesomeUtils.instance.connectWebSocket(_id, param1);
    }

    public function dispose():void {
        AneAwesomeUtils.instance.closeWebSocket(_id, 1006);
        AneAwesomeUtils.instance.removeWebSocket(this);
    }

    AneAwesomeUtilsInternal function onConnect():void {
        if (_dispatchedOnConnect) {
            return;
        }
        _dispatchedOnConnect = true;
        dispatchEvent(new Event("connect"));
    }

    AneAwesomeUtilsInternal function onData(bytes:ByteArray):void {
       dispatchEvent(new WebSocketEvent("websocketData", WebSocket.fmtBINARY, bytes));
    }

    AneAwesomeUtilsInternal function onClose(closeReason:int):void {
        if (_dispatchedOnClose) {
            return;
        }
        _dispatchedOnClose = true;
        _closeReason = closeReason;
        dispatchEvent(new Event("close"));
    }

    AneAwesomeUtilsInternal function onIoError(error:String):void {
        if (_dispatchedOnClose) {
            return;
        }
        _dispatchedOnClose = true;
        _closeReason = 1006;
        dispatchEvent(new IOErrorEvent("ioError", false, false, error));
    }
}
}
