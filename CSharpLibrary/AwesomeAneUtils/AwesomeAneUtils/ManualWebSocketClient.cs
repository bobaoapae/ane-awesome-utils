using System.Collections.Generic;
using System.Linq;

namespace AwesomeAneUtils;

using System;
using System.Buffers;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Net.Security; // SslStream
using System.Net.WebSockets;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

public static class ManualWebSocketClient
{
    public static async Task<WebSocket> ConnectAsync(Uri uri, Action<string> onLog, CancellationToken cancellationToken)
    {
        // 1. Resolve DNS
        var addresses = await Dns.GetHostAddressesAsync(uri.Host, cancellationToken);
        if (addresses.Length == 0)
            throw new InvalidOperationException($"No IP addresses resolved for host {uri.Host}");

        // Determine port
        var port = uri.IsDefaultPort
            ? (uri.Scheme == "wss" ? 443 : 80)
            : uri.Port;

        // We'll use a linked CTS so we can cancel other attempts once the first socket is connected
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);

        // 2. Start all TCP connections in parallel, *without* doing the handshake yet
        var connectTasks = addresses.Select(addr => ConnectSocketAsync(addr, port, onLog, cts.Token)).ToList();

        // We'll store exceptions if all fail
        var exceptions = new List<Exception>();

        // 3. Wait for the first TCP connection to succeed
        while (connectTasks.Count > 0)
        {
            // Wait until any one task finishes
            var finishedTask = await Task.WhenAny(connectTasks);
            connectTasks.Remove(finishedTask);

            try
            {
                // This will rethrow if it failed
                var socket = await finishedTask;

                // If we got here, we have a connected socket. Cancel the others.
                cts.Cancel();

                // 4. Now do the handshake steps on the *first connected* socket
                Stream stream = new NetworkStream(socket, ownsSocket: true);
                if (uri.Scheme.Equals("wss", StringComparison.OrdinalIgnoreCase))
                {
                    var sslStream = new SslStream(stream, false, (sender, cert, chain, errors) => true);
                    await sslStream.AuthenticateAsClientAsync(uri.Host);
                    stream = sslStream;
                }

                // 5. Perform WebSocket handshake on the single “fastest” socket
                var websocket = await DoWebSocketHandshake(stream, uri, onLog, cancellationToken);
                if (websocket == null)
                {
                    // If handshake fails, we close and throw.
                    // NOTE: We do *not* attempt fallback to other sockets in this simple example
                    // but you could consider a more advanced approach.
                    stream.Dispose();
                    throw new InvalidOperationException("WebSocket handshake failed");
                }

                onLog?.Invoke($"Connection established to host {uri.Host}, IP {socket.RemoteEndPoint}");
                return websocket;
            }
            catch (Exception ex)
            {
                // Record the failure and continue waiting for others
                exceptions.Add(ex);
            }
        }

        // If we reach here, all connection attempts (TCP) have failed
        throw new AggregateException("Failed to connect to any resolved address", exceptions);
    }

    /// <summary>
    /// Attempts to connect a Socket to a given IP address/port.
    /// Returns the connected Socket on success, or throws on failure.
    /// </summary>
    private static async Task<Socket> ConnectSocketAsync(IPAddress addr, int port, Action<string> onLog, CancellationToken cancellationToken)
    {
        onLog?.Invoke($"Trying to connect to {addr} on port {port}...");
        var socket = new Socket(addr.AddressFamily, SocketType.Stream, ProtocolType.Tcp);

        try
        {
            // ConnectAsync with cancellation
            await using (cancellationToken.Register(() =>
                         {
                             onLog?.Invoke("Cancelling connection attempt to {addr} on port {port}...");
                             socket.Close(0);
                         }))
            {
                await socket.ConnectAsync(new IPEndPoint(addr, port), cancellationToken);
            }

            onLog?.Invoke($"TCP connection to {addr}:{port} succeeded.");
            return socket;
        }
        catch (Exception ex)
        {
            // Clean up the socket on failure
            socket.Dispose();
            onLog?.Invoke($"Failed to connect to {addr} on port {port}: {ex.Message}");
            throw;
        }
    }


    private static async Task<WebSocket> DoWebSocketHandshake(
        Stream stream,
        Uri uri,
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
            "Sec-WebSocket-Version: 13\r\n" +
            "\r\n";

        var requestBytes = Encoding.ASCII.GetBytes(requestHeaders);
        await stream.WriteAsync(requestBytes, token).ConfigureAwait(false);

        // 3. Ler cabeçalho do servidor até \r\n\r\n, sem passar do ponto.
        var headerBuffer = await ReadHeadersUntilCrLfCrLf(stream, token).ConfigureAwait(false);
        var headerText = Encoding.ASCII.GetString(headerBuffer);

        onLog?.Invoke($"Server handshake:\n{headerText}");

        // 4. Verifica 101 Switching Protocols
        if (!headerText.Contains("101 Switching Protocols"))
            return null;

        // 5. Verifica Sec-WebSocket-Accept
        var expectedAccept = ComputeWebSocketAccept(secKeyBase64);
        if (!headerText.Contains(expectedAccept))
            return null;

        // 6. Ao chegar aqui, NENHUM byte do WS foi lido.
        //    Podemos criar o WebSocket do stream original, sem CombinedStream.
        var ws = WebSocket.CreateFromStream(
            stream,
            isServer: false,
            subProtocol: null,
            keepAliveInterval: TimeSpan.FromSeconds(30));

        return ws;
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