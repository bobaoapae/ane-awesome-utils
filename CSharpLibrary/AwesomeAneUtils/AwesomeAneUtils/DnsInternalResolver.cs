using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using DnsClient;

namespace AwesomeAneUtils;

public sealed class DnsInternalResolver
{
    private static readonly IPAddress[] LOCALHOST_IPS = [IPAddress.Loopback, IPAddress.IPv6Loopback];

    private static DnsInternalResolver _instance;

    public static DnsInternalResolver Instance => _instance ??= new DnsInternalResolver();

    private readonly LookupClient _dnsClient;

    private readonly HttpClient _dohHttpClientCloudFlare;

    private readonly Dictionary<string, List<IPAddress>> _staticHosts;
    private readonly Dictionary<string, List<IPAddress>> _resolvedHosts;

    private DnsInternalResolver()
    {
        _staticHosts = [];
        _resolvedHosts = [];
        _dohHttpClientCloudFlare = new HttpClient
        {
            BaseAddress = new Uri("https://1.1.1.1/dns-query"),
            Timeout = TimeSpan.FromSeconds(2),
        };
        var lookupOptions = new LookupClientOptions(
            NameServer.Cloudflare,
            NameServer.Cloudflare2,
            NameServer.GooglePublicDns,
            NameServer.GooglePublicDns2)
        {
            UseCache = true,
            Timeout = TimeSpan.FromMilliseconds(250),
            Retries = 1,
            AutoResolveNameServers = true,
            CacheFailedResults = false,
            ContinueOnDnsError = true
        };
        _dnsClient = new LookupClient(lookupOptions);
    }

    public void AddStaticHost(string host, string ip)
    {
        if (!IPAddress.TryParse(ip, out var parsedIp))
        {
            return;
        }

        lock (_staticHosts)
        {
            if (!_staticHosts.ContainsKey(host))
                _staticHosts[host] = [];

            _staticHosts[host].Add(parsedIp);
        }
    }

    public void RemoveStaticHost(string host)
    {
        lock (_staticHosts)
        {
            _staticHosts.Remove(host);
        }
    }

    public async Task<IPAddress[]> ResolveHost(string host)
    {
        IPAddress[] ipAddresses = [];

        if (host == "localhost")
        {
            return LOCALHOST_IPS;
        }

        lock (_resolvedHosts)
        {
            if (_resolvedHosts.TryGetValue(host, out var resolvedHost) && resolvedHost.Count > 0)
            {
                ipAddresses = resolvedHost.ToArray();
            }
        }

        // Resolve the host to multiple IPs
        if (ipAddresses == null || ipAddresses.Length == 0)
        {
            var ips4 = await ResolveUsingDoH(host, "A");
            var ips6 = await ResolveUsingDoH(host, "AAAA");
            ipAddresses = ips4.Concat(ips6).ToArray();
            if (ipAddresses.Length > 0)
            {
                lock (_resolvedHosts)
                {
                    _resolvedHosts.Add(host, ipAddresses.ToList());
                }
            }
        }

        if (ipAddresses.Length == 0)
        {
            try
            {
                var queryResult = await _dnsClient.GetHostEntryAsync(host);
                ipAddresses = queryResult.AddressList;
                if (ipAddresses.Length > 0)
                {
                    lock (_resolvedHosts)
                    {
                        _resolvedHosts.Add(host, ipAddresses.ToList());
                    }
                }
            }
            catch (Exception)
            {
                // ignored
            }
        }

        if (ipAddresses.Length == 0)
        {
            lock (_staticHosts)
            {
                if (_staticHosts.TryGetValue(host, out var staticIp))
                {
                    ipAddresses = staticIp.ToArray();
                }
            }
        }

        if (ipAddresses == null || ipAddresses.Length == 0)
        {
            throw new Exception("No IP addresses resolved for the domain.");
        }

        if (!IsIpv6Available())
        {
            ipAddresses = ipAddresses.Where(x => x.AddressFamily != AddressFamily.InterNetworkV6).ToArray();
        }

        return SortInterleaved(ipAddresses);
    }

