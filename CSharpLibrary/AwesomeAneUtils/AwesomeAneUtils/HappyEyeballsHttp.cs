﻿using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Security;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;

namespace AwesomeAneUtils;

public static class HappyEyeballsHttp
{
    private const int ConnectionAttemptDelay = 250;
    private static readonly string SystemArch = Environment.Is64BitProcess ? "64" : "32";

    public static readonly string CustomAgent = $"Mozilla/5.0 (Windows NT 6.1{SystemArch}; Win; x{SystemArch};) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/99.0.4844.51 Safari/537.36 NativeLoader/1.0";
    private static Action<string> _log = Console.WriteLine;

#if DEBUG

    private const int SlowIpv6 = 0;
    private const bool BrokenIpv6 = false;

#endif

    // .NET does not implement Happy Eyeballs at the time of writing.
    // https://github.com/space-wizards/SS14.Launcher/issues/38
    // This is the workaround.
    //
    // What's Happy Eyeballs? It makes the launcher try both IPv6 and IPv4,
    // the former with priority, so that if IPv6 is broken your launcher still works.
    //
    // Implementation originally based on,
    // rewritten as to be nigh-impossible to recognize https://github.com/ppy/osu-framework/pull/4191/files
    //
    // This is a simple implementation. It does not fully implement RFC 8305:
    // * We do not separately handle parallel A and AAAA DNS requests as optimization.
    // * We don't sort IPs as specified in RFC 6724. I can't tell if GetHostEntryAsync does.
    // * Look I wanted to keep this simple OK?
    //   We don't do any fancy shit like statefulness or incremental sorting
    //   or incremental DNS updates who cares about that.
    public static HttpClient CreateHttpClient(bool autoRedirect = true, Action<string> logAction = null)
    {
        if (logAction != null)
        {
            _log = logAction;
        }

        var sslOptions = new SslClientAuthenticationOptions
        {
            // Leave certs unvalidated for debugging
            RemoteCertificateValidationCallback = delegate { return true; },
        };

        var handler = new SocketsHttpHandler
        {
            ConnectCallback = OnConnect,
            AutomaticDecompression = DecompressionMethods.All,
            AllowAutoRedirect = autoRedirect,
            EnableMultipleHttp2Connections = true,
            SslOptions = sslOptions,
            ConnectTimeout = TimeSpan.FromSeconds(5)
        };

        var client = new HttpClient(handler)
        {
            DefaultRequestVersion = HttpVersion.Version20,
            DefaultVersionPolicy = HttpVersionPolicy.RequestVersionOrLower
        };

        client.DefaultRequestHeaders.UserAgent.TryParseAdd(CustomAgent);

        return client;
    }

    private static async ValueTask<Stream> OnConnect(
        SocketsHttpConnectionContext context,
        CancellationToken cancellationToken)
    {
        // Get IPs via DNS.
        // Note that we do not attempt to exclude IPv6 if the user doesn't have IPv6.
        // According to the docs, GetHostEntryAsync will not return them if there's no address.
        // BUT! I tested and that's a lie at least on Linux.
        // Regardless, if you don't have IPv6,
        // an attempt to connect to an IPv6 socket *should* immediately give a "network unreachable" socket error.
        // This will cause the code to immediately try the next address,
        // so IPv6 just gets "skipped over" if you don't have it.
        // I could find no other robust way to check "is there a chance in hell IPv6 works" other than "try it",
        // so... try it we will.
        var endPoint = context.DnsEndPoint;

        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        using var linkedCts = CancellationTokenSource.CreateLinkedTokenSource(cts.Token, cancellationToken);

        var resolvedAddresses = await GetIpsForHost(endPoint, linkedCts.Token).ConfigureAwait(false);
        if (resolvedAddresses.Length == 0)
            throw new Exception($"Host {context.DnsEndPoint.Host} resolved to no IPs!");

        linkedCts.Token.ThrowIfCancellationRequested();

        var (socket, index) = await ParallelTask(
            resolvedAddresses.Length,
            (i, cancel) => AttemptConnection(i, resolvedAddresses[i], endPoint.Port, cancel),
            TimeSpan.FromMilliseconds(ConnectionAttemptDelay),
            linkedCts.Token);


        return new NetworkStream(socket, ownsSocket: true);
    }

