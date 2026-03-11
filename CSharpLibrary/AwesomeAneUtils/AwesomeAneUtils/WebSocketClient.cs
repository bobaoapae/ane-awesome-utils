using System;
using System.Buffers;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.WebSockets;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;
using CommunityToolkit.HighPerformance.Buffers;

namespace AwesomeAneUtils;

public class WebSocketClient : IDisposable
{
    private CancellationTokenSource _cancellationTokenSource;

    // Callbacks
    private readonly Action<Dictionary<string, string>> _onConnect;
    private readonly Action _onReceived;
    private readonly Action<int, string, int, Dictionary<string, string>> _onIoError;
    private readonly Action<string> _onLog;

    private WebSocket _activeWebSocket;
    private IPEndPoint _remoteEndPoint;
    private Stream _stream;
    private Dictionary<string, string> _receivedHeaders = new();
    private int _responseCode;
    private int _disposed;
    private int _isDisconnectCalled;
    private readonly Lock _sendLocker;
    private Task _sendTask;
    private Task _receiveTask;
    private readonly Channel<byte[]> _sendChannel;
    private readonly ConcurrentQueue<IMemoryOwner<byte>> _receiveQueue;

    public IReadOnlyDictionary<string, string> ReceivedHeaders => _receivedHeaders;

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private bool IsDisposed() => Volatile.Read(ref _disposed) != 0;

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private bool IsDisconnectCalled() => Volatile.Read(ref _isDisconnectCalled) != 0;

    public WebSocketClient(Action<Dictionary<string, string>> onConnect, Action onReceived, Action<int, string, int, Dictionary<string, string>> onIoError, Action<string> onLog)
    {
        _onConnect = onConnect;
        _onReceived = onReceived;
        _onIoError = onIoError;
        _onLog = onLog;
        _sendChannel = Channel.CreateBounded<byte[]>(new BoundedChannelOptions(1000)
        {
            SingleReader = true,
            SingleWriter = true
        });
        _receiveQueue = new ConcurrentQueue<IMemoryOwner<byte>>();
        _sendLocker = new Lock();
    }

    public void Connect(string uri, Dictionary<string, string> headers = null)
    {
        _ = Task.Run(async () => await ConnectAsync(uri, headers));
    }

    private async Task ConnectAsync(string uri, Dictionary<string, string> headers)
    {
        _cancellationTokenSource = new CancellationTokenSource();

        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        using var linkedCts = CancellationTokenSource.CreateLinkedTokenSource(cts.Token, _cancellationTokenSource.Token);

        try
        {
            _onLog?.Invoke($"Connecting to {uri}...");
            var uriObject = new Uri(uri);

            (_activeWebSocket, _remoteEndPoint, _stream, _responseCode, _receivedHeaders) = await ManualWebSocketClient.ConnectAsync(uriObject, headers, _onLog, TimeSpan.FromSeconds(10), linkedCts.Token);

            // Check if the connection succeeded
            if (_activeWebSocket is { State: WebSocketState.Open })
            {
                _onConnect?.Invoke(_receivedHeaders); // Callback after successful connection

                // Start background tasks for sending and receiving messages
                _sendTask = Task.Factory.StartNew(SendLoopAsync, TaskCreationOptions.LongRunning).Unwrap();
                _receiveTask = Task.Factory.StartNew(ReceiveLoopAsync, TaskCreationOptions.LongRunning).Unwrap();
            }
            else
            {
                _onLog?.Invoke("All connection attempts failed.");
                await DisconnectAsync((int)WebSocketCloseStatus.EndpointUnavailable, "All connection attempts failed.");
            }
        }
        catch(WebSocketConnectException ex)
        {
            _responseCode = ex.ResponseCode;
            _receivedHeaders = ex.Headers;
            _onLog?.Invoke($"Error during connection: {ex.Message}");
            await DisconnectAsync((int)WebSocketCloseStatus.InternalServerError, $"Error during connection: {ex.Message}");
        }
        catch (Exception ex)
        {
            _onLog?.Invoke($"Error during connection: {ex.Message}");
            await DisconnectAsync((int)WebSocketCloseStatus.InternalServerError, $"Error during connection: {ex.Message}");
        }
    }

    public void Send(byte[] data)
    {
        using (_sendLocker.EnterScope())
        {
            if (_sendChannel.Writer.TryWrite(data))
                return;
            _onLog?.Invoke("Send queue is full. Dropping message.");
        }

        Disconnect((int)WebSocketCloseStatus.PolicyViolation);
    }

    private async Task SendLoopAsync()
    {
        var cancellationToken = _cancellationTokenSource.Token;
        while (!cancellationToken.IsCancellationRequested)
        {
            var data = await _sendChannel.Reader.ReadAsync(cancellationToken);

            try
            {
                await _activeWebSocket.SendAsync(data, WebSocketMessageType.Binary, true, cancellationToken);
                _onLog?.Invoke("Message sent.");
            }
            catch (Exception ex)
            {
                await DisconnectAsync((int)WebSocketCloseStatus.InternalServerError, $"Error during send: {ex.Message}");
            }
        }
    }

    private async Task ReceiveLoopAsync()
    {
        var cancellationToken = _cancellationTokenSource.Token;
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

                var memory = MemoryOwner<byte>.Allocate(totalBytesReceived);
                buffer.AsSpan(..totalBytesReceived).CopyTo(memory.Memory.Span);
                _receiveQueue.Enqueue(memory);
                _onReceived?.Invoke();
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
        Interlocked.Exchange(ref _isDisconnectCalled, 1);
        _ = Task.Run(async () => await DisconnectAsync(closeReason));
    }

    public bool TryGetNextMessage(out byte[] data)
    {
        data = [];
        if (IsDisconnectCalled())
            return false;

        if (!_receiveQueue.TryDequeue(out var memory))
            return false;

        data = memory.Memory.ToArray();
        memory.Dispose();
        return true;
    }

    private async Task DisconnectAsync(int closeReason, string reason = "Closing connection gracefully.")
    {
        if (!IsDisconnectCalled())
            _onIoError?.Invoke(closeReason, reason, _responseCode, _receivedHeaders);
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
        Dispose();
    }

    public void Stop()
    {
        _cancellationTokenSource?.Cancel();
    }

    public void StopAndWait(TimeSpan timeout)
    {
        _cancellationTokenSource?.Cancel();
        var tasks = new List<Task>(2);
        if (_sendTask != null) tasks.Add(_sendTask);
        if (_receiveTask != null) tasks.Add(_receiveTask);
        if (tasks.Count > 0)
        {
            try { Task.WaitAll(tasks.ToArray(), timeout); } catch { }
        }
    }

    public void Dispose()
    {
        Dispose(true);
        GC.SuppressFinalize(this);
    }

    private void Dispose(bool disposing)
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        if (disposing)
        {
            try
            {
                _cancellationTokenSource?.Cancel();
            }
            catch
            {
                // ignored
            }

            try
            {
                _cancellationTokenSource?.Dispose();
            }
            catch
            {
                // ignored
            }

            try
            {
                _activeWebSocket?.Dispose();
            }
            catch
            {
                // ignored
            }

            try { _stream?.Dispose(); } catch { }

            try
            {
                _sendChannel?.Writer.TryComplete();
            }
            catch
            {
                // ignored
            }

            while (_receiveQueue.TryDequeue(out var leftover))
            {
                try { leftover.Dispose(); } catch { }
            }
        }


        _onLog?.Invoke("WebSocket client disposed.");
    }

    ~WebSocketClient()
    {
        Dispose(false);
    }
}