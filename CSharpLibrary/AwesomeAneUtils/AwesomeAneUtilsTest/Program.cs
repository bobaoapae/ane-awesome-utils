using System.Collections.Generic;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using AwesomeAneUtils;
using Microsoft.VisualBasic.FileIO;
using System.Threading;

/*var allIps = DnsInternalResolver.Instance.GetAllAvailableIPs();
foreach (var ip in allIps)
{
    Console.WriteLine(ip);
}
Console.WriteLine(DnsInternalResolver.Instance.IsIpv6Available());*/
//read mtls from desktop mtls.pem
var mtlsPem = File.ReadAllText(SpecialDirectories.Desktop + "\\mtls.pem");
var configure = ClientCertificateProvider.TryConfigureHost("test-mtls.joaoiot.com.br", mtlsPem, string.Empty, out var error);
if (!configure)
{
    Console.WriteLine("Failed to configure client certificate: " + error);
}
else
{
    Console.WriteLine("Client certificate configured successfully.");
}
Console.WriteLine(HardwareID.GetDeviceUniqueIdHash(Console.WriteLine));
Console.WriteLine(VMDetector.DetectVM());
Console.WriteLine(VMDetector.IsRunningInVM());

var completion = new ManualResetEventSlim(false);

LoaderManager.Instance.Initialize(
    success: id =>
    {
        Console.WriteLine($"Load completed: {id}");
        if (Guid.TryParse(id, out var guid) && LoaderManager.Instance.TryGetResult(guid, out var bytes))
        {
            Console.WriteLine(Encoding.UTF8.GetString(bytes));
        }
        completion.Set();
    },
    error: (id, message) =>
    {
        Console.WriteLine($"Load failed {id}: {message}");
        completion.Set();
    },
    progress: (id, progress) => Console.WriteLine($"Progress {id}: {progress}"),
    writeLog: Console.WriteLine);

var loadId = LoaderManager.Instance.StartLoad(
    "https://test-mtls.joaoiot.com.br",
    "GET",
    new Dictionary<string, string>(),
    new Dictionary<string, string>());
Console.WriteLine($"Started load {loadId}");

if (!completion.Wait(TimeSpan.FromSeconds(30)))
{
    Console.WriteLine("Timed out waiting for loader result.");
}

return;
/*WebSocketClient webSocket = null;

webSocket = new WebSocketClient((headers) => { Console.WriteLine("Connected"); }, () =>
{
    Console.WriteLine("received message");
    if (webSocket == null)
    {
        return;
    }

    if (!webSocket.TryGetNextMessage(out var data))
    {
        return;
    }

    Console.WriteLine(Encoding.UTF8.GetString(data));
    webSocket.Disconnect(1003);
}, (closeCode, reason, responseCode, headers) => { Console.WriteLine($"Closed: {closeCode} {reason}"); }, (exception) => { Console.WriteLine(exception); });

webSocket.Connect("wss://35.199.72.111", new Dictionary<string, string>()
{
    { "authorization", "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJ0YXJnZXRfc2VydmVyIjoxMDE2LCJleHAiOjE3NDM0NDEyMTAsImlhdCI6MTc0MzQ0MDkxMH0.PJc0u305cs-PWbvnMY7DuU7NM19F5DINeFQf1apjHl8" }
});

Console.ReadLine();*/