    private static IPAddress[] SortInterleaved(IPAddress[] addresses)
    {
        // Sort IPv6 and IPv4 addresses using a custom byte comparison.
        var ipv6 = addresses.Where(x => x.AddressFamily == AddressFamily.InterNetworkV6)
            .OrderBy(x => x, new IPAddressComparer())
            .ToArray();
        var ipv4 = addresses.Where(x => x.AddressFamily == AddressFamily.InterNetwork)
            .OrderBy(x => x, new IPAddressComparer())
            .ToArray();

        // Determine the common length between IPv6 and IPv4
        var commonLength = Math.Min(ipv6.Length, ipv4.Length);
        var result = new IPAddress[addresses.Length];

        // Interleave the IPv6 and IPv4 addresses
        for (var i = 0; i < commonLength; i++)
        {
            result[i * 2] = ipv6[i];
            result[i * 2 + 1] = ipv4[i];
        }

        // Add remaining addresses if there are any left (either IPv6 or IPv4)
        if (ipv6.Length > ipv4.Length)
        {
            ipv6.AsSpan(commonLength).CopyTo(result.AsSpan(commonLength * 2));
        }
        else if (ipv4.Length > ipv6.Length)
        {
            ipv4.AsSpan(commonLength).CopyTo(result.AsSpan(commonLength * 2));
        }

        return result;
    }

    private async Task<IPAddress[]> ResolveUsingDoH(string host, string type)
    {
        try
        {
            // Create the request URL with query parameters (similar to the curl request)
            var requestUrl = $"?name={host}&type={type}";

            // Create the request to the DoH server
            var request = new HttpRequestMessage(HttpMethod.Get, requestUrl);
            request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/dns-json"));

            // Send the request and get the response
            var response = await _dohHttpClientCloudFlare.SendAsync(request).ConfigureAwait(false);
            response.EnsureSuccessStatusCode();

            // Parse the JSON result using the source generator
            var jsonResponse = await response.Content.ReadAsStringAsync().ConfigureAwait(false);
            var dnsResponse = JsonSerializer.Deserialize(jsonResponse, DnsResponseContext.Default.DnsResponse);

            if (dnsResponse.Answers == null)
                return [];

            var ipAddresses = dnsResponse.Answers
                .Where(answer => answer.Type == 1 || answer.Type == 28) // 1 for A records, 28 for AAAA records
                .Select(answer =>
                {
                    // Try to parse the IP address, handle both IPv4 and IPv6
                    if (IPAddress.TryParse(answer.Data, out var parsedAddress))
                    {
                        return parsedAddress;
                    }

                    return null; // Return null for invalid IP addresses
                })
                .Where(ip => ip != null) // Filter out nulls (failed parses)
                .ToArray();

            return ipAddresses;
        }
        catch (Exception)
        {
            return [];
        }
    }

    public List<IPAddress> GetAllAvailableIPs()
    {
        var ipAddresses = new List<IPAddress>();

        // Get all network interfaces
        foreach (var networkInterface in NetworkInterface.GetAllNetworkInterfaces())
        {
            // Skip interfaces that are not up or do not support IP
            if (networkInterface.OperationalStatus != OperationalStatus.Up)
                continue;

            foreach (var unicastAddress in networkInterface.GetIPProperties().UnicastAddresses)
            {
                // Add all valid IP addresses to the list
                ipAddresses.Add(unicastAddress.Address);
            }
        }

        return ipAddresses;
    }

    public bool IsIpv6Available()
    {
        // Get all available IP addresses
        var ipAddresses = GetAllAvailableIPs();

        // Check if any is IPv6
        foreach (var ip in ipAddresses)
        {
            //if local continue
            if (IPAddress.IsLoopback(ip))
                continue;
            if (ip.AddressFamily == AddressFamily.InterNetworkV6)
                return true;
        }

        return false;
    }
}