    private static async Task<Socket> AttemptConnection(
        int index,
        IPAddress address,
        int port,
        CancellationToken cancel)
    {
        // The following socket constructor will create a dual-mode socket on systems where IPV6 is available.
        var socket = new Socket(SocketType.Stream, ProtocolType.Tcp) { NoDelay = true };
        try
        {
#if DEBUG
            if (address.AddressFamily == AddressFamily.InterNetworkV6)
            {
                await Task.Delay(SlowIpv6, cancel).ConfigureAwait(false);

                if (BrokenIpv6)
                    throw new Exception("Oh no I can't reach the network this is SO SAD.");
            }
#endif

            await socket.ConnectAsync(new IPEndPoint(address, port), cancel).ConfigureAwait(false);
            return socket;
        }
        catch (Exception)
        {
            socket.Dispose();
            return null;
        }
    }

    private static Task<IPAddress[]> GetIpsForHost(DnsEndPoint endPoint, CancellationToken cancel)
    {
        if (IPAddress.TryParse(endPoint.Host, out var ip))
            return Task.FromResult(new[] { ip });

        return DnsInternalResolver.Instance.ResolveHost(endPoint.Host);
    }

    internal static async Task<(T, int)> ParallelTask<T>(
        int candidateCount,
        Func<int, CancellationToken, Task<T>> taskBuilder,
        TimeSpan delay,
        CancellationToken cancel) where T : IDisposable
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(candidateCount);

        using var successCts = CancellationTokenSource.CreateLinkedTokenSource(cancel);

        // All tasks we have ever tried.
        var allTasks = new List<Task<T>>();
        // Tasks we are still waiting on.
        var tasks = new List<Task<T>>();

        // The general loop here is as follows:
        // 1. Add a new task for the next IP to try.
        // 2. Wait until any task completes OR the delay happens.
        // If an error occurs, we stop checking that task and continue checking the next.
        // Every iteration we add another task, until we're full on them.
        // We keep looping until we have SUCCESS, or we run out of attempt tasks entirely.

        Task<T> successTask = null;
        while ((allTasks.Count < candidateCount || tasks.Count > 0))
        {
            if (allTasks.Count < candidateCount)
            {
                // We have to queue another task this iteration.
                var newTask = taskBuilder(allTasks.Count, successCts.Token);
                tasks.Add(newTask);
                allTasks.Add(newTask);
            }

            var whenAnyDone = Task.WhenAny(tasks);
            Task<T> completedTask;

            if (allTasks.Count < candidateCount)
            {
                // If we have another one to queue, wait for a timeout instead of *just* waiting for a connection task.
                var timeoutTask = Task.Delay(delay, successCts.Token);
                var whenAnyOrTimeout = await Task.WhenAny(whenAnyDone, timeoutTask).ConfigureAwait(false);
                if (whenAnyOrTimeout != whenAnyDone)
                {
                    // Timeout finished. Go to next iteration so we queue another one.
                    continue;
                }

                completedTask = whenAnyDone.Result;
            }
            else
            {
                completedTask = await whenAnyDone.ConfigureAwait(false);
            }

            if (completedTask.IsCompletedSuccessfully && completedTask.Result != null)
            {
                // We did it. We have success.
                successTask = completedTask;
                break;
            }
            else
            {
                // Faulted. Remove it.
                tasks.Remove(completedTask);
            }
        }

        Debug.Assert(allTasks.Count > 0);

        cancel.ThrowIfCancellationRequested();
        await successCts.CancelAsync().ConfigureAwait(false);

        if (successTask == null)
        {
            // We didn't get a single successful connection. Well heck.
            throw new AggregateException(
                allTasks.Where(x => x.IsFaulted).SelectMany(x => x.Exception!.InnerExceptions));
        }

        // I don't know if this is possible but MAKE SURE that we don't get two sockets completing at once.
        // Just a safety measure.
        foreach (var task in allTasks)
        {
            if (task.IsCompletedSuccessfully && task != successTask)
                task.Result?.Dispose();
        }

        return (successTask.Result, allTasks.IndexOf(successTask));
    }
}