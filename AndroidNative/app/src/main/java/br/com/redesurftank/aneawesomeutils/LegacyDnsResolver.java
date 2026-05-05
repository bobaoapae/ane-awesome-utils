// SPDX-License-Identifier: MIT
package br.com.redesurftank.aneawesomeutils;

import androidx.annotation.NonNull;

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.UnknownHostException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.TimeUnit;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.ResponseBody;

/**
 * Pure-okhttp + raw-UDP DNS resolver for Android API &lt; 24 (Android 7).
 *
 * <p>Why this exists: the project's primary path uses dnsjava 3.x which
 * pulls in {@code java.util.concurrent.CompletionStage} and
 * {@code CompletableFuture}. Those classes don't exist on Android 6
 * (API 23), and Android Gradle Plugin's core library desugaring config
 * (as of {@code desugar_jdk_libs_configuration_nio:2.1.5}) does NOT
 * rewrite them - verified by inspection of the spec JSON shipped with
 * the artifact (no Completable / CompletionStage entries in
 * {@code common_flags.rewrite_type}). Any class that references those
 * types on Android 6 fails to verify with
 * "[0xNN] monitor-exit on non-object (Conflict)" and kills the process.
 *
 * <p>This resolver fulfills the same DNS-poisoning-resistance role as
 * {@link InternalDnsResolver#resolveWithDns(String)} (multi-resolver
 * concurrent fan-out, race for first non-empty answer), but the bytecode
 * NEVER touches a dnsjava class - so dnsjava classes are not loaded by
 * the runtime verifier on Android 6 and the
 * {@code org.xbill.DNS.AsyncSemaphore} VerifyError never surfaces.
 *
 * <p>Resolvers (in fan-out order, race-to-finish):
 * <ul>
 *   <li>Cloudflare DoH JSON: {@code https://1.1.1.1/dns-query?name=H&type=A}
 *   <li>Google DoH JSON:    {@code https://dns.google/resolve?name=H&type=A}
 *   <li>AdGuard DoH JSON:   {@code https://unfiltered.adguard-dns.com/resolve?name=H&type=A}
 *   <li>Cloudflare UDP:     1.1.1.1:53 raw DNS query
 *   <li>Google UDP:         8.8.8.8:53 raw DNS query
 *   <li>System DNS:         {@link InetAddress#getAllByName(String)} (delayed 2s
 *                           to give priority to custom resolvers)
 * </ul>
 *
 * <p>Each resolver runs on its own thread; the first one with a
 * non-empty answer wins. Other threads are interrupted/abandoned.
 */
final class LegacyDnsResolver {

    private static final long SYSTEM_DNS_DELAY_MS = 2_000L;

    /** Shared OkHttp client used by the DoH JSON probes. Short timeouts. */
    private static final OkHttpClient HTTP = new OkHttpClient.Builder()
            .connectTimeout(3, TimeUnit.SECONDS)
            .readTimeout(3, TimeUnit.SECONDS)
            .callTimeout(5, TimeUnit.SECONDS)
            .build();

    private LegacyDnsResolver() {
    }

