package br.com.redesurftank.aneawesomeutils;

import static br.com.redesurftank.aneawesomeutils.AneAwesomeUtilsExtension.TAG;

import android.os.Build;

import androidx.annotation.NonNull;
import androidx.annotation.RequiresApi;

import org.xbill.DNS.DClass;
import org.xbill.DNS.DohResolver;
import org.xbill.DNS.Message;
import org.xbill.DNS.Name;
import org.xbill.DNS.Record;
import org.xbill.DNS.Resolver;
import org.xbill.DNS.Section;
import org.xbill.DNS.SimpleResolver;
import org.xbill.DNS.Type;

import java.net.InetAddress;
import java.net.UnknownHostException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

import okhttp3.Dns;

public class InternalDnsResolver {

    private static final InternalDnsResolver instance = new InternalDnsResolver();
    private static final long NEGATIVE_CACHE_TTL_MS = 30_000;
    private static final long SYSTEM_DNS_DELAY_MS = 2_000;

    public static InternalDnsResolver getInstance() {
        return instance;
    }

    private final Map<String, List<String>> _staticHosts;
    private final Map<String, List<InetAddress>> _resolvedHosts;
    private final Map<String, Long> _negativeCache;
    private final ExecutorService _executor;

    private InternalDnsResolver() {
        _staticHosts = new HashMap<>();
        _resolvedHosts = new HashMap<>();
        _negativeCache = new HashMap<>();
        // Bounded pool sized for max fan-out of a single lookup (7 resolvers:
        // 3 DoH + 3 UDP + 1 system). Previous newCachedThreadPool was unbounded
        // and kept idle threads for 60s, letting reconnect storms pile up 50+
        // threads and contributing to Thread.nativeCreate OOM (Play Console #3/#12).
        // DiscardOldestPolicy drops the oldest queued task under overload — DNS
        // results are idempotently cached so a dropped lookup just retries.
        _executor = new ThreadPoolExecutor(
                2, 7,
                30L, TimeUnit.SECONDS,
                new LinkedBlockingQueue<>(21),
                new ThreadPoolExecutor.DiscardOldestPolicy());
    }

    public void addStaticHost(String host, String address) {
        synchronized (_staticHosts) {
            List<String> addresses = _staticHosts.get(host);
            if (addresses == null) {
                addresses = new java.util.ArrayList<>();
                _staticHosts.put(host, addresses);
            }
            addresses.add(address);
        }
    }

    public void removeStaticHost(String host) {
        synchronized (_staticHosts) {
            _staticHosts.remove(host);
        }
    }

    public List<InetAddress> resolveHost(@NonNull String host) throws UnknownHostException {
        List<InetAddress> addresses = new ArrayList<>();

        synchronized (_resolvedHosts) {
            if (_resolvedHosts.containsKey(host)) {
                List<InetAddress> ips = _resolvedHosts.get(host);
                addresses.addAll(ips);
                if (!addresses.isEmpty()) {
                    return addresses;
                }
            }
        }

        // Check negative cache to avoid flooding DNS resolvers on repeated failures
        synchronized (_negativeCache) {
            Long failedAt = _negativeCache.get(host);
            if (failedAt != null && (System.currentTimeMillis() - failedAt) < NEGATIVE_CACHE_TTL_MS) {
                // Recently failed - go straight to system DNS
                addresses.addAll(Dns.SYSTEM.lookup(host));
                return addresses;
            }
        }

        try {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.N) {
                addresses.addAll(resolveDnsUsingThreadForLowApi(host));
            } else {
                List<InetAddress> fromResolversResult = resolveWithDns(host).join();
                addresses.addAll(fromResolversResult);
            }
        } catch (Exception e) {
            AneAwesomeUtilsLogging.d(TAG, "Custom DNS resolution failed for " + host + ", falling back to system DNS");
        }

