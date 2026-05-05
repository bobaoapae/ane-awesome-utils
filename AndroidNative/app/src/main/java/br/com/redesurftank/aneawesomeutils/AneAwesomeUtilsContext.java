package br.com.redesurftank.aneawesomeutils;

import static br.com.redesurftank.aneawesomeutils.AneAwesomeUtilsExtension.TAG;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.PowerManager;
import android.provider.Settings;
import android.util.Base64;
import android.util.JsonReader;
import android.util.JsonWriter;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.annotation.RequiresApi;

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
import java.net.InetSocketAddress;
import java.net.ProtocolException;
import java.net.Socket;
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
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import javax.net.SocketFactory;
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

    private volatile OkHttpClient _client;
    private final Map<UUID, byte[]> _urlLoaderResults = new ConcurrentHashMap<>();
    private final Map<UUID, RealWebSocket> _webSockets = new ConcurrentHashMap<>();
    private final Queue<byte[]> _byteBufferQueue = new ConcurrentLinkedQueue<>();

    // Add message queue and processor for WebSocket messages
    private final Map<UUID, LinkedBlockingQueue<WebSocketMessage>> _webSocketMessageQueues = new ConcurrentHashMap<>();
    private final Map<UUID, Thread> _webSocketSenderThreads = new ConcurrentHashMap<>();
    private final Object _webSocketLock = new Object();
    private final Executor _backGroundExecutor = Executors.newFixedThreadPool(4);

    private final Object _clientCertificateLock = new Object();
    private final Map<String, ClientCertificateEntry> _clientCertificates = new HashMap<>();
    private X509TrustManager _defaultTrustManager;
    private SSLSocketFactory _defaultSslSocketFactory;

    byte[] _logReadResult;

    private PowerManager.WakeLock _wakeLock;
    private WifiManager.WifiLock _wifiLock;
    private ConnectivityManager _connectivityManager;
    private ConnectivityManager.NetworkCallback _networkCallback;
    private int _pingInterval = 30;
    private int _connectTimeout = 10;
    private int _readTimeout = 30;
    private int _writeTimeout = 30;

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
        functionMap.put(LoadUrlWithBody.KEY, new LoadUrlWithBody());
        functionMap.put(GetLoaderResult.KEY, new GetLoaderResult());
        functionMap.put(GetWebSocketByteArrayMessage.KEY, new GetWebSocketByteArrayMessage());
        functionMap.put(GetDeviceUniqueId.KEY, new GetDeviceUniqueId());
        functionMap.put(DecompressByteArray.KEY, new DecompressByteArray());
        functionMap.put(ReadFileToByteArray.KEY, new ReadFileToByteArray());
        functionMap.put(CheckRunningEmulator.KEY, new CheckRunningEmulator());
        functionMap.put(CheckRunningEmulatorAsync.KEY, new CheckRunningEmulatorAsync());
        functionMap.put(MapXmlToObject.KEY, new MapXmlToObject());
        functionMap.put(AddClientCertificate.KEY, new AddClientCertificate());
        functionMap.put(IsBatteryOptimizationIgnored.KEY, new IsBatteryOptimizationIgnored());
        functionMap.put(RequestBatteryOptimizationExclusion.KEY, new RequestBatteryOptimizationExclusion());
        functionMap.put(ConfigureConnection.KEY, new ConfigureConnection());
        functionMap.put(ReleaseConnectionResources.KEY, new ReleaseConnectionResources());
        functionMap.put(InitLog.KEY, new InitLog());
        functionMap.put(WriteLog.KEY, new WriteLog());
        functionMap.put(GetLogFiles.KEY, new GetLogFiles());
        functionMap.put(ReadLogFile.KEY, new ReadLogFile());
        functionMap.put(GetLogResult.KEY, new GetLogResult());
        functionMap.put(DeleteLogFile.KEY, new DeleteLogFile());
        functionMap.put(PackageCrashBundle.KEY, new PackageCrashBundle());
        functionMap.put(DeleteCrashBundle.KEY, new DeleteCrashBundle());
        functionMap.put(NotifyBackground.KEY, new NotifyBackground());
        functionMap.put(NotifyForeground.KEY, new NotifyForeground());
        functionMap.put(ProbeTick.KEY, new ProbeTick());
        functionMap.put(TriggerMemoryPurge.KEY, new TriggerMemoryPurge());
        functionMap.put(SetAllocatorDecayTime.KEY, new SetAllocatorDecayTime());
        functionMap.put(ProbeMapsByPath.KEY, new ProbeMapsByPath());
        functionMap.put(AllocTracerStart.KEY, new AllocTracerStart());
        functionMap.put(AllocTracerStop.KEY, new AllocTracerStop());
        functionMap.put(AllocTracerDump.KEY, new AllocTracerDump());
        functionMap.put(AllocTracerMark.KEY, new AllocTracerMark());
        functionMap.put(AllocTracerPurgeStalePhase.KEY, new AllocTracerPurgeStalePhase());
        functionMap.put(DeferDrainInstall.KEY, new DeferDrainInstall());
        functionMap.put(DeferDrainUninstall.KEY, new DeferDrainUninstall());
        functionMap.put(DeferDrainStatus.KEY, new DeferDrainStatus());
        functionMap.put(ProfilerStart.KEY, new ProfilerStart());
        functionMap.put(ProfilerStop.KEY, new ProfilerStop());
        functionMap.put(ProfilerGetStatus.KEY, new ProfilerGetStatus());
        functionMap.put(ProfilerStartDeep.KEY, new ProfilerStartDeep());
        functionMap.put(ProfilerStopDeep.KEY, new ProfilerStopDeep());
        functionMap.put(ProfilerSnapshot.KEY, new ProfilerSnapshot());
        functionMap.put(ProfilerMarker.KEY, new ProfilerMarker());
        functionMap.put(ProfilerGetStatusDeep.KEY, new ProfilerGetStatusDeep());
        functionMap.put(ProfilerProbeEnter.KEY, new ProfilerProbeEnter());
        functionMap.put(ProfilerProbeExit.KEY, new ProfilerProbeExit());
        functionMap.put(ProfilerRegisterMethodTable.KEY, new ProfilerRegisterMethodTable());
        functionMap.put(ProfilerRecordFrame.KEY, new ProfilerRecordFrame());
        functionMap.put(ProfilerRequestGc.KEY, new ProfilerRequestGc());
        functionMap.put(ProfilerDumpAvmCore.KEY, new ProfilerDumpAvmCore());
        functionMap.put(ProfilerSamplerHookInstall.KEY, new ProfilerSamplerHookInstall());
        functionMap.put(ProfilerSamplerHookUninstall.KEY, new ProfilerSamplerHookUninstall());
        functionMap.put(ProfilerAs3SamplerInstall.KEY, new ProfilerAs3SamplerInstall());
        functionMap.put(ProfilerAs3SamplerUninstall.KEY, new ProfilerAs3SamplerUninstall());

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

        try { if (_wakeLock != null && _wakeLock.isHeld()) _wakeLock.release(); } catch (Exception e) {}
        try { if (_wifiLock != null && _wifiLock.isHeld()) _wifiLock.release(); } catch (Exception e) {}
        try {
            if (_connectivityManager != null && _networkCallback != null)
                _connectivityManager.unregisterNetworkCallback(_networkCallback);
        } catch (Exception e) {}
        _wakeLock = null;
        _wifiLock = null;
        _connectivityManager = null;
        _networkCallback = null;

        NativeLogManager.close();
    }

    public void safeDispatchEvent(String code, String level) {
        try {
            dispatchStatusEventAsync(code, level != null ? level : "");
        } catch (Exception e) {
            // ignore
        }
    }

    private void startWebSocketSenderThread(UUID uuid) {
        synchronized (_webSocketLock) {
            if (_webSocketSenderThreads.containsKey(uuid)) {
                return; // Thread already exists
            }

            // Create message queue if it doesn't exist
            if (!_webSocketMessageQueues.containsKey(uuid)) {
                _webSocketMessageQueues.put(uuid, new LinkedBlockingQueue<>());
            }

            Thread senderThread = new Thread(() -> {
                android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_FOREGROUND);
                AneAwesomeUtilsLogging.d(TAG, "Starting WebSocket sender thread for " + uuid);
                LinkedBlockingQueue<WebSocketMessage> queue = _webSocketMessageQueues.get(uuid);

                while (!Thread.currentThread().isInterrupted()) {
                    try {
                        WebSocketMessage message = queue.take();
                        WebSocket webSocket = _webSockets.get(uuid);
                        if (webSocket == null) {
                            AneAwesomeUtilsLogging.d(TAG, "WebSocket gone for " + uuid + ", sender thread exiting");
                            break;
                        }
                        message.send(webSocket);
                    } catch (InterruptedException e) {
                        // Thread interrupted - exit gracefully
                        break;
                    } catch (Exception e) {
                        AneAwesomeUtilsLogging.w(TAG, "Error in WebSocket sender thread: " + e.getMessage());
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
        OkHttpClient client = _client;
        if (host == null || host.isEmpty()) {
            AneAwesomeUtilsLogging.d(TAG, "getClientForHost: host is null or empty, using default client");
            return client;
        }
        ClientCertificateEntry entry = null;
        synchronized (_clientCertificateLock) {
            entry = _clientCertificates.get(host.toLowerCase());
            AneAwesomeUtilsLogging.d(TAG, "getClientForHost: looking for host '" + host.toLowerCase() + "', found: " + (entry != null));
        }
        if (entry == null) {
            AneAwesomeUtilsLogging.d(TAG, "getClientForHost: no certificate for host, using default client");
            return client;
        }
        try {
            X509TrustManager trustManager = entry.trustManager != null ? entry.trustManager : _defaultTrustManager;
            AneAwesomeUtilsLogging.d(TAG, "getClientForHost: building client with mTLS for host: " + host);
            return client.newBuilder().sslSocketFactory(entry.sslSocketFactory, trustManager).build();
        } catch (Exception e) {
            AneAwesomeUtilsLogging.e(TAG, "Error building client for host with client certificate", e);
            return client;
        }
    }

    private static class ClientCertificateEntry {
        final X509KeyManager keyManager;
        final SSLSocketFactory sslSocketFactory;
        final X509TrustManager trustManager;

        ClientCertificateEntry(X509KeyManager keyManager, SSLSocketFactory sslSocketFactory, X509TrustManager trustManager) {
            this.keyManager = keyManager;
            this.sslSocketFactory = sslSocketFactory;
            this.trustManager = trustManager;
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

    private static final X509TrustManager TRUST_ALL_MANAGER = new X509TrustManager() {
        @Override
        public void checkClientTrusted(X509Certificate[] chain, String authType) {
        }

        @Override
        public void checkServerTrusted(X509Certificate[] chain, String authType) {
        }

        @Override
        public X509Certificate[] getAcceptedIssuers() {
            return new X509Certificate[0];
        }
    };

    private ClientCertificateEntry buildClientCertificate(String domain, String pem) throws Exception {
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

        // Use trust-all manager for mTLS - we only care about sending client cert
        SSLContext sslContext = SSLContext.getInstance("TLS");
        sslContext.init(new KeyManager[]{keyManager}, new TrustManager[]{TRUST_ALL_MANAGER}, null);
        return new ClientCertificateEntry(keyManager, sslContext.getSocketFactory(), TRUST_ALL_MANAGER);
    }

    private boolean registerClientCertificate(String domain, String pem) {
        if (domain == null || domain.isEmpty() || pem == null || pem.isEmpty()) {
            AneAwesomeUtilsLogging.e(TAG, "Domain or PEM content is empty");
            return false;
        }
        try {
            AneAwesomeUtilsLogging.d(TAG, "Registering client certificate for domain: " + domain.toLowerCase());
            ClientCertificateEntry entry = buildClientCertificate(domain, pem);
            synchronized (_clientCertificateLock) {
                _clientCertificates.put(domain.toLowerCase(), entry);
            }
            AneAwesomeUtilsLogging.d(TAG, "Successfully registered client certificate for domain: " + domain.toLowerCase());
            return true;
        } catch (Exception e) {
            AneAwesomeUtilsLogging.e(TAG, "Error registering client certificate for domain: " + domain, e);
            return false;
        }
    }

    private void acquireConnectionResources() {
        Activity activity = getActivity();
        if (activity == null) return;

        // WiFi Lock (no extra permission needed)
        try {
            WifiManager wm = (WifiManager) activity.getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            if (wm != null) {
                _wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "ane:websocket");
                _wifiLock.acquire();
                AneAwesomeUtilsLogging.i(TAG, "WiFi lock acquired");
            }
        } catch (Exception e) {
            AneAwesomeUtilsLogging.e(TAG, "Error acquiring WiFi lock", e);
        }

        // WakeLock (needs WAKE_LOCK permission - granted at install time, no runtime check needed)
        try {
            PowerManager pm = (PowerManager) activity.getSystemService(Context.POWER_SERVICE);
            if (pm != null) {
                _wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "ane:websocket");
                _wakeLock.acquire();
                AneAwesomeUtilsLogging.i(TAG, "WakeLock acquired");
            }
        } catch (Exception e) {
            AneAwesomeUtilsLogging.e(TAG, "Error acquiring WakeLock", e);
        }

        // NetworkCallback (needs ACCESS_NETWORK_STATE - granted at install time, API 21+)
        try {
            _connectivityManager = (ConnectivityManager) activity.getSystemService(Context.CONNECTIVITY_SERVICE);
            if (_connectivityManager != null) {
                NetworkRequest request = new NetworkRequest.Builder()
                        .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                        .build();
                _networkCallback = new ConnectivityManager.NetworkCallback() {
                    @Override
                    public void onAvailable(@NonNull Network network) {
                        try {
                            dispatchStatusEventAsync("network;available", "status");
                        } catch (Exception e) {
                            AneAwesomeUtilsLogging.e(TAG, "Error dispatching network available event", e);
                        }
                    }

                    @Override
                    public void onLost(@NonNull Network network) {
                        try {
                            dispatchStatusEventAsync("network;lost", "status");
                        } catch (Exception e) {
                            AneAwesomeUtilsLogging.e(TAG, "Error dispatching network lost event", e);
                        }
                    }
                };
                _connectivityManager.registerNetworkCallback(request, _networkCallback);
                AneAwesomeUtilsLogging.i(TAG, "NetworkCallback registered");
            }
        } catch (Exception e) {
            AneAwesomeUtilsLogging.e(TAG, "Error registering NetworkCallback", e);
        }
    }

    private void releaseConnectionResources() {
        try { if (_wakeLock != null && _wakeLock.isHeld()) _wakeLock.release(); } catch (Exception e) {}
        try { if (_wifiLock != null && _wifiLock.isHeld()) _wifiLock.release(); } catch (Exception e) {}
        try {
            if (_connectivityManager != null && _networkCallback != null)
                _connectivityManager.unregisterNetworkCallback(_networkCallback);
        } catch (Exception e) {}
        _wakeLock = null;
        _wifiLock = null;
        _connectivityManager = null;
        _networkCallback = null;
        AneAwesomeUtilsLogging.i(TAG, "Connection resources released");
    }

    OkHttpClient buildClient() {
        OkHttpClient.Builder builder = new OkHttpClient.Builder()
                .fastFallback(true)
                .retryOnConnectionFailure(true)
                .dns(new Dns() {
                    @NonNull
                    @Override
                    public List<InetAddress> lookup(@NonNull String s) throws UnknownHostException {
                        return InternalDnsResolver.getInstance().resolveHost(s);
                    }
                })
                .socketFactory(new SocketFactory() {
                    @Override
                    public Socket createSocket() throws IOException {
                        Socket socket = new Socket();
                        socket.setKeepAlive(true);
                        socket.setTcpNoDelay(true);
                        return socket;
                    }

                    @Override
                    public Socket createSocket(String host, int port) throws IOException {
                        Socket socket = new Socket();
                        socket.setKeepAlive(true);
                        socket.setTcpNoDelay(true);
                        socket.connect(new InetSocketAddress(host, port));
                        return socket;
                    }

                    @Override
                    public Socket createSocket(String host, int port, InetAddress localHost, int localPort) throws IOException {
                        Socket socket = new Socket();
                        socket.setKeepAlive(true);
                        socket.setTcpNoDelay(true);
                        socket.bind(new InetSocketAddress(localHost, localPort));
                        socket.connect(new InetSocketAddress(host, port));
                        return socket;
                    }

                    @Override
                    public Socket createSocket(InetAddress host, int port) throws IOException {
                        Socket socket = new Socket();
                        socket.setKeepAlive(true);
                        socket.setTcpNoDelay(true);
                        socket.connect(new InetSocketAddress(host, port));
                        return socket;
                    }

                    @Override
                    public Socket createSocket(InetAddress address, int port, InetAddress localAddress, int localPort) throws IOException {
                        Socket socket = new Socket();
                        socket.setKeepAlive(true);
                        socket.setTcpNoDelay(true);
                        socket.bind(new InetSocketAddress(localAddress, localPort));
                        socket.connect(new InetSocketAddress(address, port));
                        return socket;
                    }
                })
                .connectionPool(new okhttp3.ConnectionPool(5, 1, TimeUnit.MINUTES))
                .pingInterval(_pingInterval, TimeUnit.SECONDS)
                .connectTimeout(_connectTimeout, TimeUnit.SECONDS)
                .readTimeout(_readTimeout, TimeUnit.SECONDS)
                .writeTimeout(_writeTimeout, TimeUnit.SECONDS)
                .addInterceptor(chain -> {
                    Request originalRequest = chain.request();
                    Request requestWithUserAgent = originalRequest.newBuilder().header("User-Agent", originalRequest.header("User-Agent") + " NativeLoader/1.0").build();
                    return chain.proceed(requestWithUserAgent);
                });

        configureToIgnoreCertificate(builder);
        return builder.build();
    }

    private static OkHttpClient.Builder configureToIgnoreCertificate(OkHttpClient.Builder builder) {
        AneAwesomeUtilsLogging.w(TAG, "Ignoring SSL certificate");
        try {
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

            final SSLContext sslContext = SSLContext.getInstance("SSL");
            sslContext.init(null, trustAllCerts, new java.security.SecureRandom());
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

    public static class Initialize implements FREFunction {
        public static final String KEY = "awesomeUtils_initialize";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_initialize");
            try {
                AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;
                ctx._client = ctx.buildClient();
                ctx.acquireConnectionResources();
                // AirIMEGuard and LayoutExceptionGuard are installed earlier in
                // AneAwesomeUtilsExtension.initialize() — don't re-install here.
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
                LinkedBlockingQueue<WebSocketMessage> queue = ctx._webSocketMessageQueues.get(uuid);
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
                        // Stop sender thread before removing WebSocket to avoid race condition
                        ctx.stopWebSocketSenderThread(id);

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
                        ctx._webSockets.remove(id);
                        ctx.dispatchWebSocketEvent(id.toString(), "disconnected", reason + ";" + closeCode + ";" + responseCode + ";" + getHeadersAsBase64(headers));
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
        private static final ExecutorService FILE_IO = Executors.newFixedThreadPool(4);
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
                } else if (method.equals("POST_JSON")) {
                    String json = "{}";
                    if (!variables.isEmpty()) {
                        org.json.JSONObject obj = new org.json.JSONObject(variables);
                        json = obj.toString();
                    }
                    requestBuilder.url(url).post(okhttp3.RequestBody.create(json, okhttp3.MediaType.parse("application/json")));
                } else if (method.equals("PUT")) {
                    String body = variables.containsKey("__body__") ? variables.get("__body__") : "";
                    String contentType = variables.containsKey("__contentType__") ? variables.get("__contentType__") : "application/octet-stream";
                    requestBuilder.url(url).put(okhttp3.RequestBody.create(body.getBytes(java.nio.charset.StandardCharsets.UTF_8), okhttp3.MediaType.parse(contentType)));
                }

                Request request = requestBuilder.build();
                OkHttpClient client = ctx.getClientForHost(parsedUrl.host());
                client.newCall(request).enqueue(new Callback() {
                    @Override
                    public void onFailure(@NonNull Call call, @NonNull IOException e) {
                        AneAwesomeUtilsLogging.e(TAG, "Error loading url:" + url, e);
                        String msg = e.getClass().getSimpleName() + ": " + (e.getMessage() != null ? e.getMessage() : "unknown");
                        if (e.getCause() != null) {
                            msg += " [caused by " + e.getCause().getClass().getSimpleName() + ": " + e.getCause().getMessage() + "]";
                        }
                        ctx.dispatchUrlLoaderEvent(uuid.toString(), "error", msg);
                    }

                    @Override
                    public void onResponse(@NonNull Call call, @NonNull Response response) throws IOException {
                        try {
                            int statusCode = response.code();
                            AneAwesomeUtilsLogging.d(TAG, "URL: " + url + " Status code: " + statusCode);
                            if (statusCode >= 400) {
                                String errorBody = "";
                                try { errorBody = response.body() != null ? response.body().string() : ""; } catch (Exception ignored) {}
                                String errorMsg = "HTTP " + statusCode;
                                if (errorBody != null && !errorBody.isEmpty()) {
                                    if (errorBody.length() > 512) errorBody = errorBody.substring(0, 512);
                                    errorMsg += ": " + errorBody;
                                }
                                ctx.dispatchUrlLoaderEvent(uuid.toString(), "error", errorMsg);
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
                            ctx.dispatchUrlLoaderEvent(uuid.toString(), "error", e.getClass().getSimpleName() + ": " + e.getMessage());
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

    public static class LoadUrlWithBody implements FREFunction {
        public static final String KEY = "awesomeUtils_loadUrlWithBody";
        private static final byte[] EmptyResult = new byte[]{0, 1, 2, 3};

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_loadUrlWithBody");
            try {
                UUID uuid = UUID.randomUUID();
                AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;

                // args: url, method, headersJson, body (ByteArray), contentType
                String url = args[0].getAsString();
                String method = args[1].getAsString();
                String headersJson = args[2].getAsString();

                FREByteArray freBody = (FREByteArray) args[3];
                freBody.acquire();
                byte[] body = new byte[(int) freBody.getLength()];
                freBody.getBytes().get(body);
                freBody.release();

                String contentType = args[4].getAsString();

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

                requestBuilder.url(url).method(method,
                    okhttp3.RequestBody.create(body, okhttp3.MediaType.parse(contentType)));

                Request request = requestBuilder.build();
                OkHttpClient client = ctx.getClientForHost(parsedUrl.host());
                client.newCall(request).enqueue(new Callback() {
                    @Override
                    public void onFailure(@NonNull Call call, @NonNull IOException e) {
                        AneAwesomeUtilsLogging.e(TAG, "Error loadUrlWithBody:" + url, e);
                        String msg = e.getClass().getSimpleName() + ": " + (e.getMessage() != null ? e.getMessage() : "unknown");
                        if (e.getCause() != null) {
                            msg += " [caused by " + e.getCause().getClass().getSimpleName() + ": " + e.getCause().getMessage() + "]";
                        }
                        ctx.dispatchUrlLoaderEvent(uuid.toString(), "error", msg);
                    }

                    @Override
                    public void onResponse(@NonNull Call call, @NonNull Response response) throws IOException {
                        try {
                            int statusCode = response.code();
                            if (statusCode >= 400) {
                                String errorBody = "";
                                try { errorBody = response.body() != null ? response.body().string() : ""; } catch (Exception ignored) {}
                                String errorMsg = "HTTP " + statusCode;
                                if (errorBody != null && !errorBody.isEmpty()) {
                                    if (errorBody.length() > 512) errorBody = errorBody.substring(0, 512);
                                    errorMsg += ": " + errorBody;
                                }
                                ctx.dispatchUrlLoaderEvent(uuid.toString(), "error", errorMsg);
                                return;
                            }
                            byte[] bytes = response.body().bytes();
                            if (bytes.length == 0) bytes = EmptyResult;
                            ctx._urlLoaderResults.put(uuid, bytes);
                            ctx.dispatchUrlLoaderEvent(uuid.toString(), "success", "");
                        } catch (Exception e) {
                            AneAwesomeUtilsLogging.e(TAG, "Error loadUrlWithBody:" + url, e);
                            ctx.dispatchUrlLoaderEvent(uuid.toString(), "error", e.getClass().getSimpleName() + ": " + e.getMessage());
                        }
                    }
                });

                return FREObject.newObject(uuid.toString());

            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error loadUrlWithBody", e);
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

    public static class IsBatteryOptimizationIgnored implements FREFunction {
        public static final String KEY = "awesomeUtils_isBatteryOptimizationIgnored";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_isBatteryOptimizationIgnored");
            try {
                // Doze mode only exists on API 23+, no need to show dialog on older versions
                if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
                    return FREObject.newObject(true);
                }
                Activity activity = context.getActivity();
                if (activity == null) return FREObject.newObject(true);
                PowerManager pm = (PowerManager) activity.getSystemService(Context.POWER_SERVICE);
                if (pm == null) return FREObject.newObject(true);
                boolean ignored = pm.isIgnoringBatteryOptimizations(activity.getPackageName());
                return FREObject.newObject(ignored);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error checking battery optimization", e);
                return null;
            }
        }
    }

    public static class RequestBatteryOptimizationExclusion implements FREFunction {
        public static final String KEY = "awesomeUtils_requestBatteryOptimizationExclusion";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.i(TAG, "awesomeUtils_requestBatteryOptimizationExclusion called");
            try {
                if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
                    AneAwesomeUtilsLogging.i(TAG, "API < 23, Doze mode not available, skipping");
                    return null;
                }
                Activity activity = context.getActivity();
                if (activity == null) {
                    AneAwesomeUtilsLogging.e(TAG, "Activity is null, cannot request battery optimization exclusion");
                    return null;
                }
                String packageName = activity.getPackageName();
                AneAwesomeUtilsLogging.i(TAG, "Requesting battery optimization exclusion for: " + packageName);

                // Check if already ignored
                PowerManager pm = (PowerManager) activity.getSystemService(Context.POWER_SERVICE);
                if (pm != null && pm.isIgnoringBatteryOptimizations(packageName)) {
                    AneAwesomeUtilsLogging.i(TAG, "Battery optimization already ignored, skipping");
                    return null;
                }

                // Launch a transparent wrapper Activity that hosts the system dialog.
                // This avoids AIR's immersive mode (ProxyWindowCallback.onWindowFocusChanged)
                // from reclaiming focus and dismissing the system dialog.
                // See: Google Issue Tracker 36992828.
                Intent wrapperIntent = new Intent(activity, BatteryOptActivity.class);
                activity.startActivity(wrapperIntent);
                AneAwesomeUtilsLogging.i(TAG, "BatteryOptActivity launched");
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error requesting battery optimization exclusion", e);
            }
            return null;
        }
    }

    public static class ConfigureConnection implements FREFunction {
        public static final String KEY = "awesomeUtils_configureConnection";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_configureConnection");
            try {
                AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;
                ctx._pingInterval = args[0].getAsInt();
                ctx._connectTimeout = args[1].getAsInt();
                ctx._readTimeout = args[2].getAsInt();
                ctx._writeTimeout = args[3].getAsInt();
                ctx._client = ctx.buildClient();
                AneAwesomeUtilsLogging.i(TAG, "Connection configured: ping=" + ctx._pingInterval + "s, connect=" + ctx._connectTimeout + "s, read=" + ctx._readTimeout + "s, write=" + ctx._writeTimeout + "s");
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error configuring connection", e);
            }
            return null;
        }
    }

    public static class ReleaseConnectionResources implements FREFunction {
        public static final String KEY = "awesomeUtils_releaseConnectionResources";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsLogging.d(TAG, "awesomeUtils_releaseConnectionResources");
            try {
                AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;
                ctx.releaseConnectionResources();
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Error releasing connection resources", e);
            }
            return null;
        }
    }

    public static class InitLog implements FREFunction {
        public static final String KEY = "awesomeUtils_initLog";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                String profile = args[0].getAsString();
                String storagePath = args[1].getAsString();
                String path = NativeLogManager.init(storagePath, profile);
                if (NativeLogManager.hadUnexpectedShutdown()) {
                    AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;
                    ctx.safeDispatchEvent("log;unexpectedShutdown", NativeLogManager.getUnexpectedShutdownInfo());
                }
                return FREObject.newObject(path);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e("NativeLog", "Error in initLog", e);
                return null;
            }
        }
    }

    public static class WriteLog implements FREFunction {
        public static final String KEY = "awesomeUtils_writeLog";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                String level = args[0].getAsString();
                String tag = args[1].getAsString();
                String message = args[2].getAsString();
                // Mirror to Android logcat so test/CI scripts can grep `adb logcat`
                // without having to pull + decrypt the XOR-encoded rolling log
                // file. The level string is one of "DEBUG"/"INFO"/"WARN"/"ERROR".
                try {
                    if ("ERROR".equalsIgnoreCase(level))      android.util.Log.e(tag, message);
                    else if ("WARN".equalsIgnoreCase(level))  android.util.Log.w(tag, message);
                    else if ("DEBUG".equalsIgnoreCase(level)) android.util.Log.d(tag, message);
                    else                                       android.util.Log.i(tag, message);
                } catch (Throwable ignored) {}
                NativeLogManager.write(level, tag, message);
            } catch (Exception e) {
                // ignore
            }
            return null;
        }
    }

    public static class GetLogFiles implements FREFunction {
        public static final String KEY = "awesomeUtils_getLogFiles";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                String json = NativeLogManager.getLogFiles();
                return FREObject.newObject(json);
            } catch (Exception e) {
                return null;
            }
        }
    }

    public static class ReadLogFile implements FREFunction {
        public static final String KEY = "awesomeUtils_readLogFile";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                String date = args[0].getAsString();
                AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;
                // Reuse the shared 4-thread background executor instead of spawning
                // a raw Thread per call — prevents unbounded thread accumulation
                // under AS3 polling patterns.
                ctx._backGroundExecutor.execute(() -> {
                    try {
                        byte[] data = NativeLogManager.readLogFile(date);
                        synchronized (ctx) {
                            ctx._logReadResult = data;
                        }
                        ctx.safeDispatchEvent("log;readComplete", "");
                    } catch (Exception e) {
                        ctx.safeDispatchEvent("log;readError", e.getMessage());
                    }
                });
            } catch (Exception e) {
                ((AneAwesomeUtilsContext) context).safeDispatchEvent("log;readError", e.getMessage());
            }
            return null;
        }
    }

    /**
     * Packages a crash session's log + metadata into a ZIP on disk and dispatches
     * "log;bundleReady" with the absolute path (or "log;bundleError" on failure).
     * AS3 then reads the ZIP bytes via readFileToByteArray and uploads as
     * application/zip. Replaces the old decrypt-then-upload-plaintext flow —
     * log stays encrypted-at-rest until packaged, then travels as ZIP bytes.
     *
     * args[0] = date ("YYYY-MM-DD" or "")
     * args[1] = appVersion (string)
     * args[2] = sessionId (string, optional)
     */
    public static class PackageCrashBundle implements FREFunction {
        public static final String KEY = "awesomeUtils_packageCrashBundle";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            final AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;
            try {
                final String date = args.length > 0 ? args[0].getAsString() : "";
                final String appVersion = args.length > 1 ? args[1].getAsString() : "";
                final String sessionId = args.length > 2 ? args[2].getAsString() : "";
                final String storagePath = ctx.getActivity().getFilesDir().getAbsolutePath();
                ctx._backGroundExecutor.execute(() -> {
                    try {
                        String zipPath = CrashBundleBuilder.build(storagePath, date, appVersion, sessionId);
                        if (zipPath == null) {
                            ctx.safeDispatchEvent("log;bundleError", "build returned null");
                            return;
                        }
                        ctx.safeDispatchEvent("log;bundleReady", zipPath);
                    } catch (Exception e) {
                        ctx.safeDispatchEvent("log;bundleError", String.valueOf(e.getMessage()));
                    }
                });
            } catch (Exception e) {
                ctx.safeDispatchEvent("log;bundleError", String.valueOf(e.getMessage()));
            }
            return null;
        }
    }

    /**
     * Deletes a ZIP bundle by absolute path — called by AS3 after a successful
     * upload to free disk. Synchronous; returns true on success.
     */
    public static class DeleteCrashBundle implements FREFunction {
        public static final String KEY = "awesomeUtils_deleteCrashBundle";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                String path = args[0].getAsString();
                return FREObject.newObject(CrashBundleBuilder.delete(path));
            } catch (Exception e) {
                try { return FREObject.newObject(false); } catch (Exception ignored) { return null; }
            }
        }
    }

    public static class GetLogResult implements FREFunction {
        public static final String KEY = "awesomeUtils_getLogResult";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;
                byte[] data;
                synchronized (ctx) {
                    data = ctx._logReadResult;
                    ctx._logReadResult = null;
                }
                if (data == null) return null;
                FREByteArray byteArray = FREByteArray.newByteArray();
                byteArray.setProperty("length", FREObject.newObject(data.length));
                byteArray.acquire();
                ByteBuffer buffer = byteArray.getBytes();
                buffer.put(data);
                byteArray.release();
                return byteArray;
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e("NativeLog", "Error getting log result", e);
                return null;
            }
        }
    }

    public static class DeleteLogFile implements FREFunction {
        public static final String KEY = "awesomeUtils_deleteLogFile";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                String date = args[0].getAsString();
                boolean result = NativeLogManager.deleteLogFile(date);
                return FREObject.newObject(result);
            } catch (Exception e) {
                try {
                    return FREObject.newObject(false);
                } catch (Exception ex) {
                    return null;
                }
            }
        }
    }

    public static class NotifyBackground implements FREFunction {
        public static final String KEY = "awesomeUtils_notifyBackground";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            NativeLogManager.onBackground();
            return null;
        }
    }

    public static class NotifyForeground implements FREFunction {
        public static final String KEY = "awesomeUtils_notifyForeground";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            NativeLogManager.onForeground();
            return null;
        }
    }

    // -------------------------------------------------------------------
    // Memory probe surface — used by AS3 test scenarios to correlate the
    // VMA/scudo:secondary curve with AS3 manager / ANE queue sizes during
    // long-burn diagnostic runs. See plans/quero-que-crie-um-replicated-milner.md
    // -------------------------------------------------------------------

    /**
     * {@code awesomeUtils_probeTick(flags:int):String}
     *
     * <p>Returns one consolidated JSON snapshot covering:
     *   bit 0 ({@code mem})       — threads, jvm/native heap, VmRSS/VmSize
     *   bit 1 ({@code maps})      — {@code /proc/self/maps} aggregate counts
     *   bit 2 ({@code internal})  — sizes of the ANE's own queues/maps
     *
     * <p>Caller passes {@code flags=0x7} for everything, or just the bits it
     * needs (mem is cheap, maps is ~20–80 ms on a 60k-mapping process,
     * internal is cheap). All sub-objects are always emitted (with default
     * values when their bit is off) so the JSON shape is stable for parsers.
     */
    public static class ProbeTick implements FREFunction {
        public static final String KEY = "awesomeUtils_probeTick";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            AneAwesomeUtilsContext ctx = (AneAwesomeUtilsContext) context;
            int flags;
            try {
                flags = (args != null && args.length > 0 && args[0] != null) ? args[0].getAsInt() : 0x7;
            } catch (Exception e) {
                flags = 0x7;
            }
            try {
                StringBuilder sb = new StringBuilder(512);
                sb.append("{\"ts\":").append(System.currentTimeMillis());

                // -- mem ------------------------------------------------------
                sb.append(",\"mem\":");
                if ((flags & 0x1) != 0) {
                    int threads = countThreads();
                    Runtime rt = Runtime.getRuntime();
                    long jvmUsedKb = (rt.totalMemory() - rt.freeMemory()) / 1024L;
                    long jvmMaxKb  = rt.maxMemory() / 1024L;
                    long nativeKb  = android.os.Debug.getNativeHeapAllocatedSize() / 1024L;
                    long[] proc = readProcStatusKb();  // [vmRss, vmSize]
                    sb.append("{\"threads\":").append(threads)
                      .append(",\"jvmUsedKb\":").append(jvmUsedKb)
                      .append(",\"jvmMaxKb\":").append(jvmMaxKb)
                      .append(",\"nativeKb\":").append(nativeKb)
                      .append(",\"vmRssKb\":").append(proc[0])
                      .append(",\"vmSizeKb\":").append(proc[1]).append('}');
                } else {
                    sb.append("null");
                }

                // -- maps -----------------------------------------------------
                sb.append(",\"maps\":");
                if ((flags & 0x2) != 0) {
                    long[] m = ProbeNative.nativeProbeMaps();
                    if (m != null && m.length >= ProbeNative.SLOT_COUNT) {
                        sb.append("{\"total\":").append(m[ProbeNative.SLOT_TOTAL])
                          .append(",\"scudoSecondary\":").append(m[ProbeNative.SLOT_SCUDO_SECONDARY])
                          .append(",\"scudoPrimary\":").append(m[ProbeNative.SLOT_SCUDO_PRIMARY])
                          .append(",\"stacks\":").append(m[ProbeNative.SLOT_STACKS])
                          .append(",\"dalvik\":").append(m[ProbeNative.SLOT_DALVIK])
                          .append(",\"other\":").append(m[ProbeNative.SLOT_OTHER])
                          .append(",\"totalKb\":").append(m[ProbeNative.SLOT_TOTAL_KB])
                          .append(",\"scudoSecondaryKb\":").append(m[ProbeNative.SLOT_SCUDO_SECONDARY_KB])
                          .append('}');
                    } else {
                        sb.append("null");
                    }
                } else {
                    sb.append("null");
                }

                // -- internal -------------------------------------------------
                sb.append(",\"internal\":");
                if ((flags & 0x4) != 0) {
                    int webSockets             = ctx._webSockets.size();
                    int urlLoaderResults       = ctx._urlLoaderResults.size();
                    int byteBufferQueue        = ctx._byteBufferQueue.size();
                    int wsMessageQueues        = ctx._webSocketMessageQueues.size();
                    int wsSenderThreads        = ctx._webSocketSenderThreads.size();
                    sb.append("{\"webSockets\":").append(webSockets)
                      .append(",\"urlLoaderResults\":").append(urlLoaderResults)
                      .append(",\"byteBufferQueue\":").append(byteBufferQueue)
                      .append(",\"wsMessageQueues\":").append(wsMessageQueues)
                      .append(",\"wsSenderThreads\":").append(wsSenderThreads).append('}');
                } else {
                    sb.append("null");
                }

                sb.append('}');
                return FREObject.newObject(sb.toString());
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "probeTick failed", e);
                try { return FREObject.newObject("{\"error\":\"probeTick threw\"}"); } catch (Exception ex) { return null; }
            }
        }
    }

    /**
     * {@code awesomeUtils_triggerMemoryPurge():String}
     *
     * <p>Calls bionic {@code mallopt(M_PURGE_ALL, 0)} to force scudo to
     * {@code munmap} idle :secondary regions. Captures native heap +
     * {@code /proc/self/maps} VMA count before/after; returns delta JSON.
     *
     * <p>Useful for the diagnostic scenario that separates "VMAs the
     * allocator refuses to return" (purge zeros them, leak is fragmentation)
     * from "VMAs holding live allocations" (purge no-ops, leak is a
     * retainer — chase via dumpManagerKeys diff).
     */
    /**
     * {@code awesomeUtils_profilerStart(outputPath:String, headerJson:String, telemetryPort:int):int}
     *
     * <p>Starts the Scout TCP byte tap. Output written to a .flmc file at
     * {@code outputPath}, header JSON populated with {@code headerJson}.
     * Sockets peered to {@code 127.0.0.1:telemetryPort} are captured;
     * 0 = no filter (capture all sends).
     */
    public static class ProfilerStart implements FREFunction {
        public static final String KEY = "awesomeUtils_profilerStart";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                String outputPath = (args != null && args.length > 0 && args[0] != null) ? args[0].getAsString() : "";
                String headerJson = (args != null && args.length > 1 && args[1] != null) ? args[1].getAsString() : "{}";
                int port = 0;
                if (args != null && args.length > 2 && args[2] != null) {
                    port = args[2].getAsInt();
                }
                int rc = Profiler.start(outputPath, headerJson, port);
                AneAwesomeUtilsLogging.i(TAG, "profilerStart rc=" + rc + " path=" + outputPath + " port=" + port);
                return FREObject.newObject(rc);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "profilerStart failed", e);
                try { return FREObject.newObject(-99); } catch (Exception ex) { return null; }
            }
        }
    }

    public static class ProfilerStop implements FREFunction {
        public static final String KEY = "awesomeUtils_profilerStop";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                int rc = Profiler.stop();
                AneAwesomeUtilsLogging.i(TAG, "profilerStop rc=" + rc);
                return FREObject.newObject(rc);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "profilerStop failed", e);
                try { return FREObject.newObject(-1); } catch (Exception ex) { return null; }
            }
        }
    }

    public static class ProfilerGetStatus implements FREFunction {
        public static final String KEY = "awesomeUtils_profilerGetStatus";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                String json = Profiler.getStatus();
                return FREObject.newObject(json);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "profilerGetStatus failed", e);
                try { return FREObject.newObject("{\"error\":\"status threw\"}"); } catch (Exception ex) { return null; }
            }
        }
    }

    /**
     * {@code awesomeUtils_profilerStartDeep(outputPath, headerJson, timing,
     *                                       memory, snapshots, maxLive,
     *                                       intervalMs, as3Sampling):int}
     *
     * <p>Starts the deep .aneprof profiler — full event stream with timing,
     * native memory (via alloc_tracer wiring), snapshots, and markers.
     * AS3 sampling is reserved (Phase 4 TBD).
     */
    public static class ProfilerStartDeep implements FREFunction {
        public static final String KEY = "awesomeUtils_profilerStartDeep";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                String outputPath  = (args != null && args.length > 0 && args[0] != null) ? args[0].getAsString() : "";
                String headerJson  = (args != null && args.length > 1 && args[1] != null) ? args[1].getAsString() : "{}";
                boolean timing     = (args != null && args.length > 2 && args[2] != null) && args[2].getAsBool();
                boolean memory     = (args != null && args.length > 3 && args[3] != null) && args[3].getAsBool();
                boolean snapshots  = (args != null && args.length > 4 && args[4] != null) && args[4].getAsBool();
                int maxLive        = (args != null && args.length > 5 && args[5] != null) ? args[5].getAsInt() : 4096;
                int intervalMs     = (args != null && args.length > 6 && args[6] != null) ? args[6].getAsInt() : 0;
                boolean as3Sampling= (args != null && args.length > 7 && args[7] != null) && args[7].getAsBool();
                int rc = Profiler.startDeep(outputPath, headerJson, timing, memory, snapshots,
                                             maxLive, intervalMs, as3Sampling);
                AneAwesomeUtilsLogging.i(TAG, "profilerStartDeep rc=" + rc + " path=" + outputPath);
                return FREObject.newObject(rc);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "profilerStartDeep failed", e);
                try { return FREObject.newObject(-99); } catch (Exception ex) { return null; }
            }
        }
    }

    public static class ProfilerStopDeep implements FREFunction {
        public static final String KEY = "awesomeUtils_profilerStopDeep";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                int rc = Profiler.stopDeep();
                AneAwesomeUtilsLogging.i(TAG, "profilerStopDeep rc=" + rc);
                return FREObject.newObject(rc);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "profilerStopDeep failed", e);
                try { return FREObject.newObject(-1); } catch (Exception ex) { return null; }
            }
        }
    }

    public static class ProfilerSnapshot implements FREFunction {
        public static final String KEY = "awesomeUtils_profilerSnapshot";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                String label = (args != null && args.length > 0 && args[0] != null) ? args[0].getAsString() : "";
                boolean ok = Profiler.snapshot(label);
                return FREObject.newObject(ok);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "profilerSnapshot failed", e);
                try { return FREObject.newObject(false); } catch (Exception ex) { return null; }
            }
        }
    }

    public static class ProfilerMarker implements FREFunction {
        public static final String KEY = "awesomeUtils_profilerMarker";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                String name      = (args != null && args.length > 0 && args[0] != null) ? args[0].getAsString() : "";
                String valueJson = (args != null && args.length > 1 && args[1] != null) ? args[1].getAsString() : "{}";
                boolean ok = Profiler.marker(name, valueJson);
                return FREObject.newObject(ok);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "profilerMarker failed", e);
                try { return FREObject.newObject(false); } catch (Exception ex) { return null; }
            }
        }
    }

    public static class ProfilerGetStatusDeep implements FREFunction {
        public static final String KEY = "awesomeUtils_profilerGetStatusDeep";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                String json = Profiler.getStatusDeep();
                return FREObject.newObject(json);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "profilerGetStatusDeep failed", e);
                try { return FREObject.newObject("{\"error\":\"status threw\"}"); } catch (Exception ex) { return null; }
            }
        }
    }

    /**
     * {@code awesomeUtils_profilerProbeEnter(methodId:int):Boolean}
     *
     * <p>Phase 3+4 method-entry probe injected by the AS3 compiler with
     * {@code --profile-probes}. Pushes the {@code methodId} onto the
     * per-thread method stack inside the DeepProfilerController. Native alloc
     * events captured by alloc_tracer inherit the current top-of-stack as
     * their {@code method_id} payload, enabling AS3 attribution of native
     * heap growth without reverse-engineering libCore.so AvmCore internals.
     */
    public static class ProfilerProbeEnter implements FREFunction {
        public static final String KEY = "awesomeUtils_profilerProbeEnter";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                if (args == null || args.length < 1 || args[0] == null) {
                    return FREObject.newObject(false);
                }
                int methodId = args[0].getAsInt();
                boolean ok = Profiler.probeEnter(methodId);
                return FREObject.newObject(ok);
            } catch (Exception e) {
                // Probes fire millions of times — silent failure on rare error.
                try { return FREObject.newObject(false); } catch (Exception ex) { return null; }
            }
        }
    }

    /**
     * {@code awesomeUtils_profilerProbeExit(methodId:int):Boolean}
     *
     * <p>Companion to {@link ProfilerProbeEnter}. Pops the per-thread method
     * stack at AS3 method exit. {@code methodId} is included for verification.
     */
    public static class ProfilerProbeExit implements FREFunction {
        public static final String KEY = "awesomeUtils_profilerProbeExit";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                if (args == null || args.length < 1 || args[0] == null) {
                    return FREObject.newObject(false);
                }
                int methodId = args[0].getAsInt();
                boolean ok = Profiler.probeExit(methodId);
                return FREObject.newObject(ok);
            } catch (Exception e) {
                try { return FREObject.newObject(false); } catch (Exception ex) { return null; }
            }
        }
    }

    /**
     * {@code awesomeUtils_profilerRegisterMethodTable(table:ByteArray):Boolean}
     *
     * <p>Register the method-id → name mapping. Called once at app startup
     * with a packed binary blob produced by the AS3 compiler's
     * {@code --profile-probes} pass. Used by {@code aneprof_analyze.py} to
     * render human-readable AS3 method names.
     */
    public static class ProfilerRegisterMethodTable implements FREFunction {
        public static final String KEY = "awesomeUtils_profilerRegisterMethodTable";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                if (args == null || args.length < 1 || args[0] == null) {
                    return FREObject.newObject(false);
                }
                FREByteArray ba = (FREByteArray) args[0];
                ba.acquire();
                byte[] bytes;
                try {
                    java.nio.ByteBuffer bb = ba.getBytes();
                    bytes = new byte[bb.remaining()];
                    bb.get(bytes);
                } finally {
                    ba.release();
                }
                boolean ok = Profiler.registerMethodTable(bytes);
                AneAwesomeUtilsLogging.i(TAG, "profilerRegisterMethodTable size=" + bytes.length + " ok=" + ok);
                return FREObject.newObject(ok);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "profilerRegisterMethodTable failed", e);
                try { return FREObject.newObject(false); } catch (Exception ex) { return null; }
            }
        }
    }

    /**
     * {@code awesomeUtils_profilerRecordFrame(frameIndex:Number, durationNs:Number,
     *                                          allocationCount:int, allocationBytes:Number,
     *                                          label:String):Boolean}
     *
     * <p>Phase 7b: explicit AS3-side Frame event. Distinct from Phase 6
     * RenderFrame events (auto-emitted from EGL hook) — used for scene
     * transitions, "battle_start", or any AS3-driven frame boundary marker.
     */
    public static class ProfilerRecordFrame implements FREFunction {
        public static final String KEY = "awesomeUtils_profilerRecordFrame";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                if (args == null || args.length < 2) return FREObject.newObject(false);
                long frameIndex = (args[0] != null) ? (long) args[0].getAsDouble() : 0L;
                long durationNs = (args[1] != null) ? (long) args[1].getAsDouble() : 0L;
                int  allocCount = (args.length > 2 && args[2] != null) ? args[2].getAsInt() : 0;
                long allocBytes = (args.length > 3 && args[3] != null) ? (long) args[3].getAsDouble() : 0L;
                String label    = (args.length > 4 && args[4] != null) ? args[4].getAsString() : "";
                boolean ok = Profiler.recordFrame(frameIndex, durationNs, allocCount, allocBytes, label);
                return FREObject.newObject(ok);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "profilerRecordFrame failed", e);
                try { return FREObject.newObject(false); } catch (Exception ex) { return null; }
            }
        }
    }

    /**
     * {@code awesomeUtils_profilerRequestGc():Boolean}
     *
     * <p>Phase 7a — programmatic GC trigger. Routes to {@code AndroidGcHook}
     * which calls {@code MMgc::GC::Collect} on the runtime-captured GC
     * singleton. Returns false until at least one Collect cycle has been
     * observed (singleton not yet known); AS3 should retry after a few
     * frames if the first call returns false.
     */
    public static class ProfilerRequestGc implements FREFunction {
        public static final String KEY = "awesomeUtils_profilerRequestGc";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                boolean ok = Profiler.requestGc();
                return FREObject.newObject(ok);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "profilerRequestGc failed", e);
                try { return FREObject.newObject(false); } catch (Exception ex) { return null; }
            }
        }
    }

    /**
     * Phase 4a RA — install diagnostic hook on every non-null sampler
     * vftable slot. Per-slot hit counts logged at uninstall.
     */
    public static class ProfilerSamplerHookInstall implements FREFunction {
        public static final String KEY = "awesomeUtils_profilerSamplerHookInstall";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                boolean ok = Profiler.samplerHookInstall();
                return FREObject.newObject(ok);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "samplerHookInstall failed", e);
                try { return FREObject.newObject(false); } catch (Exception ex) { return null; }
            }
        }
    }

    public static class ProfilerSamplerHookUninstall implements FREFunction {
        public static final String KEY = "awesomeUtils_profilerSamplerHookUninstall";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                boolean ok = Profiler.samplerHookUninstall();
                return FREObject.newObject(ok);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "samplerHookUninstall failed", e);
                try { return FREObject.newObject(false); } catch (Exception ex) { return null; }
            }
        }
    }

    public static class ProfilerAs3SamplerInstall implements FREFunction {
        public static final String KEY = "awesomeUtils_profilerAs3SamplerInstall";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                boolean ok = Profiler.as3SamplerInstall();
                return FREObject.newObject(ok);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "as3SamplerInstall failed", e);
                try { return FREObject.newObject(false); } catch (Exception ex) { return null; }
            }
        }
    }

    public static class ProfilerAs3SamplerUninstall implements FREFunction {
        public static final String KEY = "awesomeUtils_profilerAs3SamplerUninstall";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                boolean ok = Profiler.as3SamplerUninstall();
                return FREObject.newObject(ok);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "as3SamplerUninstall failed", e);
                try { return FREObject.newObject(false); } catch (Exception ex) { return null; }
            }
        }
    }

    /**
     * RA-only helper. Dumps AvmCore* via the captured GC singleton (gc+0x10).
     * Phase 4a sampler RA — take pre/post snapshots around startSampling().
     */
    public static class ProfilerDumpAvmCore implements FREFunction {
        public static final String KEY = "awesomeUtils_profilerDumpAvmCore";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                String label = "?";
                if (args != null && args.length >= 1 && args[0] != null) {
                    label = args[0].getAsString();
                }
                boolean ok = Profiler.dumpAvmCore(label);
                return FREObject.newObject(ok);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "profilerDumpAvmCore failed", e);
                try { return FREObject.newObject(false); } catch (Exception ex) { return null; }
            }
        }
    }

    /**
     * {@code awesomeUtils_allocTracerStart():int}
     *
     * <p>Installs PLT/GOT hooks on libCore.so for malloc/calloc/realloc/free/
     * mmap/munmap. Allocations >= 64 KB are recorded with full stack traces.
     * Returns 1=ok, 0=already active, -1=failure.
     */
    public static class AllocTracerStart implements FREFunction {
        public static final String KEY = "awesomeUtils_allocTracerStart";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                int rc = AllocTracer.start();
                AneAwesomeUtilsLogging.i(TAG, "allocTracerStart rc=" + rc);
                return FREObject.newObject(rc);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "allocTracerStart failed", e);
                try { return FREObject.newObject(-1); } catch (Exception ex) { return null; }
            }
        }
    }

    /**
     * {@code awesomeUtils_allocTracerStop():int}
     *
     * <p>Uninstalls allocation hooks. Returns 1=ok, 0=was not active.
     */
    public static class AllocTracerStop implements FREFunction {
        public static final String KEY = "awesomeUtils_allocTracerStop";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                int rc = AllocTracer.stop();
                AneAwesomeUtilsLogging.i(TAG, "allocTracerStop rc=" + rc);
                return FREObject.newObject(rc);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "allocTracerStop failed", e);
                try { return FREObject.newObject(-1); } catch (Exception ex) { return null; }
            }
        }
    }

    /**
     * {@code awesomeUtils_allocTracerDump(topN:int):String}
     *
     * <p>Returns JSON of live (surviving) allocations sorted by size desc.
     * Each entry has addr/size/kind/tsMs/stack[symbolized]. Caller can drain
     * and call again later for incremental analysis.
     */
    public static class AllocTracerDump implements FREFunction {
        public static final String KEY = "awesomeUtils_allocTracerDump";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                int topN = -1;
                if (args != null && args.length > 0 && args[0] != null) {
                    topN = args[0].getAsInt();
                }
                String json = AllocTracer.dumpAllocs(topN);
                return FREObject.newObject(json);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "allocTracerDump failed", e);
                try { return FREObject.newObject("{\"error\":\"dump threw\"}"); } catch (Exception ex) { return null; }
            }
        }
    }

    /**
     * {@code awesomeUtils_allocTracerMark(name:String):int}
     *
     * <p>Tag the current capture phase. Subsequent allocs are stamped with
     * the given name until the next mark. Used to attribute leaked
     * allocations to game phases (matchroom_enter, battle_start, etc.).
     * Returns the assigned phase id.
     */
    public static class AllocTracerMark implements FREFunction {
        public static final String KEY = "awesomeUtils_allocTracerMark";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                String name = (args != null && args.length > 0 && args[0] != null)
                        ? args[0].getAsString() : "";
                int rc = AllocTracer.markPhase(name);
                AneAwesomeUtilsLogging.i(TAG, "allocTracerMark name=" + name + " id=" + rc);
                return FREObject.newObject(rc);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "allocTracerMark failed", e);
                try { return FREObject.newObject(-1); } catch (Exception ex) { return null; }
            }
        }
    }

    /**
     * {@code awesomeUtils_allocTracerPurgeStalePhase(substr:String, minAgeMs:int, maxFree:int):String}
     *
     * <p>Walk the live alloc table, free pointers whose phase name contains
     * {@code substr} AND were alloc'd more than {@code minAgeMs} ago.
     * Returns JSON with scanned/matched/freed counts. AS3 caller MUST
     * guarantee AS3 GC + mallopt(M_PURGE_ALL) ran before invoking this and
     * that the matching phases are logically dead.
     */
    public static class AllocTracerPurgeStalePhase implements FREFunction {
        public static final String KEY = "awesomeUtils_allocTracerPurgeStalePhase";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                String substr = (args != null && args.length > 0 && args[0] != null)
                        ? args[0].getAsString() : "";
                int minAgeMs = (args != null && args.length > 1 && args[1] != null)
                        ? args[1].getAsInt() : 1000;
                int maxFree = (args != null && args.length > 2 && args[2] != null)
                        ? args[2].getAsInt() : 100000;
                String json = AllocTracer.purgeStalePhase(substr, minAgeMs, maxFree);
                AneAwesomeUtilsLogging.i(TAG, "allocTracerPurgeStalePhase " + json);
                return FREObject.newObject(json);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "allocTracerPurgeStalePhase failed", e);
                try { return FREObject.newObject("{\"error\":\"purge threw\"}"); } catch (Exception ex) { return null; }
            }
        }
    }

    /**
     * {@code awesomeUtils_deferDrainInstall():int}
     *
     * <p>Installs the libCore.so deferred-destruction force-drain workaround.
     * Hooks the BitmapData/Texture destructor and starts a background thread
     * that periodically invokes Adobe's own deferred-completion function on
     * pending owner structs. Returns 1 on success, -1 on failure.
     */
    public static class DeferDrainInstall implements FREFunction {
        public static final String KEY = "awesomeUtils_deferDrainInstall";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                int rc = DeferDrain.install();
                AneAwesomeUtilsLogging.i(TAG, "deferDrainInstall rc=" + rc);
                return FREObject.newObject(rc);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "deferDrainInstall failed", e);
                try { return FREObject.newObject(-1); } catch (Exception ex) { return null; }
            }
        }
    }

    public static class DeferDrainUninstall implements FREFunction {
        public static final String KEY = "awesomeUtils_deferDrainUninstall";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                int rc = DeferDrain.uninstall();
                AneAwesomeUtilsLogging.i(TAG, "deferDrainUninstall rc=" + rc);
                return FREObject.newObject(rc);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "deferDrainUninstall failed", e);
                try { return FREObject.newObject(-1); } catch (Exception ex) { return null; }
            }
        }
    }

    public static class DeferDrainStatus implements FREFunction {
        public static final String KEY = "awesomeUtils_deferDrainStatus";
        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                String json = DeferDrain.getStatus();
                return FREObject.newObject(json);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "deferDrainStatus failed", e);
                try { return FREObject.newObject("{\"error\":\"status threw\"}"); } catch (Exception ex) { return null; }
            }
        }
    }

    /**
     * {@code awesomeUtils_setAllocatorDecayTime(seconds:int):int}
     *
     * <p>Calls bionic {@code mallopt(M_DECAY_TIME, seconds)}. Default decay
     * is ~1 s on most devices — meaning scudo holds freed slabs in cache for
     * 1 s before returning them to the kernel via {@code madvise(MADV_DONTNEED)}.
     * Setting to 0 makes free → immediate release (lower steady-state RSS,
     * marginal alloc cost).
     *
     * <p>Returns the int rc from mallopt (1 on success, 0 on failure).
     */
    public static class SetAllocatorDecayTime implements FREFunction {
        public static final String KEY = "awesomeUtils_setAllocatorDecayTime";

        private static final int M_DECAY_TIME = -100;

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                int seconds = 0;
                if (args != null && args.length > 0 && args[0] != null) {
                    seconds = args[0].getAsInt();
                }
                int rc = ProbeNative.nativeMallopt(M_DECAY_TIME, seconds);
                AneAwesomeUtilsLogging.i(TAG, "mallopt(M_DECAY_TIME, " + seconds + ") rc=" + rc);
                return FREObject.newObject(rc);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "setAllocatorDecayTime failed", e);
                try { return FREObject.newObject(-1); } catch (Exception ex) { return null; }
            }
        }
    }

    public static class TriggerMemoryPurge implements FREFunction {
        public static final String KEY = "awesomeUtils_triggerMemoryPurge";

        // M_PURGE_ALL — bionic-specific aggressive purge including caches.
        private static final int M_PURGE_ALL = -104;

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                long nativeBeforeKb = android.os.Debug.getNativeHeapAllocatedSize() / 1024L;
                long[] mapsBefore = ProbeNative.nativeProbeMaps();
                long vmaBefore       = mapsBefore != null ? mapsBefore[ProbeNative.SLOT_TOTAL] : -1L;
                long secondaryBefore = mapsBefore != null ? mapsBefore[ProbeNative.SLOT_SCUDO_SECONDARY] : -1L;
                long t0 = System.nanoTime();

                int rc = ProbeNative.nativeMallopt(M_PURGE_ALL, 0);

                long durationUs = (System.nanoTime() - t0) / 1000L;
                long nativeAfterKb = android.os.Debug.getNativeHeapAllocatedSize() / 1024L;
                long[] mapsAfter = ProbeNative.nativeProbeMaps();
                long vmaAfter       = mapsAfter != null ? mapsAfter[ProbeNative.SLOT_TOTAL] : -1L;
                long secondaryAfter = mapsAfter != null ? mapsAfter[ProbeNative.SLOT_SCUDO_SECONDARY] : -1L;

                String json = new StringBuilder(256)
                        .append("{\"rc\":").append(rc)
                        .append(",\"durationUs\":").append(durationUs)
                        .append(",\"nativeBeforeKb\":").append(nativeBeforeKb)
                        .append(",\"nativeAfterKb\":").append(nativeAfterKb)
                        .append(",\"nativeDeltaKb\":").append(nativeAfterKb - nativeBeforeKb)
                        .append(",\"vmaBefore\":").append(vmaBefore)
                        .append(",\"vmaAfter\":").append(vmaAfter)
                        .append(",\"vmaDelta\":").append(vmaAfter - vmaBefore)
                        .append(",\"secondaryBefore\":").append(secondaryBefore)
                        .append(",\"secondaryAfter\":").append(secondaryAfter)
                        .append(",\"secondaryDelta\":").append(secondaryAfter - secondaryBefore)
                        .append('}').toString();
                AneAwesomeUtilsLogging.i("ProbePurge", json);
                return FREObject.newObject(json);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "triggerMemoryPurge failed", e);
                try { return FREObject.newObject("{\"error\":\"purge threw\"}"); } catch (Exception ex) { return null; }
            }
        }
    }

    /**
     * {@code awesomeUtils_probeMapsByPath():String}
     *
     * <p>Returns one snapshot of {@code /proc/self/maps} aggregated per
     * trailing-name field, as JSON:
     * <pre>{"ts":N,"totalCount":N,"totalSizeKb":N,"byPath":{"path":{"count":N,"sizeKb":N},...}}</pre>
     *
     * <p>The diff between two snapshots (e.g., baseline vs post-battle) tells
     * exactly which lib/cookie grew: {@code [anon:libc_malloc]} for native
     * heap chunks, {@code /dev/kgsl-3d0} for GPU textures, file paths for
     * mmap'd assets, etc.
     *
     * <p>Cheaper than a host-side {@code adb shell run-as cat /proc/<pid>/maps}
     * poll because it stays in the AS3 thread tick (no adb round trip, no
     * extra subprocess per cycle).
     */
    public static class ProbeMapsByPath implements FREFunction {
        public static final String KEY = "awesomeUtils_probeMapsByPath";

        @Override
        public FREObject call(FREContext context, FREObject[] args) {
            try {
                String json = ProbeNative.nativeProbeMapsByPath();
                if (json == null) {
                    return FREObject.newObject("{\"error\":\"probeMapsByPath read failed\"}");
                }
                // Splice the AS3-side timestamp at the head — Java has clean
                // System.currentTimeMillis() while the C side avoided pulling
                // <ctime> just for this.
                long ts = System.currentTimeMillis();
                String withTs = "{\"ts\":" + ts + "," + json.substring(1);
                return FREObject.newObject(withTs);
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "probeMapsByPath failed", e);
                try { return FREObject.newObject("{\"error\":\"probeMapsByPath threw\"}"); } catch (Exception ex) { return null; }
            }
        }
    }

    // Helpers shared by ProbeTick — duplicated from RuntimeStatsCollector to
    // avoid widening that class's API surface for a one-off caller.

    private static int countThreads() {
        String[] names = new java.io.File("/proc/self/task").list();
        return names == null ? -1 : names.length;
    }

    /** Returns {@code [vmRssKb, vmSizeKb]}. Both are {@code 0} on read failure. */
    private static long[] readProcStatusKb() {
        long vmRssKb = 0L, vmSizeKb = 0L;
        try (java.io.BufferedReader r = new java.io.BufferedReader(
                new java.io.FileReader("/proc/self/status"))) {
            String line;
            while ((line = r.readLine()) != null) {
                if      (line.startsWith("VmRSS:"))  vmRssKb  = parseKb(line);
                else if (line.startsWith("VmSize:")) vmSizeKb = parseKb(line);
            }
        } catch (Throwable ignored) { /* best-effort */ }
        return new long[]{vmRssKb, vmSizeKb};
    }

    private static long parseKb(String line) {
        long v = 0L;
        for (int i = 0, n = line.length(); i < n; i++) {
            char c = line.charAt(i);
            if (c >= '0' && c <= '9') v = v * 10L + (c - '0');
        }
        return v;
    }
}
