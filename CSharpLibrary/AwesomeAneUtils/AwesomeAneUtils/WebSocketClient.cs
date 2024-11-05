using System;
using System.Buffers;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using DnsClient;

namespace AwesomeAneUtils;

public class WebSocketClient : IDisposable
{
    private readonly ConcurrentQueue<byte[]> _sendQueue = new();
    private CancellationTokenSource _cancellationTokenSource;

    // Callbacks
    private readonly Action _onConnect;
    private readonly Action<ArraySegment<byte>> _onReceived;
    private readonly Action<int, string> _onIoError;
    private readonly Action<string> _onLog;

    private ClientWebSocket _activeWebSocket;
    private bool _disposed;
    private bool _isDisconnectCalled;

    public WebSocketClient(Action onConnect, Action<ArraySegment<byte>> onReceived, Action<int, string> onIoError, Action<string> onLog)
    {
        _onConnect = onConnect;
        _onReceived = onReceived;
        _onIoError = onIoError;
        _onLog = onLog;
    }
    
    public void Connect(string uri)
    {
        _ = Task.Run(async () => await ConnectAsync(uri));
    }

    private async Task ConnectAsync(string uri)
    {
        _cancellationTokenSource = new CancellationTokenSource();

        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        using var linkedCts = CancellationTokenSource.CreateLinkedTokenSource(cts.Token, _cancellationTokenSource.Token);

        try
        {
            _onLog?.Invoke($"Connecting to {uri}...");
            var uriObject = new Uri(uri);
            var host = uriObject.Host;

            var ipAddresses = await DnsInternalResolver.Instance.ResolveHost(host);

            _onLog?.Invoke($"Resolved {ipAddresses.Length} IP addresses for {host}.");

            // Try to connect to all resolved IPs in parallel
            var ctxSource = new CancellationTokenSource();

            var (webSocket, index) = await ParallelTask(
                ipAddresses.Length,
                (i, cancel) => AttemptConnectionAsync(uriObject, ipAddresses[i].ToString(), ctxSource.Token),
                TimeSpan.FromMilliseconds(250),
                linkedCts.Token);

            _activeWebSocket = webSocket;

            await ctxSource.CancelAsync(); // Cancel the other connection attempts

            // Check if the connection succeeded
            if (_activeWebSocket is { State: WebSocketState.Open })
            {
                _onConnect?.Invoke(); // Callback after successful connection

                _onLog?.Invoke($"Connection established to host {host} and ip {ipAddresses[index]}.");

                // Start background tasks for sending and receiving messages
                _ = Task.Factory.StartNew(() => SendLoopAsync(_cancellationTokenSource.Token), TaskCreationOptions.LongRunning);
                _ = Task.Factory.StartNew(() => ReceiveLoopAsync(_cancellationTokenSource.Token), TaskCreationOptions.LongRunning);
            }
            else
            {
                _onLog?.Invoke("All connection attempts failed.");
                await DisconnectAsync((int)WebSocketCloseStatus.EndpointUnavailable, "All connection attempts failed.");
            }
        }
        catch (Exception ex)
        {
            _onLog?.Invoke($"Error during connection: {ex.Message}");
            await DisconnectAsync((int)WebSocketCloseStatus.InternalServerError, $"Error during connection: {ex.Message}");
        }
    }

    private async Task<(ClientWebSocket, int)> ParallelTask(
        int candidateCount,
        Func<int, CancellationToken, Task<ClientWebSocket>> taskBuilder,
        TimeSpan delay,
        CancellationToken cancel)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(candidateCount);

        using var successCts = CancellationTokenSource.CreateLinkedTokenSource(cancel);

        // All tasks we have ever tried.
        var allTasks = new List<Task<ClientWebSocket>>();
        // Tasks we are still waiting on.
        var tasks = new List<Task<ClientWebSocket>>();

        // The general loop here is as follows:
        // 1. Add a new task for the next IP to try.
        // 2. Wait until any task completes OR the delay happens.
        // If an error occurs, we stop checking that task and continue checking the next.
        // Every iteration we add another task, until we're full on them.
        // We keep looping until we have SUCCESS, or we run out of attempt tasks entirely.

        Task<ClientWebSocket> successTask = null;
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
            Task<ClientWebSocket> completedTask;

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

        _ = Task.Run(async () =>
        {
            await Task.WhenAll(allTasks); // Wait for all connection attempts to finish
            foreach (var task in allTasks)
            {
                if (task == successTask || task.Result is not { State: WebSocketState.Open })
                    continue;

                try
                {
                    await task.Result.CloseAsync(WebSocketCloseStatus.EndpointUnavailable, "Connection established", CancellationToken.None);
                }
                catch (Exception ex)
                {
                    _onLog?.Invoke($"Error closing secondary connection: {ex.Message}");
                }
                finally
                {
                    task.Result.Dispose();
                }
            }
        });

