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

webSocket = new WebSocketClient(()=>
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
}, (closeCode, reason)=>
{
    Console.WriteLine($"Closed: {closeCode} {reason}");
}, (exception)=>
{
    Console.WriteLine(exception);
});

webSocket.Connect("wss://neo.cabal-argo-tunnel.com/38141");

Console.ReadLine();