    static List<InetAddress> resolve(@NonNull String host) {
        final List<InetAddress> winner = new ArrayList<>();

        Thread cfDoh = thread("dns-cf-doh", () -> {
            try { merge(winner, resolveDoh("https://1.1.1.1/dns-query?name=" + host + "&type=A")); }
            catch (Exception e) { /* ignore - racer */ }
        });
        Thread gDoh = thread("dns-google-doh", () -> {
            try { merge(winner, resolveDoh("https://dns.google/resolve?name=" + host + "&type=A")); }
            catch (Exception e) { /* ignore */ }
        });
        Thread adgDoh = thread("dns-adguard-doh", () -> {
            try { merge(winner, resolveDoh("https://unfiltered.adguard-dns.com/resolve?name=" + host + "&type=A")); }
            catch (Exception e) { /* ignore */ }
        });
        Thread cfUdp = thread("dns-cf-udp", () -> {
            try { merge(winner, resolveUdp("1.1.1.1", host)); }
            catch (Exception e) { /* ignore */ }
        });
        Thread gUdp = thread("dns-google-udp", () -> {
            try { merge(winner, resolveUdp("8.8.8.8", host)); }
            catch (Exception e) { /* ignore */ }
        });
        Thread sysDns = thread("dns-system", () -> {
            try {
                Thread.sleep(SYSTEM_DNS_DELAY_MS);
                synchronized (winner) {
                    if (!winner.isEmpty()) return;
                }
                InetAddress[] sys = InetAddress.getAllByName(host);
                List<InetAddress> tmp = new ArrayList<>();
                for (InetAddress a : sys) tmp.add(a);
                merge(winner, tmp);
            } catch (Exception e) { /* ignore */ }
        });

        cfDoh.start();
        gDoh.start();
        adgDoh.start();
        cfUdp.start();
        gUdp.start();
        sysDns.start();

        long deadline = System.currentTimeMillis() + 6000L;
        while (true) {
            synchronized (winner) {
                if (!winner.isEmpty()) break;
            }
            boolean anyAlive = cfDoh.isAlive() || gDoh.isAlive() || adgDoh.isAlive()
                    || cfUdp.isAlive() || gUdp.isAlive() || sysDns.isAlive();
            if (!anyAlive || System.currentTimeMillis() >= deadline) break;
            try { Thread.sleep(20L); } catch (InterruptedException ignored) { Thread.currentThread().interrupt(); break; }
        }

        synchronized (winner) {
            return new ArrayList<>(winner);
        }
    }

    private static Thread thread(String name, Runnable r) {
        Thread t = new Thread(r, name);
        t.setDaemon(true);
        return t;
    }

    private static void merge(List<InetAddress> winner, List<InetAddress> add) {
        if (add == null || add.isEmpty()) return;
        synchronized (winner) {
            if (winner.isEmpty()) winner.addAll(add);
        }
    }

    /**
     * DoH JSON probe. Both Cloudflare ({@code application/dns-json}) and
     * Google ({@code dns.google/resolve}) speak the same JSON dialect:
     * {@code {"Status":0, "Answer":[{"name":"...", "type":1, "TTL":..,
     * "data":"1.2.3.4"}, ...]}}.
     */
    private static List<InetAddress> resolveDoh(String url) throws IOException {
        Request req = new Request.Builder()
                .url(url)
                .header("accept", "application/dns-json")
                .build();
        try (Response resp = HTTP.newCall(req).execute()) {
            if (!resp.isSuccessful()) return null;
            ResponseBody body = resp.body();
            if (body == null) return null;
            String json = body.string();
            return parseDohJson(json);
        }
    }

    /** Lightweight JSON parser - only digs out IPv4 strings under
     *  {@code Answer[*].data}. Avoids pulling in a JSON lib. */
    private static List<InetAddress> parseDohJson(String json) {
        List<InetAddress> out = new ArrayList<>();
        // Locate "Answer":[...]
        int answerIdx = json.indexOf("\"Answer\"");
        if (answerIdx < 0) answerIdx = json.indexOf("\"answer\"");
        if (answerIdx < 0) return out;
        int arrayStart = json.indexOf('[', answerIdx);
        if (arrayStart < 0) return out;
        int depth = 0;
        int arrayEnd = -1;
        for (int i = arrayStart; i < json.length(); i++) {
            char c = json.charAt(i);
            if (c == '[') depth++;
            else if (c == ']') { depth--; if (depth == 0) { arrayEnd = i; break; } }
        }
        if (arrayEnd < 0) return out;
        String slice = json.substring(arrayStart, arrayEnd);
        // Find every "data":"..." in the slice
        int cursor = 0;
        while (true) {
            int k = slice.indexOf("\"data\"", cursor);
            if (k < 0) break;
            int colon = slice.indexOf(':', k);
            if (colon < 0) break;
            int q1 = slice.indexOf('"', colon + 1);
            if (q1 < 0) break;
            int q2 = slice.indexOf('"', q1 + 1);
            if (q2 < 0) break;
            String value = slice.substring(q1 + 1, q2);
            cursor = q2 + 1;
            if (looksLikeIpv4(value)) {
                try { out.add(InetAddress.getByName(value)); }
                catch (UnknownHostException ignored) { }
            }
        }
        return out;
    }

