using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace AwesomeAneUtils;

public static partial class HardwareID
{
    public static string GetDeviceUniqueId()
    {
        try
        {
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                return GetWindowsDeviceInfo();
            }

            if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
            {
                return GetLinuxDeviceInfo();
            }

            if (RuntimeInformation.IsOSPlatform(OSPlatform.OSX))
            {
                return GetMacOSDeviceInfo();
            }

            return string.Empty;
        }
        catch
        {
            return string.Empty;
        }
    }

    public static string GetDeviceUniqueIdHash()
    {
        try
        {
            var deviceId = GetDeviceUniqueId();
            if (string.IsNullOrEmpty(deviceId))
                return string.Empty;

            var hashBytes = SHA256.HashData(Encoding.UTF8.GetBytes(deviceId));
            return Convert.ToHexString(hashBytes).ToLowerInvariant();
        }
        catch
        {
            return string.Empty;
        }
    }

    private static string GetWindowsDeviceInfo()
    {
        string GetHardwareInfo(string command, string args) =>
            RunCommand(command, args).Split('\n').Skip(1).FirstOrDefault()?.Trim() ?? string.Empty;

        var cpuId = GetHardwareInfo("wmic", "cpu get processorid");
        var motherboardId = GetHardwareInfo("wmic", "baseboard get serialnumber");
        var diskId = GetHardwareInfo("wmic", "diskdrive get serialnumber");
    
        return $"{cpuId}-{motherboardId}-{diskId}";
    }

    private static string RunCommand(string command, string arguments)
    {
        using var process = new Process();
        process.StartInfo = new ProcessStartInfo
        {
            FileName = command,
            Arguments = arguments,
            RedirectStandardOutput = true,
            UseShellExecute = false,
            CreateNoWindow = true
        };

        process.Start();
        var result = process.StandardOutput.ReadToEnd();
        process.WaitForExit();
        return result;
    }

    private static string GetLinuxDeviceInfo()
    {
        string cpuId = File.ReadAllText("/proc/cpuinfo")
            .Split('\n')
            .FirstOrDefault(line => line.StartsWith("Serial", StringComparison.OrdinalIgnoreCase))
            ?.Split(':')
            .ElementAtOrDefault(1)?.Trim() ?? string.Empty;

        string motherboardId = File.Exists("/sys/devices/virtual/dmi/id/board_serial")
            ? File.ReadAllText("/sys/devices/virtual/dmi/id/board_serial").Trim()
            : string.Empty;

        return $"{cpuId}{motherboardId}";
    }

    private static string GetMacOSDeviceInfo()
    {
        var process = new Process
        {
            StartInfo = new ProcessStartInfo
            {
                FileName = "/usr/sbin/system_profiler",
                Arguments = "SPHardwareDataType",
                RedirectStandardOutput = true,
                UseShellExecute = false,
                CreateNoWindow = true
            }
        };

        process.Start();
        var output = process.StandardOutput.ReadToEnd();
        process.WaitForExit();

        var hardwareUUID = RegexHardwareUUIDMac().Match(output).Groups[1].Value.Trim();

        return hardwareUUID;
    }

    [GeneratedRegex(@"Hardware UUID: (.*)")]
    private static partial Regex RegexHardwareUUIDMac();
}