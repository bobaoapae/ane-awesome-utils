package {
import air.net.WebSocket;

import flash.events.Event;
import flash.events.IOErrorEvent;
import flash.events.StatusEvent;
import flash.events.WebSocketEvent;
import flash.external.ExtensionContext;
import flash.net.Socket;
import flash.system.Capabilities;
import flash.utils.ByteArray;

public class AneWebSocket extends WebSocket {

    private var _id:String;
    private var _closeReason:int = -1;

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

    public function set closeReason(value:int):void {
        _closeReason = value;
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
}
}
