package br.com.redesurftank.aneawesomeutils;

import static br.com.redesurftank.aneawesomeutils.AneAwesomeUtilsExtension.TAG;

import android.content.ContentResolver;
import android.os.Build;
import android.provider.Settings;
import android.util.Base64;
import android.util.JsonReader;
import android.util.JsonWriter;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import com.adobe.fre.FREASErrorException;
import com.adobe.fre.FREArray;
import com.adobe.fre.FREByteArray;
import com.adobe.fre.FREContext;
import com.adobe.fre.FREFunction;
import com.adobe.fre.FREInvalidObjectException;
import com.adobe.fre.FRENoSuchNameException;
import com.adobe.fre.FREObject;
import com.adobe.fre.FREReadOnlyException;
import com.adobe.fre.FRETypeMismatchException;
import com.adobe.fre.FREWrongThreadException;
import com.fasterxml.aalto.stax.InputFactoryImpl;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.StringReader;
import java.io.StringWriter;
import java.net.InetAddress;
import java.net.ProtocolException;
import java.net.URI;
import java.net.UnknownHostException;
import java.nio.ByteBuffer;
import java.nio.channels.AsynchronousFileChannel;
import java.nio.file.Files;
import java.security.KeyFactory;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.cert.CertificateException;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Queue;
import java.util.UUID;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import javax.net.ssl.HostnameVerifier;
import javax.net.ssl.KeyManager;
import javax.net.ssl.KeyManagerFactory;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLSession;
import javax.net.ssl.SSLSocketFactory;
import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509KeyManager;
import javax.net.ssl.X509TrustManager;
import javax.xml.stream.XMLInputFactory;
import javax.xml.stream.XMLStreamConstants;
import javax.xml.stream.XMLStreamException;
import javax.xml.stream.XMLStreamReader;

import okhttp3.Call;
import okhttp3.Callback;
import okhttp3.Dns;
import okhttp3.HttpUrl;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.internal.ws.RealWebSocket;
import okio.ByteString;

public class AneAwesomeUtilsContext extends FREContext {

    private OkHttpClient _client;
    private final Map<UUID, byte[]> _urlLoaderResults = new HashMap<>();
    private final Map<UUID, RealWebSocket> _webSockets = new HashMap<>();
    private final Queue<byte[]> _byteBufferQueue = new ConcurrentLinkedQueue<>();

    // Add message queue and processor for WebSocket messages
    private final Map<UUID, ConcurrentLinkedQueue<WebSocketMessage>> _webSocketMessageQueues = new HashMap<>();
    private final Map<UUID, Thread> _webSocketSenderThreads = new HashMap<>();
    private final Object _webSocketLock = new Object();
    private final Executor _backGroundExecutor = Executors.newCachedThreadPool();

    private final Object _clientCertificateLock = new Object();
    private final Map<String, ClientCertificateEntry> _clientCertificates = new HashMap<>();
    private X509TrustManager _defaultTrustManager;
    private SSLSocketFactory _defaultSslSocketFactory;

    @Override
    public Map<String, FREFunction> getFunctions() {
        AneAwesomeUtilsLogging.d(TAG, "Creating function Map");
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
        functionMap.put(DecompressByteArray.KEY, new DecompressByteArray());
        functionMap.put(ReadFileToByteArray.KEY, new ReadFileToByteArray());
        functionMap.put(CheckRunningEmulator.KEY, new CheckRunningEmulator());
        functionMap.put(CheckRunningEmulatorAsync.KEY, new CheckRunningEmulatorAsync());
        functionMap.put(MapXmlToObject.KEY, new MapXmlToObject());
        functionMap.put(AddClientCertificate.KEY, new AddClientCertificate());

        return Collections.unmodifiableMap(functionMap);
    }

    @Override
    public void dispose() {
        // Clean up all WebSocket sender threads
        synchronized (_webSocketLock) {
            for (Thread thread : _webSocketSenderThreads.values()) {
                thread.interrupt();
            }
            _webSocketSenderThreads.clear();
            _webSocketMessageQueues.clear();
        }

        synchronized (_clientCertificateLock) {
            _clientCertificates.clear();
        }

        // Original dispose logic if any
    }