        return (successTask.Result, allTasks.IndexOf(successTask));
    }

    private async Task<ClientWebSocket> AttemptConnectionAsync(Uri uri, string ipAddress, CancellationToken ctx)
    {
        try
        {
            var webSocket = new ClientWebSocket();

            // Set the 'Host' header to the domain from the original URI
            webSocket.Options.SetRequestHeader("Host", uri.Host);

            _onLog?.Invoke($"Attempting connection to {uri} via IP {ipAddress}");

            // Create the URI using the IP address but keep the correct path and scheme
            var webSocketUri = new UriBuilder(uri)
            {
                Host = ipAddress // Use the IP address for the connection
            }.Uri;

            try
            {
                await webSocket.ConnectAsync(webSocketUri, ctx);
                if (webSocket.State == WebSocketState.Open)
                {
                    _onLog?.Invoke($"Successfully connected to {ipAddress}.");
                    return webSocket;
                }
            }
            catch (Exception ex)
            {
                _onLog?.Invoke($"Failed to connect to {ipAddress}: {ex.Message}");
            }
        }
        catch (Exception ex)
        {
            _onLog?.Invoke($"Error connecting to {ipAddress}: {ex.Message}");
        }

        return null;
    }


    public void Send(byte[] data)
    {
        // Synchronous method to add data to the send queue
        _sendQueue.Enqueue(data);
    }

    private async Task SendLoopAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            if (_sendQueue.TryDequeue(out var data))
            {
                try
                {
                    await _activeWebSocket.SendAsync(new ArraySegment<byte>(data), WebSocketMessageType.Binary, true, cancellationToken);
                    _onLog?.Invoke("Message sent.");
                }
                catch (Exception ex)
                {
                    await DisconnectAsync((int)WebSocketCloseStatus.InternalServerError, $"Error during send: {ex.Message}");
                }
            }

            await Task.Delay(10, cancellationToken); // Avoid busy-waiting
        }
    }

    private async Task ReceiveLoopAsync(CancellationToken cancellationToken)
    {
        var bufferPool = ArrayPool<byte>.Shared; // ArrayPool for efficient buffer management
        var buffer = bufferPool.Rent(1024); // Rent a buffer from the pool
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                var totalBytesReceived = 0;
                WebSocketReceiveResult result;

                do
                {
                    result = await _activeWebSocket.ReceiveAsync(new ArraySegment<byte>(buffer, totalBytesReceived, buffer.Length - totalBytesReceived), cancellationToken);

                    if (result.MessageType == WebSocketMessageType.Close)
                    {
                        _onLog?.Invoke("Connection closed by the server.");
                        await DisconnectAsync((int)result.CloseStatus.GetValueOrDefault()); // Get the close code from the client
                        return;
                    }

                    totalBytesReceived += result.Count;

                    if (totalBytesReceived >= buffer.Length)
                    {
                        var newBuffer = bufferPool.Rent(buffer.Length * 2); // Double the buffer size if necessary
                        Array.Copy(buffer, newBuffer, buffer.Length);
                        bufferPool.Return(buffer); // Return the old buffer
                        buffer = newBuffer;
                    }
                } while (!result.EndOfMessage); // Keep receiving until the end of the message

                _onReceived?.Invoke(new ArraySegment<byte>(buffer, 0, totalBytesReceived));
                _onLog?.Invoke("Message received.");
            }
        }
        catch (Exception ex)
        {
            await DisconnectAsync((int)WebSocketCloseStatus.InternalServerError, $"Error during receive: {ex.Message}");
        }
        finally
        {
            bufferPool.Return(buffer); // Return the buffer to the pool
        }
    }

    public void Disconnect(int closeReason)
    {
        _isDisconnectCalled = true;
        _ = Task.Run(async () => await DisconnectAsync(closeReason));
    }

    private async Task DisconnectAsync(int closeReason, string reason = "Closing connection gracefully.")
    {
        if(!_isDisconnectCalled)
            _onIoError?.Invoke(closeReason, reason);
        if (_activeWebSocket is { State: WebSocketState.Open or WebSocketState.CloseReceived })
        {
            try
            {
                await _activeWebSocket.CloseAsync((WebSocketCloseStatus)closeReason, $"Closing with reason {closeReason}", CancellationToken.None);
                _onLog?.Invoke($"Connection closed gracefully. Reason: {closeReason}");
            }
            catch (Exception ex)
            {
                _onLog?.Invoke($"Error during close: {ex.Message}");
            }
        }

        _cancellationTokenSource?.Cancel();
        _onLog?.Invoke($"Connection closed. Reason: {closeReason}");
    }

    public void Stop()
    {
        _cancellationTokenSource?.Cancel();
    }

    public void Dispose()
    {
        Dispose(true);
        GC.SuppressFinalize(this);
    }

    private void Dispose(bool disposing)
    {
        if (!_disposed)
        {
            if (disposing)
            {
                // Dispose managed resources
                _cancellationTokenSource?.Cancel();
                _cancellationTokenSource?.Dispose();
                _activeWebSocket?.Dispose();
            }

            // Mark as disposed
            _disposed = true;
            _onLog?.Invoke("WebSocket client disposed.");
        }
    }

    ~WebSocketClient()
    {
        Dispose(false);
    }
}