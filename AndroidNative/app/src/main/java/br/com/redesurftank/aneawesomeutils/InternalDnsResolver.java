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
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

import okhttp3.Dns;

public class InternalDnsResolver {

    private static final InternalDnsResolver instance = new InternalDnsResolver();

    public static InternalDnsResolver getInstance() {
        return instance;
    }

    private final Map<String, List<String>> _staticHosts;
    private final Map<String, List<InetAddress>> _resolvedHosts;
    private final ExecutorService _executor;

    private InternalDnsResolver() {
        _staticHosts = new HashMap<>();
        _resolvedHosts = new HashMap<>();
        _executor= Executors.newCachedThreadPool();
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

        try {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.N) {
                addresses.addAll(resolveDnsUsingThreadForLowApi(host));
            } else {
                List<InetAddress> fromResolversResult = resolveWithDns(host).join();
                addresses.addAll(fromResolversResult);
            }
        } catch (Exception e) {
            AneAwesomeUtilsLogging.e(TAG, "Error in lookup() : " + e.getMessage(), e);
        }

        if (!addresses.isEmpty()) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                addresses.sort(new InetAddressComparator());
            }
            synchronized (_resolvedHosts) {
                _resolvedHosts.put(host, addresses);
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

        addresses.addAll(Dns.SYSTEM.lookup(host));

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
        AneAwesomeUtilsLogging.i(TAG, "resolveDnsUsingThreadForLowApi() called with: domain = [" + domain + "]");
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
                AneAwesomeUtilsLogging.e(TAG, "Failure in resolveDnsUsingThreadForLowApi() : " + e.getMessage(), e);
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
                AneAwesomeUtilsLogging.e(TAG, "Failure in resolveDnsUsingThreadForLowApi() : " + e.getMessage(), e);
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
                AneAwesomeUtilsLogging.e(TAG, "Failure in resolveDnsUsingThreadForLowApi() : " + e.getMessage(), e);
            }
        });
        Thread cloudflareNormalThread = new Thread(() -> {
            try {
                List<InetAddress> adguardAddresses = resolveDns(new SimpleResolver(Objects.requireNonNull(getByIpWithoutException("1.1.1.1"))), domain);
                synchronized (addresses) {
                    if (!addresses.isEmpty())
                        return;
                    AneAwesomeUtilsLogging.d(TAG, "resolveDnsUsingThreadForLowApi() resolved with cloudflare normal");
                    addresses.addAll(adguardAddresses);
                }
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Failure in resolveDnsUsingThreadForLowApi() : " + e.getMessage(), e);
            }
        });
        Thread googleNormalThread = new Thread(() -> {
            try {
                List<InetAddress> adguardAddresses = resolveDns(new SimpleResolver(Objects.requireNonNull(getByIpWithoutException("8.8.8.8"))), domain);
                synchronized (addresses) {
                    if (!addresses.isEmpty())
                        return;
                    AneAwesomeUtilsLogging.d(TAG, "resolveDnsUsingThreadForLowApi() resolved with google normal");
                    addresses.addAll(adguardAddresses);
                }
            } catch (Exception e) {
                AneAwesomeUtilsLogging.e(TAG, "Failure in resolveDnsUsingThreadForLowApi() : " + e.getMessage(), e);
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
                AneAwesomeUtilsLogging.e(TAG, "Failure in resolveDnsUsingThreadForLowApi() : " + e.getMessage(), e);
            }
        });

        cloudflareThread.start();
        googleThread.start();
        adguardThread.start();
        cloudflareNormalThread.start();
        googleNormalThread.start();
        adGuardNormalThread.start();

        while (true) {
            synchronized (addresses) {
                if (!addresses.isEmpty()) {
                    break;
                }
            }
            //check if all threads are done
            if (!cloudflareThread.isAlive() && !googleThread.isAlive() && !adguardThread.isAlive() && !cloudflareNormalThread.isAlive() && !googleNormalThread.isAlive() && !adGuardNormalThread.isAlive()) {
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
        AneAwesomeUtilsLogging.i(TAG, "resolveWithDns() called with: domain = [" + domain + "]");
        CompletableFuture<List<InetAddress>> cloudflareFuture = CompletableFuture.supplyAsync(() -> resolveDns(new DohResolver("https://1.1.1.1/dns-query"), domain), _executor);
        CompletableFuture<List<InetAddress>> googleFuture = CompletableFuture.supplyAsync(() -> resolveDns(new DohResolver("https://dns.google/dns-query"), domain), _executor);
        CompletableFuture<List<InetAddress>> adguardFuture = CompletableFuture.supplyAsync(() -> resolveDns(new DohResolver("https://unfiltered.adguard-dns.com/dns-query"), domain), _executor);
        CompletableFuture<List<InetAddress>> cloudflareNormalFuture = CompletableFuture.supplyAsync(() -> resolveDns(new SimpleResolver(Objects.requireNonNull(getByIpWithoutException("1.1.1.1"))), domain), _executor);
        CompletableFuture<List<InetAddress>> googleNormalFuture = CompletableFuture.supplyAsync(() -> resolveDns(new SimpleResolver(Objects.requireNonNull(getByIpWithoutException("8.8.8.8"))), domain), _executor);
        CompletableFuture<List<InetAddress>> adguardNormalFuture = CompletableFuture.supplyAsync(() -> resolveDns(new SimpleResolver(Objects.requireNonNull(getByIpWithoutException("94.140.14.140"))), domain), _executor);

        return CompletableFuture.anyOf(cloudflareFuture, googleFuture, adguardFuture, cloudflareNormalFuture, googleNormalFuture, adguardNormalFuture)
                .thenApply(o -> {
                    if (cloudflareFuture.isDone() && !cloudflareFuture.isCompletedExceptionally()) {
                        AneAwesomeUtilsLogging.i(TAG, "resolveWithDns() resolved with cloudflare");
                        return cloudflareFuture.join();
                    } else if (googleFuture.isDone() && !googleFuture.isCompletedExceptionally()) {
                        AneAwesomeUtilsLogging.i(TAG, "resolveWithDns() resolved with google");
                        return googleFuture.join();
                    } else if (adguardFuture.isDone() && !adguardFuture.isCompletedExceptionally()) {
                        AneAwesomeUtilsLogging.i(TAG, "resolveWithDns() resolved with adguard");
                        return adguardFuture.join();
                    } else if (cloudflareNormalFuture.isDone() && !cloudflareNormalFuture.isCompletedExceptionally()) {
                        AneAwesomeUtilsLogging.i(TAG, "resolveWithDns() resolved with cloudflare normal");
                        return cloudflareNormalFuture.join();
                    } else if (googleNormalFuture.isDone() && !googleNormalFuture.isCompletedExceptionally()) {
                        AneAwesomeUtilsLogging.i(TAG, "resolveWithDns() resolved with google normal");
                        return googleNormalFuture.join();
                    } else if (adguardNormalFuture.isDone() && !adguardNormalFuture.isCompletedExceptionally()) {
                        AneAwesomeUtilsLogging.i(TAG, "resolveWithDns() resolved with adguard normal");
                        return adguardNormalFuture.join();
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