    private void startWebSocketSenderThread(UUID uuid) {
        synchronized (_webSocketLock) {
            if (_webSocketSenderThreads.containsKey(uuid)) {
                return; // Thread already exists
            }

            // Create message queue if it doesn't exist
            if (!_webSocketMessageQueues.containsKey(uuid)) {
                _webSocketMessageQueues.put(uuid, new ConcurrentLinkedQueue<>());
            }

            Thread senderThread = new Thread(() -> {
                AneAwesomeUtilsLogging.d(TAG, "Starting WebSocket sender thread for " + uuid);
                ConcurrentLinkedQueue<WebSocketMessage> queue = _webSocketMessageQueues.get(uuid);

                while (!Thread.currentThread().isInterrupted()) {
                    try {
                        WebSocketMessage message = queue.poll();
                        if (message != null) {
                            WebSocket webSocket = _webSockets.get(uuid);
                            if (webSocket != null) {
                                message.send(webSocket);
                            } else {
                                AneAwesomeUtilsLogging.w(TAG, "WebSocket not found for " + uuid);
                                break; // Exit thread if WebSocket is gone
                            }
                        } else {
                            // Sleep a bit when the queue is empty
                            Thread.sleep(10);
                        }
                    } catch (Exception e) {
                        AneAwesomeUtilsLogging.e(TAG, "Error in WebSocket sender thread", e);
                        // Continue processing other messages even if one fails
                    }
                }

                AneAwesomeUtilsLogging.d(TAG, "WebSocket sender thread ending for " + uuid);
            });

            senderThread.setName("WebSocketSender-" + uuid);
            senderThread.setDaemon(true); // Make it a daemon thread so it doesn't prevent app exit
            senderThread.start();

            _webSocketSenderThreads.put(uuid, senderThread);
        }
    }

    // Stop the WebSocket sender thread for a specific UUID
    private void stopWebSocketSenderThread(UUID uuid) {
        synchronized (_webSocketLock) {
            Thread thread = _webSocketSenderThreads.remove(uuid);
            if (thread != null) {
                thread.interrupt();
            }
            _webSocketMessageQueues.remove(uuid);
        }
    }

    public void dispatchWebSocketEvent(String guid, String code, String level) {
        String fullCode = "web-socket;" + code + ";" + guid;
        try {
            dispatchStatusEventAsync(fullCode, level);
        } catch (Exception e) {
            AneAwesomeUtilsLogging.e(TAG, "Error dispatching web socket event", e);
        }
    }

    public void dispatchUrlLoaderEvent(String guid, String code, String level) {
        String fullCode = "url-loader;" + code + ";" + guid;
        try {
            dispatchStatusEventAsync(fullCode, level);
        } catch (Exception e) {
            AneAwesomeUtilsLogging.e(TAG, "Error dispatching url loader event", e);
        }
    }

    private void ensureDefaultTrust() {
        if (_defaultTrustManager != null && _defaultSslSocketFactory != null) {
            return;
        }
        try {
            TrustManagerFactory tmf = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
            tmf.init((KeyStore) null);
            TrustManager[] trustManagers = tmf.getTrustManagers();
            for (TrustManager tm : trustManagers) {
                if (tm instanceof X509TrustManager) {
                    _defaultTrustManager = (X509TrustManager) tm;
                    break;
                }
            }
            if (_defaultTrustManager == null) {
                throw new IllegalStateException("No default X509TrustManager found");
            }
            SSLContext sslContext = SSLContext.getInstance("TLS");
            sslContext.init(null, new TrustManager[]{_defaultTrustManager}, null);
            _defaultSslSocketFactory = sslContext.getSocketFactory();
        } catch (Exception e) {
            AneAwesomeUtilsLogging.e(TAG, "Error initializing default trust manager", e);
        }
    }

    private OkHttpClient getClientForHost(String host) {
        if (host == null || host.isEmpty()) {
            return _client;
        }
        ClientCertificateEntry entry = null;
        synchronized (_clientCertificateLock) {
            entry = _clientCertificates.get(host.toLowerCase());
        }
        if (entry == null) {
            return _client;
        }
        try {
            return _client.newBuilder().sslSocketFactory(entry.sslSocketFactory, _defaultTrustManager).build();
        } catch (Exception e) {
            AneAwesomeUtilsLogging.e(TAG, "Error building client for host with client certificate", e);
            return _client;
        }
    }

    private static class ClientCertificateEntry {
        final X509KeyManager keyManager;
        final SSLSocketFactory sslSocketFactory;

        ClientCertificateEntry(X509KeyManager keyManager, SSLSocketFactory sslSocketFactory) {
            this.keyManager = keyManager;
            this.sslSocketFactory = sslSocketFactory;
        }
    }

    private static class PemUtils {
        private static final Pattern CERT_PATTERN = Pattern.compile("-----BEGIN CERTIFICATE-----([^-]+)-----END CERTIFICATE-----");
        private static final Pattern PKCS8_KEY_PATTERN = Pattern.compile("-----BEGIN PRIVATE KEY-----([^-]+)-----END PRIVATE KEY-----", Pattern.DOTALL);
        private static final Pattern PKCS1_KEY_PATTERN = Pattern.compile("-----BEGIN RSA PRIVATE KEY-----([^-]+)-----END RSA PRIVATE KEY-----", Pattern.DOTALL);