    private static boolean looksLikeIpv4(String s) {
        if (s == null || s.length() < 7 || s.length() > 15) return false;
        int dots = 0;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '.') dots++;
            else if (c < '0' || c > '9') return false;
        }
        return dots == 3;
    }

    /**
     * Raw UDP DNS query. Builds a minimal Standard Query for A records,
     * sends to {@code resolverIp:53}, parses Answer section for A records.
     * No dnsjava - wire format is just bytes.
     */
    private static List<InetAddress> resolveUdp(String resolverIp, String host) throws IOException {
        byte[] query = buildDnsQuery(host);
        DatagramSocket socket = new DatagramSocket();
        try {
            socket.setSoTimeout(3000);
            InetAddress resolver = InetAddress.getByName(resolverIp);
            DatagramPacket sendPkt = new DatagramPacket(query, query.length, resolver, 53);
            socket.send(sendPkt);
            byte[] buf = new byte[1500];
            DatagramPacket recvPkt = new DatagramPacket(buf, buf.length);
            socket.receive(recvPkt);
            return parseDnsAnswerForA(buf, recvPkt.getLength());
        } finally {
            socket.close();
        }
    }

    /** Build a basic DNS query: 12-byte header + QNAME + QTYPE=A + QCLASS=IN. */
    private static byte[] buildDnsQuery(String host) {
        byte[] qname = encodeName(host);
        byte[] out = new byte[12 + qname.length + 4];
        // Transaction ID (random)
        int txid = (int) (System.nanoTime() & 0xFFFF);
        out[0] = (byte) (txid >>> 8);
        out[1] = (byte) (txid & 0xFF);
        // Flags: standard query, recursion desired
        out[2] = 0x01;
        out[3] = 0x00;
        // QDCOUNT=1, AN/NS/AR=0
        out[4] = 0x00; out[5] = 0x01;
        // QNAME
        System.arraycopy(qname, 0, out, 12, qname.length);
        int p = 12 + qname.length;
        // QTYPE=A
        out[p] = 0x00; out[p + 1] = 0x01;
        // QCLASS=IN
        out[p + 2] = 0x00; out[p + 3] = 0x01;
        return out;
    }

    /** Encode host as DNS labels: [len][label][len][label]...0. */
    private static byte[] encodeName(String host) {
        String[] labels = host.toLowerCase(Locale.ROOT).split("\\.");
        int total = 1; // trailing zero
        for (String l : labels) total += 1 + l.length();
        byte[] out = new byte[total];
        int p = 0;
        for (String l : labels) {
            byte[] lb = l.getBytes();
            out[p++] = (byte) lb.length;
            System.arraycopy(lb, 0, out, p, lb.length);
            p += lb.length;
        }
        out[p] = 0;
        return out;
    }

    /** Parse DNS response, return all A-record IPv4 addresses from Answer section. */
    private static List<InetAddress> parseDnsAnswerForA(byte[] buf, int len) throws IOException {
        List<InetAddress> out = new ArrayList<>();
        if (len < 12) return out;
        int qdCount = ((buf[4] & 0xFF) << 8) | (buf[5] & 0xFF);
        int anCount = ((buf[6] & 0xFF) << 8) | (buf[7] & 0xFF);
        int p = 12;
        // Skip QDs
        for (int i = 0; i < qdCount; i++) {
            p = skipName(buf, p);
            p += 4; // QTYPE + QCLASS
        }
        // Read ANs
        for (int i = 0; i < anCount && p < len; i++) {
            p = skipName(buf, p);
            if (p + 10 > len) return out;
            int type = ((buf[p] & 0xFF) << 8) | (buf[p + 1] & 0xFF);
            int rdLen = ((buf[p + 8] & 0xFF) << 8) | (buf[p + 9] & 0xFF);
            p += 10;
            if (p + rdLen > len) return out;
            if (type == 1 && rdLen == 4) {
                byte[] addr = new byte[4];
                System.arraycopy(buf, p, addr, 0, 4);
                out.add(InetAddress.getByAddress(addr));
            }
            p += rdLen;
        }
        return out;
    }

    /** Skip an encoded DNS name, accounting for compressed pointers. */
    private static int skipName(byte[] buf, int p) {
        while (p < buf.length) {
            int len = buf[p] & 0xFF;
            if (len == 0) return p + 1;
            if ((len & 0xC0) == 0xC0) return p + 2; // pointer
            p += 1 + len;
        }
        return p;
    }
}
