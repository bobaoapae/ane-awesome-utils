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

        throw new PlatformNotSupportedException();
    }

    public static string GetDeviceUniqueIdHash(Action<Exception> onError = null)
    {
        try
        {
            var deviceId = GetDeviceUniqueId();
            if (string.IsNullOrEmpty(deviceId))
                return "NOT_AVAILABLE";

            var hashBytes = SHA256.HashData(Encoding.UTF8.GetBytes(deviceId));
            return Convert.ToHexString(hashBytes).ToLowerInvariant();
        }
        catch (Exception e)
        {
            onError?.Invoke(e);
            return "NOT_AVAILABLE";
        }
    }

    private static string RunCommand(string command, string args)
    {
        try
        {
            using (var process = new System.Diagnostics.Process())
            {
                process.StartInfo.FileName = command;
                process.StartInfo.Arguments = args;
                process.StartInfo.RedirectStandardOutput = true;
                process.StartInfo.RedirectStandardError = true;
                process.StartInfo.UseShellExecute = false;
                process.StartInfo.CreateNoWindow = true;

                process.Start();
                string output = process.StandardOutput.ReadToEnd();
                process.WaitForExit();

                return output.Trim();
            }
        }
        catch
        {
            return string.Empty;
        }
    }

    private static string RunPowerShellCommand(string command)
    {
        try
        {
            using (var process = new System.Diagnostics.Process())
            {
                process.StartInfo.FileName = "powershell";
                process.StartInfo.Arguments = $"-Command \"{command}\"";
                process.StartInfo.RedirectStandardOutput = true;
                process.StartInfo.RedirectStandardError = true;
                process.StartInfo.UseShellExecute = false;
                process.StartInfo.CreateNoWindow = true;

                process.Start();
                string output = process.StandardOutput.ReadToEnd();
                process.WaitForExit();

                return output.Trim();
            }
        }
        catch
        {
            return string.Empty;
        }
    }

    private static bool IsWmicAvailable()
    {
        return !string.IsNullOrEmpty(RunCommand("where", "wmic"));
    }

    private static string GetSystemDiskSerial()
    {
        if (IsWmicAvailable())
        {
            var logicalToPartitionOutput = RunCommand("wmic", "path Win32_LogicalDiskToPartition get Antecedent,Dependent");
            var osDiskMapping = logicalToPartitionOutput
                .Split('\n')
                .Skip(1)
                .Select(line => line.Trim())
                .FirstOrDefault(line => line.Contains("C:"));

            if (string.IsNullOrEmpty(osDiskMapping))
                return string.Empty;

            var mappingParts = osDiskMapping.Split('"');
            if (mappingParts.Length < 2)
                return string.Empty;

            var partitionDeviceId = mappingParts[1];

            var diskToPartitionOutput = RunCommand("wmic", "path Win32_DiskDriveToDiskPartition get Antecedent,Dependent");
            var physicalDiskMapping = diskToPartitionOutput
                .Split('\n')
                .Skip(1)
                .Select(line => line.Trim())
                .FirstOrDefault(line => line.Contains(partitionDeviceId));

            if (string.IsNullOrEmpty(physicalDiskMapping))
                return string.Empty;

            var parts = physicalDiskMapping.Split(new[] { "  " }, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 2)
                return string.Empty;

            var diskDrivePart = parts[0];
            if (!diskDrivePart.Contains("PHYSICALDRIVE"))
                return string.Empty;

            var physicalDriveId = diskDrivePart.Split('"')[1];
            var diskIndexString = new string(physicalDriveId.Where(char.IsDigit).ToArray());
            if (!int.TryParse(diskIndexString, out var diskIndex))
                return string.Empty;

            var serialNumberOutput = RunCommand("wmic", $"diskdrive where Index={diskIndex} get SerialNumber");
            return serialNumberOutput
                .Split('\n')
                .Skip(1)
                .Select(line => line.Trim())
                .FirstOrDefault() ?? string.Empty;
        }
        else
        {
            var command = @"
            $osVolume = Get-Volume -DriveLetter C;
            $partition = Get-Partition | Where-Object { $_.DriveLetter -eq $osVolume.DriveLetter };
            $disk = Get-Disk -Number $partition.DiskNumber;
            $disk.SerialNumber;
        ";
            var output = RunPowerShellCommand(command);
            return output.Split('\n').FirstOrDefault()?.Trim() ?? string.Empty;
        }
    }

    private static string GetWindowsDeviceInfo()
    {
        string GetHardwareInfoWmic(string command, string args) =>
            RunCommand(command, args)
                .Split('\n')
                .Skip(1)
                .Select(line => line.Trim())
                .FirstOrDefault(line => !string.IsNullOrEmpty(line)) ?? string.Empty;

        string GetHardwareInfoPowerShell(string query)
        {
            var output = RunPowerShellCommand(query);
            return output.Split('\n').FirstOrDefault()?.Trim() ?? string.Empty;
        }

        var cpuId = IsWmicAvailable()
            ? GetHardwareInfoWmic("wmic", "cpu get processorid")
            : GetHardwareInfoPowerShell("(Get-WmiObject Win32_Processor).ProcessorId");

        var motherboardId = IsWmicAvailable()
            ? GetHardwareInfoWmic("wmic", "baseboard get serialnumber")
            : GetHardwareInfoPowerShell("(Get-WmiObject Win32_BaseBoard).SerialNumber");

        var biosId = IsWmicAvailable()
            ? GetHardwareInfoWmic("wmic", "bios get serialnumber")
            : GetHardwareInfoPowerShell("(Get-WmiObject Win32_BIOS).SerialNumber");

        var osDiskSerial = GetSystemDiskSerial();

        string[] memorySerials;
        string[] videoPnps;

        if (IsWmicAvailable())
        {
            var memOutput = RunCommand("wmic", "memorychip get serialnumber");
            memorySerials = memOutput.Split('\n').Skip(1).Select(l => l.Trim()).Where(s => !string.IsNullOrEmpty(s)).ToArray();

            var vidOutput = RunCommand("wmic", "path Win32_VideoController get PNPDeviceID");
            videoPnps = vidOutput.Split('\n').Skip(1).Select(l => l.Trim()).Where(s => !string.IsNullOrEmpty(s)).ToArray();
        }
        else
        {
            var memOut = RunPowerShellCommand("(Get-WmiObject Win32_PhysicalMemory | Select -Expand SerialNumber)");
            memorySerials = memOut.Split('\n').Select(l => l.Trim()).Where(s => !string.IsNullOrEmpty(s)).ToArray();

            var vidOut = RunPowerShellCommand("(Get-WmiObject Win32_VideoController | Select -Expand PNPDeviceID)");
            videoPnps = vidOut.Split('\n').Select(l => l.Trim()).Where(s => !string.IsNullOrEmpty(s)).ToArray();
        }

        var result = string.Join("-",
            new[] { cpuId, motherboardId, biosId, osDiskSerial }
            .Concat(memorySerials)
            .Concat(videoPnps)
            .Where(s => !string.IsNullOrEmpty(s)));

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
