using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace AwesomeAneUtils;

// SslStream

public static class ManualWebSocketClient
{
    public static async Task<(WebSocket, IPEndPoint, Stream, int, Dictionary<string, string>)> ConnectAsync(
        Uri uri,
        Dictionary<string, string> headers,
        Action<string> onLog,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var addresses = await DnsInternalResolver.Instance.ResolveHost(uri.Host);
        if (addresses.Length == 0)
            throw new InvalidOperationException($"No IP addresses resolved for host {uri.Host}");

        var port = uri.IsDefaultPort ? (uri.Scheme == "wss" ? 443 : 80) : uri.Port;
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        var maxTimeIndividually = TimeSpan.FromMilliseconds(timeout.TotalMilliseconds / addresses.Length);

        var connectTasks = addresses.Select(addr =>
            Task.Factory.StartNew(
                () => ConnectSocketAsync(addr, port, onLog, maxTimeIndividually, cts.Token),
                cts.Token,
                TaskCreationOptions.DenyChildAttach,
                TaskScheduler.Default).Unwrap()
        ).ToList();

        var exceptions = new List<Exception>();
        var latestResponseCode = 0;
        Dictionary<string, string> latestHeaders = new();

        while (connectTasks.Count > 0)
        {
            var finishedTask = await Task.WhenAny(connectTasks);
            connectTasks.Remove(finishedTask);

            try
            {
                var socket = await finishedTask;
                cts.Cancel();

                onLog?.Invoke($"TCP connection to {socket.RemoteEndPoint} succeeded.");

                Stream stream = new NetworkStream(socket, ownsSocket: true);
                if (uri.Scheme.Equals("wss", StringComparison.OrdinalIgnoreCase))
                {
                    var sslStream = new SslStream(stream, false, (sender, cert, chain, errors) => true);
                    await sslStream.AuthenticateAsClientAsync(uri.Host);
                    stream = sslStream;
                }

                var (websocket, responseCode, receivedHeaders) = await DoWebSocketHandshake(stream, uri, headers, onLog, cancellationToken);

                latestResponseCode = responseCode;
                latestHeaders = receivedHeaders;

                if (websocket == null)
                {
                    stream.Dispose();
                    throw new InvalidOperationException("WebSocket handshake failed");
                }

                onLog?.Invoke($"Connection established to host {uri.Host}, IP {socket.RemoteEndPoint}");
                return (websocket, socket.RemoteEndPoint as IPEndPoint, stream, responseCode, receivedHeaders);
            }
            catch (Exception ex)
            {
                exceptions.Add(ex);
            }
        }

        // If all fail, still return latest responseCode and headers (even if 0/empty)
        throw new WebSocketConnectException("Failed to connect to any resolved address", exceptions, latestResponseCode, latestHeaders);
    }


    /// <summary>
    /// Attempts to connect a Socket to a given IP address/port.
    /// Returns the connected Socket on success, or throws on failure.
    /// </summary>
    private static async Task<Socket> ConnectSocketAsync(IPAddress addr, int port, Action<string> onLog, TimeSpan timeout, CancellationToken cancellationToken)
    {
        using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeoutCts.CancelAfter(timeout);
        onLog?.Invoke($"Trying to connect to {addr} on port {port}...");

        var socket = new Socket(addr.AddressFamily, SocketType.Stream, ProtocolType.Tcp);
        socket.NoDelay = true;

        // Keep-alive settings for long-lived WebSocket connections
        socket.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.KeepAlive, true);

        // On Windows, you can set more specific keep-alive parameters
        // This requires platform-specific code or a check for the operating system
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
        {
            // Set TCP keep-alive time (milliseconds)
            byte[] keepAliveValues = new byte[12];
            BitConverter.GetBytes(1).CopyTo(keepAliveValues, 0); // Enable keep-alive
            BitConverter.GetBytes(3000).CopyTo(keepAliveValues, 4); // Idle time before first probe (30 seconds)
            BitConverter.GetBytes(1000).CopyTo(keepAliveValues, 8); // Interval between probes (5 seconds)
            socket.IOControl(IOControlCode.KeepAliveValues, keepAliveValues, null);
        }

