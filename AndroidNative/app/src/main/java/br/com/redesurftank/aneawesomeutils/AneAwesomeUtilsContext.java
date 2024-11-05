package br.com.redesurftank.aneawesomeutils;

import static br.com.redesurftank.aneawesomeutils.AneAwesomeUtilsExtension.TAG;

import android.content.ContentResolver;
import android.provider.Settings;
import android.util.JsonReader;

import androidx.annotation.NonNull;

import com.adobe.fre.FREByteArray;
import com.adobe.fre.FREContext;
import com.adobe.fre.FREFunction;
import com.adobe.fre.FREObject;

import java.io.IOException;
import java.net.InetAddress;
import java.net.ProtocolException;
import java.net.UnknownHostException;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Queue;
import java.util.UUID;
import java.util.concurrent.ConcurrentLinkedQueue;

import okhttp3.Call;
import okhttp3.Callback;
import okhttp3.Dns;
import okhttp3.HttpUrl;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.internal.ws.RealWebSocket;
import okhttp3.internal.ws.WebSocketProtocol;
import okio.ByteString;

public class AneAwesomeUtilsContext extends FREContext {

    private OkHttpClient _client;
    private final Map<UUID, byte[]> _urlLoaderResults = new HashMap<>();
    private final Map<UUID, RealWebSocket> _webSockets = new HashMap<>();
    private final Queue<byte[]> _byteBufferQueue = new ConcurrentLinkedQueue<>();


    @Override
    public Map<String, FREFunction> getFunctions() {
        AneAwesomeUtilsLogging.i(TAG, "Creating function Map");
        Map<String, FREFunction> functionMap = new HashMap<>();

        functionMap.put(Initialize.KEY, new Initialize());
        functionMap.put(CreateWebSocket.KEY, new CreateWebSocket());
        functionMap.put(SendWebSocketMessage.KEY, new SendWebSocketMessage());
        functionMap.put(CloseWebSocket.KEY, new CloseWebSocket());
        functionMap.put(ConnectWebSocket.KEY, new ConnectWebSocket());
        functionMap.put(AddStaticHost.KEY, new AddStaticHost());
        functionMap.put(RemoveStaticHost.KEY, new RemoveStaticHost());
        functionMap.put(LoadUrl.KEY, new LoadUrl());
        functionMap.put(GetLoaderResult.KEY, new GetLoaderResult());
        functionMap.put(GetWebSocketByteArrayMessage.KEY, new GetWebSocketByteArrayMessage());
        functionMap.put(GetDeviceUniqueId.KEY, new GetDeviceUniqueId());

        return Collections.unmodifiableMap(functionMap);
    }

    @Override
    public void dispose() {
        // Cleanup logic if needed
    }

    public void dispatchWebSocketEvent(String guid, String code, String level) {
        String fullCode = "web-socket;" + code + ";" + guid;
        dispatchStatusEventAsync(fullCode, level);
    }

    public void dispatchUrlLoaderEvent(String guid, String code, String level) {
        String fullCode = "url-loader;" + code + ";" + guid;
        dispatchStatusEventAsync(fullCode, level);
    }