        static List<X509Certificate> parseCertificates(String pem) throws Exception {
            Matcher matcher = CERT_PATTERN.matcher(pem.replace("\r", ""));
            List<X509Certificate> certificates = new ArrayList<>();
            CertificateFactory factory = CertificateFactory.getInstance("X.509");
            while (matcher.find()) {
                String base64 = matcher.group(1).replaceAll("\\s+", "");
                byte[] der = Base64.decode(base64, Base64.DEFAULT);
                certificates.add((X509Certificate) factory.generateCertificate(new java.io.ByteArrayInputStream(der)));
            }
            if (certificates.isEmpty()) {
                throw new CertificateException("No certificates found in PEM");
            }
            return certificates;
        }

        static PrivateKey parsePrivateKey(String pem) throws Exception {
            String normalized = pem.replace("\r", "");
            Matcher pkcs8 = PKCS8_KEY_PATTERN.matcher(normalized);
            if (pkcs8.find()) {
                byte[] der = Base64.decode(pkcs8.group(1).replaceAll("\\s+", ""), Base64.DEFAULT);
                return buildPrivateKeyFromPkcs8(der);
            }
            Matcher pkcs1 = PKCS1_KEY_PATTERN.matcher(normalized);
            if (pkcs1.find()) {
                byte[] der = Base64.decode(pkcs1.group(1).replaceAll("\\s+", ""), Base64.DEFAULT);
                byte[] pkcs8Bytes = wrapPkcs1ToPkcs8(der);
                return buildPrivateKeyFromPkcs8(pkcs8Bytes);
            }
            throw new CertificateException("No private key found in PEM");
        }

        private static PrivateKey buildPrivateKeyFromPkcs8(byte[] der) throws Exception {
            PKCS8EncodedKeySpec keySpec = new PKCS8EncodedKeySpec(der);
            try {
                return KeyFactory.getInstance("RSA").generatePrivate(keySpec);
            } catch (Exception ignored) {
            }
            try {
                return KeyFactory.getInstance("EC").generatePrivate(keySpec);
            } catch (Exception ignored) {
            }
            return KeyFactory.getInstance("DSA").generatePrivate(keySpec);
        }

        private static byte[] wrapPkcs1ToPkcs8(byte[] pkcs1Bytes) throws IOException {
            // PKCS#8 header for RSA: SEQUENCE {INTEGER 0, SEQ(OID rsaEncryption, NULL), OCTET STRING (PKCS#1 key)}
            byte[] rsaAlgorithmId = new byte[]{0x30, 0x0d, 0x06, 0x09, 0x2a, (byte) 0x86, 0x48, (byte) 0x86, (byte) 0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00};
            ByteArrayOutputStream inner = new ByteArrayOutputStream();
            inner.write(0x02); // INTEGER
            inner.write(0x01);
            inner.write(0x00); // version 0
            inner.write(rsaAlgorithmId);
            inner.write(0x04); // OCTET STRING
            writeDerLength(inner, pkcs1Bytes.length);
            inner.write(pkcs1Bytes);
            byte[] innerBytes = inner.toByteArray();

            ByteArrayOutputStream out = new ByteArrayOutputStream();
            out.write(0x30); // SEQUENCE
            writeDerLength(out, innerBytes.length);
            out.write(innerBytes);
            return out.toByteArray();
        }

        private static void writeDerLength(ByteArrayOutputStream out, int length) {
            if (length < 0x80) {
                out.write(length);
            } else {
                int numBytes = Integer.BYTES - Integer.numberOfLeadingZeros(length) / 8;
                out.write(0x80 | numBytes);
                for (int i = numBytes - 1; i >= 0; i--) {
                    out.write((length >> (8 * i)) & 0xFF);
                }
            }
        }
    }

    private ClientCertificateEntry buildClientCertificate(String domain, String pem) throws Exception {
        ensureDefaultTrust();
        List<X509Certificate> certificates = PemUtils.parseCertificates(pem);
        PrivateKey privateKey = PemUtils.parsePrivateKey(pem);

        KeyStore keyStore = KeyStore.getInstance("PKCS12");
        keyStore.load(null, null);
        X509Certificate[] chain = certificates.toArray(new X509Certificate[0]);
        keyStore.setKeyEntry("client", privateKey, new char[0], chain);

        KeyManagerFactory kmf = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
        kmf.init(keyStore, new char[0]);
        KeyManager[] kms = kmf.getKeyManagers();
        X509KeyManager keyManager = null;
        for (KeyManager km : kms) {
            if (km instanceof X509KeyManager) {
                keyManager = (X509KeyManager) km;
                break;
            }
        }
        if (keyManager == null) {
            throw new IllegalStateException("No X509KeyManager available");
        }

        SSLContext sslContext = SSLContext.getInstance("TLS");
        sslContext.init(new KeyManager[]{keyManager}, new TrustManager[]{_defaultTrustManager}, null);
        return new ClientCertificateEntry(keyManager, sslContext.getSocketFactory());
    }