        try
        {
            // ConnectAsync with cancellation
            await using (cancellationToken.Register(() =>
                         {
                             onLog?.Invoke("Cancelling connection attempt to {addr} on port {port}...");
                             socket.Close(0);
                         }))
            {
                await socket.ConnectAsync(new IPEndPoint(addr, port), timeoutCts.Token);
            }

            return socket;
        }
        catch (Exception ex)
        {
            // Clean up the socket on failure
            socket.Dispose();
            if (ex is not OperationCanceledException)
                onLog?.Invoke($"Failed to connect to {addr} on port {port}: {ex.Message}");
            throw;
        }
    }


    private static async Task<(WebSocket, int, Dictionary<string, string>)> DoWebSocketHandshake(
        Stream stream,
        Uri uri,
        Dictionary<string, string> headers,
        Action<string> onLog,
        CancellationToken token)
    {
        // 1. Gera chave aleatória para o Sec-WebSocket-Key
        var secKey = new byte[16];
        RandomNumberGenerator.Fill(secKey);
        var secKeyBase64 = Convert.ToBase64String(secKey);

        // 2. Monta a requisição HTTP
        var requestHeaders =
            $"GET {uri.PathAndQuery} HTTP/1.1\r\n" +
            $"Host: {uri.Host}\r\n" +
            $"User-Agent: {HappyEyeballsHttp.CustomAgent}\r\n" +
            "Upgrade: websocket\r\n" +
            "Connection: Upgrade\r\n" +
            $"Sec-WebSocket-Key: {secKeyBase64}\r\n" +
            "Sec-WebSocket-Version: 13\r\n";

        if (headers != null)
        {
            foreach (var (key, value) in headers)
            {
                if (string.IsNullOrEmpty(key) || string.IsNullOrEmpty(value))
                    continue;
                requestHeaders += $"{key}: {value}\r\n";
            }
        }

        requestHeaders += "\r\n";

        var requestBytes = Encoding.ASCII.GetBytes(requestHeaders);
        await stream.WriteAsync(requestBytes, token).ConfigureAwait(false);

        // 3. Ler cabeçalho do servidor até \r\n\r\n, sem passar do ponto.
        var headerBuffer = await ReadHeadersUntilCrLfCrLf(stream, token).ConfigureAwait(false);
        var headerText = Encoding.ASCII.GetString(headerBuffer);

        var headersDict = new Dictionary<string, string>();
        var responseCode = 0;

        // Split the header text by lines and process each line
        var headerLines = headerText.Split(new[] { "\r\n" }, StringSplitOptions.RemoveEmptyEntries);

        foreach (var line in headerLines)
        {
            // Skip the first line which is typically the HTTP status line
            if (line.StartsWith("HTTP/"))
                continue;

            // Find the position of the first colon
            var colonPos = line.IndexOf(':');
            if (colonPos > 0)
            {
                // Extract header name and value
                var headerName = line[..colonPos].Trim();
                var headerValue = line[(colonPos + 1)..].Trim();

                // Add to dictionary (case-insensitive option)
                headersDict[headerName] = headerValue;
            }
        }

#if DEBUG
        onLog?.Invoke($"Server handshake:\n{headerText}");
#endif
        
        // 4. Verifica o código de resposta
        if (headerLines.Length > 0)
        {
            var statusLine = headerLines[0];
            var statusParts = statusLine.Split(' ');
            if (statusParts.Length > 1 && int.TryParse(statusParts[1], out responseCode))
            {
                // Sucesso
                if (responseCode != 101 || !statusLine.Contains("Switching Protocols"))
                    return (null, responseCode, headersDict);
            }
        }

        // 5. Verifica Sec-WebSocket-Accept
        var expectedAccept = ComputeWebSocketAccept(secKeyBase64);
        if (!headerText.Contains(expectedAccept))
            return (null, responseCode, headersDict);

        // 6. Ao chegar aqui, NENHUM byte do WS foi lido.
        //    Podemos criar o WebSocket do stream original, sem CombinedStream.
        var ws = WebSocket.CreateFromStream(
            stream,
            isServer: false,
            subProtocol: null,
            keepAliveInterval: TimeSpan.FromSeconds(30));

        return (ws, responseCode, headersDict);
    }

    private static async Task<byte[]> ReadHeadersUntilCrLfCrLf(Stream stream, CancellationToken token)
    {
        using var ms = new MemoryStream();
        var buffer = new byte[1];

        // Precisamos detectar \r\n\r\n. Guardaremos os últimos 4 bytes lidos para comparação.
        var endMarker = new[] { (byte)'\r', (byte)'\n', (byte)'\r', (byte)'\n' };

        while (true)
        {
            int bytesRead = await stream.ReadAsync(buffer, 0, 1, token).ConfigureAwait(false);
            if (bytesRead == 0)
                throw new IOException("Connection closed before end of headers");

            ms.WriteByte(buffer[0]);

            // Se o número de bytes no ms >= 4, podemos verificar se os 4 últimos bytes são \r\n\r\n
            if (ms.Length < 4)
                continue;
            var data = ms.GetBuffer(); // pega o array interno
            var len = (int)ms.Length;

            if (data[len - 4] == endMarker[0] &&
                data[len - 3] == endMarker[1] &&
                data[len - 2] == endMarker[2] &&
                data[len - 1] == endMarker[3])
            {
                // Encontramos \r\n\r\n. Parar a leitura aqui mesmo.
                break;
            }
        }

        return ms.ToArray();
    }

    static string ComputeWebSocketAccept(string secKeyBase64)
    {
        // 258EAFA5-E914-47DA-95CA-C5AB0DC85B11 é a "magic string"
        var sha1 = SHA1.HashData(Encoding.ASCII.GetBytes(secKeyBase64 + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
        return Convert.ToBase64String(sha1);
    }
}