        if (!addresses.isEmpty()) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                addresses.sort(new InetAddressComparator());
            }
            synchronized (_resolvedHosts) {
                _resolvedHosts.put(host, addresses);
            }
            // Clear negative cache on success
            synchronized (_negativeCache) {
                _negativeCache.remove(host);
            }
            return addresses;
        }

        synchronized (_staticHosts) {
            try {
                if (_staticHosts.containsKey(host)) {
                    List<String> ips = _staticHosts.get(host);
                    for (String ip : ips) {
                        addresses.add(InetAddress.getByName(ip));
                    }
                    if (!addresses.isEmpty()) {
                        return addresses;
                    }
                }
            } catch (UnknownHostException e) {
                AneAwesomeUtilsLogging.e(TAG, "Error in lookup() : " + e.getMessage(), e);
            }
        }

        try {
            addresses.addAll(Dns.SYSTEM.lookup(host));
        } catch (UnknownHostException e) {
            // All resolution methods failed - add to negative cache
            synchronized (_negativeCache) {
                _negativeCache.put(host, System.currentTimeMillis());
            }
            throw e;
        }

        return addresses;
    }

    private InetAddress getByIpWithoutException(String ip) {
        try {
            return InetAddress.getByName(ip);
        } catch (UnknownHostException e) {
            AneAwesomeUtilsLogging.e(TAG, "Failure in getByIpWithoutException() : " + e.getMessage(), e);
            return null;
        }
    }

    private List<InetAddress> resolveDnsUsingThreadForLowApi(String domain) {
        AneAwesomeUtilsLogging.d(TAG, "resolveDnsUsingThreadForLowApi() called with: domain = [" + domain + "]");
        List<InetAddress> addresses = new ArrayList<>();
        Thread cloudflareThread = new Thread(() -> {
            try {
                List<InetAddress> cloudflareAddresses = resolveDns(new DohResolver("https://1.1.1.1/dns-query"), domain);
                synchronized (addresses) {
                    if (!addresses.isEmpty())
                        return;
                    AneAwesomeUtilsLogging.d(TAG, "resolveDnsUsingThreadForLowApi() resolved with cloudflare");
                    addresses.addAll(cloudflareAddresses);
                }
            } catch (Exception e) {
                AneAwesomeUtilsLogging.d(TAG, "Failure in resolveDnsUsingThreadForLowApi() cloudflare: " + e.getMessage());
            }
        });
        Thread googleThread = new Thread(() -> {
            try {
                List<InetAddress> googleAddresses = resolveDns(new DohResolver("https://dns.google/dns-query"), domain);
                synchronized (addresses) {
                    if (!addresses.isEmpty())
                        return;
                    AneAwesomeUtilsLogging.d(TAG, "resolveDnsUsingThreadForLowApi() resolved with google");
                    addresses.addAll(googleAddresses);
                }
            } catch (Exception e) {
                AneAwesomeUtilsLogging.d(TAG, "Failure in resolveDnsUsingThreadForLowApi() google: " + e.getMessage());
            }
        });
        Thread adguardThread = new Thread(() -> {
            try {
                List<InetAddress> adguardAddresses = resolveDns(new DohResolver("https://unfiltered.adguard-dns.com/dns-query"), domain);
                synchronized (addresses) {
                    if (!addresses.isEmpty())
                        return;
                    AneAwesomeUtilsLogging.d(TAG, "resolveDnsUsingThreadForLowApi() resolved with adguard");
                    addresses.addAll(adguardAddresses);
                }
            } catch (Exception e) {
                AneAwesomeUtilsLogging.d(TAG, "Failure in resolveDnsUsingThreadForLowApi() adguard: " + e.getMessage());
            }
        });
        Thread cloudflareNormalThread = new Thread(() -> {
            try {
                List<InetAddress> cloudflareAddresses = resolveDns(new SimpleResolver(Objects.requireNonNull(getByIpWithoutException("1.1.1.1"))), domain);
                synchronized (addresses) {
                    if (!addresses.isEmpty())
                        return;
                    AneAwesomeUtilsLogging.d(TAG, "resolveDnsUsingThreadForLowApi() resolved with cloudflare normal");
                    addresses.addAll(cloudflareAddresses);
                }
            } catch (Exception e) {
                AneAwesomeUtilsLogging.d(TAG, "Failure in resolveDnsUsingThreadForLowApi() cloudflare normal: " + e.getMessage());
            }
        });
        Thread googleNormalThread = new Thread(() -> {
            try {
                List<InetAddress> googleAddresses = resolveDns(new SimpleResolver(Objects.requireNonNull(getByIpWithoutException("8.8.8.8"))), domain);
                synchronized (addresses) {
                    if (!addresses.isEmpty())
                        return;
                    AneAwesomeUtilsLogging.d(TAG, "resolveDnsUsingThreadForLowApi() resolved with google normal");
                    addresses.addAll(googleAddresses);
                }
            } catch (Exception e) {
                AneAwesomeUtilsLogging.d(TAG, "Failure in resolveDnsUsingThreadForLowApi() google normal: " + e.getMessage());
            }
        });
        Thread adGuardNormalThread = new Thread(() -> {
            try {
                List<InetAddress> adguardAddresses = resolveDns(new SimpleResolver(Objects.requireNonNull(getByIpWithoutException("94.140.14.140"))), domain);
                synchronized (addresses) {
                    if (!addresses.isEmpty())
                        return;
                    AneAwesomeUtilsLogging.d(TAG, "resolveDnsUsingThreadForLowApi() resolved with adguard normal");
                    addresses.addAll(adguardAddresses);
                }
            } catch (Exception e) {
                AneAwesomeUtilsLogging.d(TAG, "Failure in resolveDnsUsingThreadForLowApi() adguard normal: " + e.getMessage());
            }
        });
        Thread systemDnsThread = new Thread(() -> {
            try {
                // Delay to give priority to custom resolvers over potentially poisoned system DNS
                Thread.sleep(SYSTEM_DNS_DELAY_MS);
                synchronized (addresses) {
                    if (!addresses.isEmpty())
                        return;
                }
                List<InetAddress> systemAddresses = Dns.SYSTEM.lookup(domain);
                synchronized (addresses) {
                    if (!addresses.isEmpty())
                        return;
                    AneAwesomeUtilsLogging.d(TAG, "resolveDnsUsingThreadForLowApi() resolved with system DNS");
                    addresses.addAll(systemAddresses);
                }
            } catch (Exception e) {
                AneAwesomeUtilsLogging.d(TAG, "Failure in resolveDnsUsingThreadForLowApi() system DNS: " + e.getMessage());
            }
        });

        cloudflareThread.start();
        googleThread.start();
        adguardThread.start();
        cloudflareNormalThread.start();
        googleNormalThread.start();
        adGuardNormalThread.start();
        systemDnsThread.start();

        while (true) {
            synchronized (addresses) {
                if (!addresses.isEmpty()) {
                    break;
                }
            }
            if (!cloudflareThread.isAlive() && !googleThread.isAlive() && !adguardThread.isAlive()
                    && !cloudflareNormalThread.isAlive() && !googleNormalThread.isAlive()
                    && !adGuardNormalThread.isAlive() && !systemDnsThread.isAlive()) {
                break;
            }
            try {
                Thread.sleep(100);
            } catch (InterruptedException e) {
                throw new RuntimeException(e);
            }
        }

        return addresses;
    }

    @RequiresApi(api = Build.VERSION_CODES.N)
    private CompletableFuture<List<InetAddress>> resolveWithDns(String domain) {
        AneAwesomeUtilsLogging.d(TAG, "resolveWithDns() called with: domain = [" + domain + "]");
        CompletableFuture<List<InetAddress>> cloudflareFuture = CompletableFuture.supplyAsync(() -> resolveDns(new DohResolver("https://1.1.1.1/dns-query"), domain), _executor);
        CompletableFuture<List<InetAddress>> googleFuture = CompletableFuture.supplyAsync(() -> resolveDns(new DohResolver("https://dns.google/dns-query"), domain), _executor);
        CompletableFuture<List<InetAddress>> adguardFuture = CompletableFuture.supplyAsync(() -> resolveDns(new DohResolver("https://unfiltered.adguard-dns.com/dns-query"), domain), _executor);
        CompletableFuture<List<InetAddress>> cloudflareNormalFuture = CompletableFuture.supplyAsync(() -> resolveDns(new SimpleResolver(Objects.requireNonNull(getByIpWithoutException("1.1.1.1"))), domain), _executor);
        CompletableFuture<List<InetAddress>> googleNormalFuture = CompletableFuture.supplyAsync(() -> resolveDns(new SimpleResolver(Objects.requireNonNull(getByIpWithoutException("8.8.8.8"))), domain), _executor);
        CompletableFuture<List<InetAddress>> adguardNormalFuture = CompletableFuture.supplyAsync(() -> resolveDns(new SimpleResolver(Objects.requireNonNull(getByIpWithoutException("94.140.14.140"))), domain), _executor);
        CompletableFuture<List<InetAddress>> systemDnsFuture = CompletableFuture.supplyAsync(() -> {
            try {
                // Delay to give priority to custom resolvers over potentially poisoned system DNS
                Thread.sleep(SYSTEM_DNS_DELAY_MS);
                List<InetAddress> result = Dns.SYSTEM.lookup(domain);
                if (result.isEmpty()) throw new RuntimeException("System DNS returned empty for " + domain);
                return result;
            } catch (UnknownHostException e) {
                throw new RuntimeException(e);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new RuntimeException(e);
            }
        }, _executor);

        List<CompletableFuture<List<InetAddress>>> allFutures = Arrays.asList(
                cloudflareFuture, googleFuture, adguardFuture,
                cloudflareNormalFuture, googleNormalFuture, adguardNormalFuture,
                systemDnsFuture
        );
        String[] names = {"cloudflare", "google", "adguard", "cloudflare normal", "google normal", "adguard normal", "system"};

        return CompletableFuture.anyOf(allFutures.toArray(new CompletableFuture[0]))
                .thenApply(o -> {
                    for (int i = 0; i < allFutures.size(); i++) {
                        CompletableFuture<List<InetAddress>> f = allFutures.get(i);
                        if (f.isDone() && !f.isCompletedExceptionally()) {
                            List<InetAddress> result = f.join();
                            if (!result.isEmpty()) {
                                AneAwesomeUtilsLogging.d(TAG, "resolveWithDns() resolved with " + names[i]);
                                return result;
                            }
                        }
                    }
                    return new ArrayList<>();
                });
    }

    private List<InetAddress> resolveDns(Resolver resolver, String domain) {
        try {
            org.xbill.DNS.Record queryRecord = org.xbill.DNS.Record.newRecord(Name.fromString(domain + "."), Type.A, DClass.IN);
            Message queryMessage = Message.newQuery(queryRecord);
            Message result = resolver.send(queryMessage);
            List<org.xbill.DNS.Record> answers = result.getSection(Section.ANSWER);
            List<InetAddress> addresses = new ArrayList<>();
            for (Record record : answers) {
                if (record.getType() == Type.A || record.getType() == Type.AAAA) {
                    addresses.add(InetAddress.getByName(record.rdataToString()));
                }
            }
            return addresses;
        } catch (Exception e) {
            AneAwesomeUtilsLogging.d(TAG, "Failure in resolveDns() : " + e.getMessage(), e);
            throw new RuntimeException(e);
        }
    }
}
