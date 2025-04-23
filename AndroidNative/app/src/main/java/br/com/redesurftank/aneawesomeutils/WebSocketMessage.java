package br.com.redesurftank.aneawesomeutils;

import okhttp3.WebSocket;
import okio.ByteString;

public class WebSocketMessage {
    enum Type { TEXT, BINARY }

    private final Type type;
    private final String textMessage;
    private final byte[] binaryMessage;

    public WebSocketMessage(String textMessage) {
        this.type = Type.TEXT;
        this.textMessage = textMessage;
        this.binaryMessage = null;
    }

    public WebSocketMessage(byte[] binaryMessage) {
        this.type = Type.BINARY;
        this.textMessage = null;
        this.binaryMessage = binaryMessage;
    }

    public void send(WebSocket webSocket) {
        if (type == Type.TEXT && textMessage != null) {
            webSocket.send(textMessage);
        } else if (type == Type.BINARY && binaryMessage != null) {
            webSocket.send(ByteString.of(binaryMessage));
        }
    }
}