    private boolean registerClientCertificate(String domain, String pem) {
        if (domain == null || domain.isEmpty() || pem == null || pem.isEmpty()) {
            AneAwesomeUtilsLogging.e(TAG, "Domain or PEM content is empty");
            return false;
        }
        try {
            ClientCertificateEntry entry = buildClientCertificate(domain, pem);
            synchronized (_clientCertificateLock) {
                _clientCertificates.put(domain.toLowerCase(), entry);
            }
            return true;
        } catch (Exception e) {
            AneAwesomeUtilsLogging.e(TAG, "Error registering client certificate for domain: " + domain, e);
            return false;
        }
    }

    public static class Initialize implements FREFunction {
        public static final String KEY = "awesomeUtils_initialize";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_initialize");
            try {
                AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;
                ctx.ensureDefaultTrust();
                OkHttpClient.Builder builder = new OkHttpClient.Builder().fastFallback(true).dns(new Dns() {
                    @NonNull
                    @Override
                    public List<InetAddress> lookup(@NonNull String s) throws UnknownHostException {
                        return InternalDnsResolver.getInstance().resolveHost(s);
                    }
                }).connectionPool(new okhttp3.ConnectionPool(5, 1, TimeUnit.MINUTES)).pingInterval(30, TimeUnit.SECONDS).connectTimeout(5, TimeUnit.SECONDS).addInterceptor(chain -> {
                    Request originalRequest = chain.request();
                    Request requestWithUserAgent = originalRequest.newBuilder().header("User-Agent", originalRequest.header("User-Agent") + " NativeLoader/1.0").build();
                    return chain.proceed(requestWithUserAgent);
                });

                if (ctx._defaultSslSocketFactory != null && ctx._defaultTrustManager != null) {
                    builder.sslSocketFactory(ctx._defaultSslSocketFactory, ctx._defaultTrustManager);
                }
                ctx._client = builder.build();
                return FREObject.newObject(true);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error initializing", e);
            }
            return null;
        }