    public static class Initialize implements FREFunction {
        public static final String KEY = "awesomeUtils_initialize";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.i(TAG, "awesomeUtils_initialize");
            try {
                AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;
                ctx._client = new OkHttpClient.Builder().fastFallback(true).dns(new Dns() {
                    @NonNull
                    @Override
                    public List<InetAddress> lookup(@NonNull String s) throws UnknownHostException {
                        return InternalDnsResolver.getInstance().resolveHost(s);
                    }
                }).build();

                return FREObject.newObject(true);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error initializing", e);
            }
            return null;
        }
    }

    public static class CreateWebSocket implements FREFunction {
        public static final String KEY = "awesomeUtils_createWebSocket";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.i(TAG, "awesomeUtils_createWebSocket");
            try {
                UUID uuid = UUID.randomUUID();
                return FREObject.newObject(uuid.toString());
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error creating web socket", e);
            }
            return null;
        }
    }

    public static class SendWebSocketMessage implements FREFunction {
        public static final String KEY = "awesomeUtils_sendWebSocketMessage";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.i(TAG, "awesomeUtils_sendWebSocketMessage");

            try {
                AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;
                UUID uuid = UUID.fromString(args[0].getAsString());
                RealWebSocket webSocket = ctx._webSockets.get(uuid);
                if (webSocket == null) {
                    AneAwesomeUtilsLogging.e(TAG, "Web socket not found");
                    return null;
                }

                if (args[2] instanceof FREByteArray) {
                    FREByteArray byteArray = (FREByteArray) args[2];
                    byteArray.acquire();
                    byte[] bytes = new byte[(int) byteArray.getLength()];
                    byteArray.getBytes().get(bytes);
                    byteArray.release();
                    webSocket.send(ByteString.of(bytes));
                } else {
                    String message = args[2].getAsString();
                    webSocket.send(message);
                }

            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error sending web socket message", e);
            }
            return null;
        }
    }

    public static class CloseWebSocket implements FREFunction {
        public static final String KEY = "awesomeUtils_closeWebSocket";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.i(TAG, "awesomeUtils_closeWebSocket");

            try {
                AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;
                UUID uuid = UUID.fromString(args[0].getAsString());
                WebSocket webSocket = ctx._webSockets.get(uuid);
                if (webSocket == null) {
                    AneAwesomeUtilsLogging.e(TAG, "Web socket not found");
                    return null;
                }
                webSocket.close(1000, "User closed");
                ctx._webSockets.remove(uuid);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error closing web socket", e);
            }
            return null;
        }
    }

    public static class ConnectWebSocket implements FREFunction {
        public static final String KEY = "awesomeUtils_connectWebSocket";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.i(TAG, "awesomeUtils_connectWebSocket");

            try {
                AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;
                UUID uuid = UUID.fromString(args[0].getAsString());
                String url = args[1].getAsString();

                if (ctx._webSockets.containsKey(uuid)) {
                    return null;
                }

                WebSocket webSocket = ctx._client.newWebSocket(new Request.Builder().url(url).build(), new okhttp3.WebSocketListener() {
                    @Override
                    public void onOpen(@NonNull WebSocket webSocket, @NonNull Response response) {
                        AneAwesomeUtilsLogging.i(TAG, "WebSocket opened");
                        ctx.dispatchWebSocketEvent(uuid.toString(), "connected", "");
                    }

                    @Override
                    public void onMessage(@NonNull WebSocket webSocket, @NonNull String text) {
                        AneAwesomeUtilsLogging.i(TAG, "WebSocket text message: " + text);
                        AneAwesomeUtilsLogging.e(TAG, "Text message not supported");
                    }

                    @Override
                    public void onMessage(@NonNull WebSocket webSocket, @NonNull ByteString bytes) {
                        AneAwesomeUtilsLogging.d(TAG, "WebSocket binary message: " + bytes.hex());
                        ctx._byteBufferQueue.add(bytes.toByteArray());
                        ctx.dispatchWebSocketEvent(uuid.toString(), "nextMessage", "");
                    }

                    @Override
                    public void onClosing(@NonNull WebSocket webSocket, int code, @NonNull String reason) {
                        AneAwesomeUtilsLogging.i(TAG, "WebSocket closing: " + code + " " + reason);
                    }

                    @Override
                    public void onClosed(@NonNull WebSocket webSocket, int code, @NonNull String reason) {
                        AneAwesomeUtilsLogging.i(TAG, "WebSocket closed: " + code + " " + reason);
                        ctx.dispatchWebSocketEvent(uuid.toString(), "disconnected", code + ";" + reason);
                        ctx._webSockets.remove(uuid);
                    }

                    @Override
                    public void onFailure(@NonNull WebSocket webSocket, @NonNull Throwable t, @NonNull Response response) {
                        AneAwesomeUtilsLogging.e(TAG, "WebSocket failure", t);
                        if (t instanceof ProtocolException) {
                            ProtocolException protocolException = (ProtocolException) t;
                            //template: Code must be in range [1000,5000): 6000
                            String message = protocolException.getMessage();
                            if (message != null && message.contains("Code must be in range [1000,5000):")) {
                                String[] parts = message.split(":");
                                if (parts.length > 1) {
                                    String[] parts2 = parts[1].split(" ");
                                    if (parts2.length > 1) {
                                        ctx.dispatchWebSocketEvent(uuid.toString(), "disconnected", parts2[1]);
                                        ctx._webSockets.remove(uuid);
                                        return;
                                    }
                                }
                            }
                        }
                        ctx.dispatchWebSocketEvent(uuid.toString(), "disconnected", "1005;Unknown error");
                        ctx._webSockets.remove(uuid);
                    }
                });

                ctx._webSockets.put(uuid, (RealWebSocket) webSocket);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error connecting web socket", e);
            }
            return null;
        }
    }

    public static class AddStaticHost implements FREFunction {
        public static final String KEY = "awesomeUtils_addStaticHost";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.i(TAG, "awesomeUtils_addStaticHost");

            try {
                String host = args[0].getAsString();
                String ip = args[1].getAsString();
                InternalDnsResolver.getInstance().addStaticHost(host, ip);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error adding static host", e);
            }
            return null;
        }
    }

    public static class RemoveStaticHost implements FREFunction {
        public static final String KEY = "awesomeUtils_removeStaticHost";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.i(TAG, "awesomeUtils_removeStaticHost");

            try {
                String host = args[0].getAsString();
                InternalDnsResolver.getInstance().removeStaticHost(host);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error removing static host", e);
            }
            return null;
        }
    }

    public static class LoadUrl implements FREFunction {
        public static final String KEY = "awesomeUtils_loadUrl";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.i(TAG, "awesomeUtils_loadUrl");
            try {
                AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;

                String url = args[0].getAsString();
                String method = args[1].getAsString();
                String variablesJson = args[2].getAsString();
                String headersJson = args[3].getAsString();

                AneAwesomeUtilsLogging.i(TAG, "URL: " + url);
                AneAwesomeUtilsLogging.i(TAG, "Method: " + method);
                AneAwesomeUtilsLogging.i(TAG, "Variables: " + variablesJson);
                AneAwesomeUtilsLogging.i(TAG, "Headers: " + headersJson);

                Map<String, String> variables = new HashMap<>();
                JsonReader reader = new JsonReader(new java.io.StringReader(variablesJson));
                if (!variablesJson.isEmpty()) {
                    reader.beginObject();
                    while (reader.hasNext()) {
                        String name = reader.nextName();
                        String value = reader.nextString();
                        variables.put(name, value);
                    }
                    reader.endObject();
                }
                reader.close();

                Map<String, String> headers = new HashMap<>();
                reader = new JsonReader(new java.io.StringReader(headersJson));
                if (!headersJson.isEmpty()) {
                    reader.beginObject();
                    while (reader.hasNext()) {
                        String name = reader.nextName();
                        String value = reader.nextString();
                        headers.put(name, value);
                    }
                    reader.endObject();
                }
                reader.close();

                UUID uuid = UUID.randomUUID();

                Request.Builder requestBuilder = new Request.Builder();

                if (method.equals("GET")) {
                    okhttp3.HttpUrl.Builder urlBuilder = Objects.requireNonNull(HttpUrl.parse(url)).newBuilder();
                    for (Map.Entry<String, String> entry : variables.entrySet()) {
                        urlBuilder.addQueryParameter(entry.getKey(), entry.getValue());
                    }
                    requestBuilder.url(urlBuilder.build());
                } else if (method.equals("POST")) {
                    okhttp3.FormBody.Builder formBuilder = new okhttp3.FormBody.Builder();
                    for (Map.Entry<String, String> entry : variables.entrySet()) {
                        formBuilder.add(entry.getKey(), entry.getValue());
                    }
                    requestBuilder.url(url).post(formBuilder.build());
                }

                for (Map.Entry<String, String> entry : headers.entrySet()) {
                    requestBuilder.addHeader(entry.getKey(), entry.getValue());
                }

                Request request = requestBuilder.build();
                ctx._client.newCall(request).enqueue(new Callback() {
                    @Override
                    public void onFailure(@NonNull Call call, @NonNull IOException e) {
                        AneAwesomeUtilsLogging.e(TAG, "Error loading url", e);
                        ctx.dispatchUrlLoaderEvent(uuid.toString(), "ërror", e.getMessage());
                    }

                    @Override
                    public void onResponse(@NonNull Call call, @NonNull Response response) throws IOException {
                        try {
                            int statusCode = response.code();
                            AneAwesomeUtilsLogging.i(TAG, "URL: " + url + " Status code: " + statusCode);
                            if (statusCode >= 400) {
                                ctx.dispatchUrlLoaderEvent(uuid.toString(), "error", "Invalid status code: " + statusCode);
                                return;
                            }
                            byte[] bytes = response.body().bytes();
                            ctx._urlLoaderResults.put(uuid, bytes);
                            ctx.dispatchUrlLoaderEvent(uuid.toString(), "success", "");
                        } catch (Exception e) {
                            AneAwesomeUtilsLogging.e(TAG, "Error loading url", e);
                            ctx.dispatchUrlLoaderEvent(uuid.toString(), "error", e.getMessage());
                        }
                    }
                });

                return FREObject.newObject(uuid.toString());

            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error loading url", e);
            }

            return null;
        }
    }

    public static class GetLoaderResult implements FREFunction {
        public static final String KEY = "awesomeUtils_getLoaderResult";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.i(TAG, "awesomeUtils_getLoaderResult");
            try {
                AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;

                UUID uuid = UUID.fromString(args[0].getAsString());
                byte[] bytes = ctx._urlLoaderResults.get(uuid);
                if (bytes != null) {
                    ctx._urlLoaderResults.remove(uuid);
                    FREByteArray byteArray = FREByteArray.newByteArray(bytes.length);
                    byteArray.acquire();
                    byteArray.getBytes().put(bytes);
                    byteArray.release();
                    return byteArray;
                }
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error getting result", e);
            }

            return null;
        }
    }

    public static class GetWebSocketByteArrayMessage implements FREFunction {
        public static final String KEY = "awesomeUtils_getWebSocketByteArrayMessage";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.i(TAG, "awesomeUtils_getWebSocketByteArrayMessage");

            try {
                AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;
                byte[] bytes = ctx._byteBufferQueue.poll();
                if (bytes != null) {
                    FREByteArray byteArray = FREByteArray.newByteArray(bytes.length);
                    byteArray.acquire();
                    byteArray.getBytes().put(bytes);
                    byteArray.release();
                    return byteArray;
                }
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error getting web socket message", e);
            }
            return null;
        }
    }

    public static class GetDeviceUniqueId implements FREFunction {
        public static final String KEY = "awesomeUtils_getDeviceUniqueId";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.i(TAG, "awesomeUtils_getDeviceUniqueId");
            try {
                ContentResolver contentResolver = context.getActivity().getContentResolver();
                String androidId = Settings.Secure.getString(contentResolver, Settings.Secure.ANDROID_ID);
                return FREObject.newObject(androidId);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error getting device unique id", e);
            }
            return null;
        }
    }
}
