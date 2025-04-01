using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using AwesomeAneUtils;

var allIps = DnsInternalResolver.Instance.GetAllAvailableIPs();
foreach (var ip in allIps)
{
    Console.WriteLine(ip);
}
Console.WriteLine(DnsInternalResolver.Instance.IsIpv6Available());
Console.WriteLine(HardwareID.GetDeviceUniqueIdHash(Console.WriteLine));
WebSocketClient webSocket = null;

webSocket = new WebSocketClient((headers)=>
{
    Console.WriteLine("Connected");
}, ()=>
{
    Console.WriteLine("received message");
    if(webSocket == null)
    {
        return;
    }
    if (!webSocket.TryGetNextMessage(out var data))
    {
        return;
    }
    
    Console.WriteLine(Encoding.UTF8.GetString(data));
    webSocket.Disconnect(1003);
}, (closeCode, reason, responseCode, headers)=>
{
    Console.WriteLine($"Closed: {closeCode} {reason}");
}, (exception)=>
{
    Console.WriteLine(exception);
});

webSocket.Connect("wss://35.199.72.111", new Dictionary<string, string>()
{
    {"authorization", "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJ0YXJnZXRfc2VydmVyIjoxMDE2LCJleHAiOjE3NDM0NDEyMTAsImlhdCI6MTc0MzQ0MDkxMH0.PJc0u305cs-PWbvnMY7DuU7NM19F5DINeFQf1apjHl8"}
});

Console.ReadLine();