        private static OkHttpClient.Builder configureToIgnoreCertificate(OkHttpClient.Builder builder) {
            AneAwesomeUtilsLogging.w(TAG, "Ignoring SSL certificate");
            try {

                // Create a trust manager that does not validate certificate chains
                final TrustManager[] trustAllCerts = new TrustManager[]{new X509TrustManager() {
                    @Override
                    public void checkClientTrusted(java.security.cert.X509Certificate[] chain, String authType) throws CertificateException {
                    }

                    @Override
                    public void checkServerTrusted(java.security.cert.X509Certificate[] chain, String authType) throws CertificateException {
                    }

                    @Override
                    public java.security.cert.X509Certificate[] getAcceptedIssuers() {
                        return new java.security.cert.X509Certificate[]{};
                    }
                }};

                // Install the all-trusting trust manager
                final SSLContext sslContext = SSLContext.getInstance("SSL");
                sslContext.init(null, trustAllCerts, new java.security.SecureRandom());
                // Create an ssl socket factory with our all-trusting manager
                final SSLSocketFactory sslSocketFactory = sslContext.getSocketFactory();

                builder.sslSocketFactory(sslSocketFactory, (X509TrustManager) trustAllCerts[0]);
                builder.hostnameVerifier(new HostnameVerifier() {
                    @Override
                    public boolean verify(String hostname, SSLSession session) {
                        return true;
                    }
                });
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error configuring to ignore certificate", e);
            }
            return builder;
        }
    }

    public static class CreateWebSocket implements FREFunction {
        public static final String KEY = "awesomeUtils_createWebSocket";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_createWebSocket");
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
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_sendWebSocketMessage");

            try {
                AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;
                UUID uuid = UUID.fromString(args[0].getAsString());
                RealWebSocket webSocket = ctx._webSockets.get(uuid);
                if (webSocket == null) {
                    AneAwesomeUtilsLogging.e(TAG, "Web socket not found");
                    return null;
                }
                // Get or create the message queue for this WebSocket
                ConcurrentLinkedQueue<WebSocketMessage> queue = ctx._webSocketMessageQueues.get(uuid);
                if (queue == null) {
                    AneAwesomeUtilsLogging.e(TAG, "Message queue not found for WebSocket");
                    return null;
                }

                if (args[2] instanceof FREByteArray) {
                    FREByteArray byteArray = (FREByteArray) args[2];
                    byteArray.acquire();
                    byte[] bytes = new byte[(int) byteArray.getLength()];
                    byteArray.getBytes().get(bytes);
                    byteArray.release();
                    queue.add(new WebSocketMessage(bytes));
                } else {
                    String message = args[2].getAsString();
                    queue.add(new WebSocketMessage(message));
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
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_closeWebSocket");

            try {
                AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;
                UUID uuid = UUID.fromString(args[0].getAsString());
                WebSocket webSocket = ctx._webSockets.get(uuid);
                if (webSocket == null) {
                    AneAwesomeUtilsLogging.e(TAG, "Web socket not found");
                    return null;
                }
                ctx.stopWebSocketSenderThread(uuid);

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
                String headersJson = args[2].getAsString();

                Map<String, String> headers = new HashMap<>();
                JsonReader reader = new JsonReader(new java.io.StringReader(headersJson));
                if (!headersJson.isEmpty()) {
                    reader.beginObject();
                    while (reader.hasNext()) {
                        String name = reader.nextName();
                        String value = reader.nextString();
                        headers.put(name, value);
                    }
                    reader.endObject();
                }


                if (ctx._webSockets.containsKey(uuid)) {
                    return null;
                }

                Request.Builder requestBuilder = new Request.Builder();

                for (Map.Entry<String, String> entry : headers.entrySet()) {
                    requestBuilder.addHeader(entry.getKey(), entry.getValue());
                }

                HttpUrl parsedUrl = HttpUrl.parse(url);
                String host = null;
                if (parsedUrl != null) {
                    host = parsedUrl.host();
                } else {
                    try {
                        URI uri = URI.create(url);
                        host = uri.getHost();
                    } catch (Exception e) {
                        AneAwesomeUtilsLogging.e(TAG, "Invalid WebSocket URL: " + url, e);
                        return null;
                    }
                }

                if (host == null || host.isEmpty()) {
                    AneAwesomeUtilsLogging.e(TAG, "Invalid WebSocket URL (no host): " + url);
                    return null;
                }

                OkHttpClient client = ctx.getClientForHost(host);

                WebSocket webSocket = client.newWebSocket(requestBuilder.url(url).build(), new okhttp3.WebSocketListener() {
                    private Map<String, String> receivedHeaders = new HashMap<>();

                    @Override
                    public void onOpen(@NonNull WebSocket webSocket, @NonNull Response response) {
                        AneAwesomeUtilsLogging.i(TAG, "WebSocket opened");
                        Map<String, String> headers = new HashMap<>();
                        for (String name : response.headers().names()) {
                            headers.put(name, response.header(name));
                        }
                        receivedHeaders = headers;
                        ctx.dispatchWebSocketEvent(uuid.toString(), "connected", getHeadersAsBase64(receivedHeaders));
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
                        handleDisconnect(uuid, reason, code, null);
                    }

                    @Override
                    public void onClosed(@NonNull WebSocket webSocket, int code, @NonNull String reason) {
                        AneAwesomeUtilsLogging.i(TAG, "WebSocket closed: " + code + " " + reason);
                        handleDisconnect(uuid, reason, code, null);
                    }

                    @Override
                    public void onFailure(@NonNull WebSocket webSocket, @NonNull Throwable t, @Nullable Response response) {
                        if (t instanceof ProtocolException) {
                            ProtocolException protocolException = (ProtocolException) t;
                            //template: Code must be in range [1000,5000): 6000
                            String message = protocolException.getMessage();
                            if (message != null && message.contains("Code must be in range [1000,5000):")) {
                                String[] parts = message.split(":");
                                if (parts.length > 1) {
                                    String[] parts2 = parts[1].split(" ");
                                    if (parts2.length > 1) {
                                        int closeCode = Integer.parseInt(parts2[1]);
                                        handleDisconnect(uuid, t.getMessage(), closeCode, response);
                                        return;
                                    }
                                }
                            }
                        }
                        AneAwesomeUtilsLogging.e(TAG, "WebSocket failure", t);
                        handleDisconnect(uuid, t.getMessage(), 0, response);
                    }

                    private void handleDisconnect(UUID id, String reason, int closeCode, @Nullable Response response) {
                        int responseCode = response != null ? response.code() : 0;
                        Map<String, String> headers = new HashMap<>();
                        try {
                            if (response != null) {
                                for (String name : response.headers().names()) {
                                    headers.put(name, response.header(name));
                                }
                            }
                        } catch (Exception e) {
                            AneAwesomeUtilsLogging.e(TAG, "Error getting headers", e);
                        }
                        if (headers.isEmpty()) {
                            headers = receivedHeaders;
                        }
                        ctx.dispatchWebSocketEvent(id.toString(), "disconnected", reason + ";" + closeCode + ";" + responseCode + ";" + getHeadersAsBase64(headers));
                        ctx._webSockets.remove(id);
                    }

                    private String getHeadersAsBase64(@Nullable Map<String, String> headers) {
                        StringWriter stringWriter = new StringWriter();
                        JsonWriter jsonWriter = new JsonWriter(stringWriter);
                        try {
                            jsonWriter.beginObject();
                            if (headers != null) {
                                for (Map.Entry<String, String> entry : headers.entrySet()) {
                                    jsonWriter.name(entry.getKey()).value(entry.getValue());
                                }
                            }
                            jsonWriter.endObject();
                            jsonWriter.close();
                        } catch (IOException e) {
                            AneAwesomeUtilsLogging.e(TAG, "Error writing JSON", e);
                        }
                        String json = stringWriter.toString();
                        return Base64.encodeToString(json.getBytes(), Base64.NO_WRAP);
                    }
                });

                ctx._webSockets.put(uuid, (RealWebSocket) webSocket);
                ctx.startWebSocketSenderThread(uuid);
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
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_addStaticHost");

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
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_removeStaticHost");

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
        private static final ExecutorService FILE_IO = Executors.newCachedThreadPool();
        private static final byte[] EmptyResult = new byte[]{0, 1, 2, 3};
        public static final String KEY = "awesomeUtils_loadUrl";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_loadUrl");
            try {
                UUID uuid = UUID.randomUUID();
                AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;


                String url = args[0].getAsString();
                String method = args[1].getAsString();
                String variablesJson = args[2].getAsString();
                String headersJson = args[3].getAsString();

                AneAwesomeUtilsLogging.d(TAG, "URL: " + url);
                AneAwesomeUtilsLogging.d(TAG, "Method: " + method);
                AneAwesomeUtilsLogging.d(TAG, "Variables: " + variablesJson);
                AneAwesomeUtilsLogging.d(TAG, "Headers: " + headersJson);

                if (url != null && url.startsWith("file://")) {
                    String path = url.replaceFirst("file://", "");
                    FILE_IO.execute(() -> {
                        try {
                            File file = new File(path);
                            if (!file.exists() || !file.isFile()) {
                                ctx.dispatchUrlLoaderEvent(uuid.toString(), "error", "File not found: " + path);
                                return;
                            }
                            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                                try (var openChanel = AsynchronousFileChannel.open(file.toPath())) {
                                    if (!openChanel.isOpen()) {
                                        ctx.dispatchUrlLoaderEvent(uuid.toString(), "error", "Error opening file: " + path);
                                        return;
                                    }
                                    long size = openChanel.size();
                                    if (size > Integer.MAX_VALUE) {
                                        ctx.dispatchUrlLoaderEvent(uuid.toString(), "error", "File too large: " + path);
                                        return;
                                    }
                                    ByteBuffer byteBuffer = ByteBuffer.allocate((int) size);
                                    openChanel.read(byteBuffer, 0).get();
                                    byte[] bytes = byteBuffer.array();
                                    if (bytes.length == 0) bytes = EmptyResult;
                                    ctx._urlLoaderResults.put(uuid, bytes);
                                    ctx.dispatchUrlLoaderEvent(uuid.toString(), "success", "");
                                    return;
                                } catch (Exception e) {
                                    AneAwesomeUtilsLogging.e(TAG, "Error reading file with AsynchronousFileChannel: " + path, e);
                                    ctx.dispatchUrlLoaderEvent(uuid.toString(), "error", "Error reading file: " + e.getMessage());
                                    return;
                                }
                            }
                            ByteArrayOutputStream baos = new ByteArrayOutputStream();
                            FileInputStream fis = new FileInputStream(file);
                            byte[] buffer = new byte[8192];
                            int read;
                            while ((read = fis.read(buffer)) != -1) {
                                baos.write(buffer, 0, read);
                            }
                            fis.close();
                            byte[] bytes = baos.toByteArray();
                            if (bytes.length == 0) bytes = EmptyResult;
                            ctx._urlLoaderResults.put(uuid, bytes);
                            ctx.dispatchUrlLoaderEvent(uuid.toString(), "success", "");
                        } catch (Throwable e) {
                            AneAwesomeUtilsLogging.e(TAG, "Error reading file: " + path, e);
                            ctx.dispatchUrlLoaderEvent(uuid.toString(), "error", e.getMessage());
                        }
                    });
                    return FREObject.newObject(uuid.toString());
                }

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

                Request.Builder requestBuilder = new Request.Builder();

                HttpUrl parsedUrl = HttpUrl.parse(url);
                if (parsedUrl == null) {
                    AneAwesomeUtilsLogging.e(TAG, "Invalid URL: " + url);
                    return null;
                }

                for (Map.Entry<String, String> entry : headers.entrySet()) {
                    requestBuilder.addHeader(entry.getKey(), entry.getValue());
                }

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

                Request request = requestBuilder.build();
                OkHttpClient client = ctx.getClientForHost(parsedUrl.host());
                client.newCall(request).enqueue(new Callback() {
                    @Override
                    public void onFailure(@NonNull Call call, @NonNull IOException e) {
                        AneAwesomeUtilsLogging.e(TAG, "Error loading url:" + url, e);
                        ctx.dispatchUrlLoaderEvent(uuid.toString(), "error", e.getMessage());
                    }

                    @Override
                    public void onResponse(@NonNull Call call, @NonNull Response response) throws IOException {
                        try {
                            int statusCode = response.code();
                            AneAwesomeUtilsLogging.d(TAG, "URL: " + url + " Status code: " + statusCode);
                            if (statusCode >= 400) {
                                ctx.dispatchUrlLoaderEvent(uuid.toString(), "error", "Invalid status code: " + statusCode);
                                return;
                            }
                            byte[] bytes = response.body().bytes();
                            if (bytes.length == 0) {
                                bytes = EmptyResult;
                            }
                            ctx._urlLoaderResults.put(uuid, bytes);
                            ctx.dispatchUrlLoaderEvent(uuid.toString(), "success", "");
                        } catch (Exception e) {
                            AneAwesomeUtilsLogging.e(TAG, "Error loading url:" + url, e);
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
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_getLoaderResult");
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
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_getWebSocketByteArrayMessage");

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
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_getDeviceUniqueId");
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

    public static class DecompressByteArray implements FREFunction {
        public static final String KEY = "awesomeUtils_decompressByteArray";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_decompressByteArray");
            try {
                FREByteArray byteArray = (FREByteArray) args[0];
                byteArray.acquire();
                byte[] bytes = new byte[(int) byteArray.getLength()];
                byteArray.getBytes().get(bytes);
                byteArray.release();

                // Decompress the byte array (assuming it's GZIP compressed)
                byte[] decompressedBytes = AneAwesomeUtilsDecompressor.decompress(bytes);

                FREByteArray output = (FREByteArray) args[1];
                output.setLength(decompressedBytes.length);
                output.acquire();
                output.getBytes().put(decompressedBytes);
                output.release();

            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error decompressing byte array", e);
            }
            return null;
        }
    }

    public static class ReadFileToByteArray implements FREFunction {
        public static final String KEY = "awesomeUtils_readFileToByteArray";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_readFileToByteArray");
            try {
                String filePath = args[0].getAsString();
                File file = new File(filePath);
                if (!file.exists()) {
                    AneAwesomeUtilsLogging.e(TAG, "File not found: " + filePath);
                    return null;
                }

                byte[] bytes = null;
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    bytes = Files.readAllBytes(file.toPath());
                } else {
                    FileInputStream fis = new FileInputStream(file);
                    ByteArrayOutputStream bos = new ByteArrayOutputStream();
                    byte[] buffer = new byte[1024];
                    int bytesRead;
                    while ((bytesRead = fis.read(buffer)) != -1) {
                        bos.write(buffer, 0, bytesRead);
                    }
                    bytes = bos.toByteArray();
                    fis.close();
                }

                FREByteArray output = (FREByteArray) args[1];
                output.setLength(bytes.length);
                output.acquire();
                output.getBytes().put(bytes);
                output.release();

            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error reading file to byte array", e);
            }
            return null;
        }
    }

    public static class CheckRunningEmulator implements FREFunction {
        public static final String KEY = "awesomeUtils_isRunningOnEmulator";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_isRunningOnEmulator");
            try {
                boolean isEmulator = false;
                try {
                    var emulatorDetection = new EmulatorDetection();
                    isEmulator = emulatorDetection.isDetected();
                    if (isEmulator) {
                        var detectionList = emulatorDetection.getResult();
                        AneAwesomeUtilsLogging.i(TAG, "Emulator detected: " + detectionList);
                    }
                } catch (UnsatisfiedLinkError e) {
                    AneAwesomeUtilsLogging.e(TAG, "Emulator detection library not found, assuming not an emulator", e);
                }
                return FREObject.newObject(isEmulator);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error checking if running on emulator", e);
            }
            return null;
        }
    }

    public static class CheckRunningEmulatorAsync implements FREFunction {
        public static final String KEY = "awesomeUtils_isRunningOnEmulatorAsync";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_isRunningOnEmulator");
            AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;
            ctx._backGroundExecutor.execute(() -> {
                try {
                    var emulatorDetection = new EmulatorDetection();
                    boolean isEmulator = emulatorDetection.isDetected();
                    if (isEmulator) {
                        var detectionList = emulatorDetection.getResult();
                        AneAwesomeUtilsLogging.i(TAG, "Emulator detected (async): " + detectionList);
                    }
                    ctx.dispatchStatusEventAsync("emulator-detected;", isEmulator ? "true" : "false");
                } catch (UnsatisfiedLinkError e) {
                    AneAwesomeUtilsLogging.e(TAG, "Emulator detection library not found, assuming not an emulator", e);
                    ctx.dispatchStatusEventAsync("emulator-detected;", "false");
                } catch (Exception e) {
                    AneAwesomeUtilsLogging.e(TAG, "Error checking if running on emulator (async)", e);
                    ctx.dispatchStatusEventAsync("emulator-detected;", "false");
                }
            });
            return null;
        }
    }

    public static class MapXmlToObject implements FREFunction {
        public static final String KEY = "awesomeUtils_mapXmlToObject";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                String xml = args[0].getAsString();
                if (xml == null || xml.isEmpty()) return null;

                XMLInputFactory factory = new InputFactoryImpl();
                factory.setProperty(XMLInputFactory.IS_COALESCING, true);
                XMLStreamReader reader = factory.createXMLStreamReader(new StringReader(xml));

                while (reader.hasNext()) {
                    if (reader.next() == XMLStreamConstants.START_ELEMENT) {
                        return parseElement(reader);
                    }
                }
                return null;
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error mapping XML to object", e);
                return null;
            }
        }

        private static FREObject parseElement(XMLStreamReader reader) throws XMLStreamException, FREWrongThreadException, FREInvalidObjectException, FRETypeMismatchException, FRENoSuchNameException, FREASErrorException, FREReadOnlyException {
            int attrCount = reader.getAttributeCount();

            Map<String, String> attrs = new HashMap<>();
            for (int i = 0; i < attrCount; i++) {
                attrs.put(reader.getAttributeLocalName(i), reader.getAttributeValue(i));
            }

            boolean hasChildren = false;
            StringBuilder text = new StringBuilder();
            Map<String, List<FREObject>> groups = new HashMap<>();

            while (reader.hasNext()) {
                int event = reader.next();
                if (event == XMLStreamConstants.END_ELEMENT) {
                    break;
                } else if (event == XMLStreamConstants.START_ELEMENT) {
                    hasChildren = true;
                    String childName = reader.getLocalName();
                    FREObject childObj = parseElement(reader);
                    List<FREObject> list = groups.get(childName);
                    if (list == null) {
                        list = new ArrayList<>();
                        groups.put(childName, list);
                    }
                    list.add(childObj);
                } else if (event == XMLStreamConstants.CHARACTERS || event == XMLStreamConstants.CDATA) {
                    text.append(reader.getText());
                }
            }

            String value = text.toString().trim();

            if (!hasChildren && attrCount == 0) {
                return valueToFre(value);
            }

            FREObject obj = FREObject.newObject("Object", null);

            for (Map.Entry<String, String> entry : attrs.entrySet()) {
                obj.setProperty(entry.getKey(), valueToFre(entry.getValue()));
            }

            for (Map.Entry<String, List<FREObject>> entry : groups.entrySet()) {
                String key = entry.getKey();
                List<FREObject> list = entry.getValue();
                FREObject propVal;
                int size = list.size();
                if (size == 1) {
                    propVal = list.get(0);
                } else {
                    FREArray arr = FREArray.newArray(size);
                    for (int j = 0; j < size; j++) {
                        arr.setObjectAt(j, list.get(j));
                    }
                    propVal = arr;
                }
                obj.setProperty(key, propVal);
            }

            return obj;
        }

        private static FREObject valueToFre(String value) throws FREWrongThreadException {
            value = value.trim();
            if (value.equalsIgnoreCase("true")) return FREObject.newObject(true);
            if (value.equalsIgnoreCase("false")) return FREObject.newObject(false);

            // Check for leading zero numeric strings to treat as string
            boolean isLeadingZeroNumeric = value.length() > 1 && value.charAt(0) == '0' && value.matches("\\d+");

            if (!isLeadingZeroNumeric) {
                try {
                    int i = Integer.parseInt(value);
                    return FREObject.newObject(i);
                } catch (NumberFormatException ignored) {
                }
                try {
                    long l = Long.parseLong(value);
                    if (l >= 0 && l <= 4294967295L) {
                        return FREObject.newObject((double) l);
                    }
                } catch (NumberFormatException ignored) {
                }
                try {
                    double d = Double.parseDouble(value);
                    return FREObject.newObject(d);
                } catch (NumberFormatException ignored) {
                }
            }

            return FREObject.newObject(value);
        }
    }

    public static class AddClientCertificate implements FREFunction {
        public static final String KEY = "awesomeUtils_addClientCertificate";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_addClientCertificate");
            try {
                AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;
                String domain = args[0].getAsString();
                String pemContent = args[1].getAsString();
                boolean success = ctx.registerClientCertificate(domain, pemContent);
                return FREObject.newObject(success);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error adding client certificate", e);
                return null;
            }
        }
    }
}
