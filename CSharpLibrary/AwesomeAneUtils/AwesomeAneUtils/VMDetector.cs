using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace AwesomeAneUtils;

public class VMDetector
{
    [DllImport("kernel32.dll")]
    private static extern uint GetSystemFirmwareTable(uint FirmwareTableProviderSignature, uint FirmwareTableID, IntPtr pFirmwareTableBuffer, uint BufferSize);

    [StructLayout(LayoutKind.Sequential)]
    private struct RawSMBIOSData
    {
        public byte Used20CallingMethod;
        public byte SMBIOSMajorVersion;
        public byte SMBIOSMinorVersion;
        public byte DmiRevision;
        public uint Length;
    }

    public enum VMType
    {
        None,
        VMware,
        VirtualBox,
        HyperV,
        QEMU,
        KVM,
        Xen,
        Parallels,
        Other
    }

    public static bool IsRunningInVM()
    {
        return DetectVM() != VMType.None;
    }

    public static VMType DetectVM()
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
        {
            return DetectVMWindows();
        }
        else if (RuntimeInformation.IsOSPlatform(OSPlatform.OSX))
        {
            return DetectVMMacOS();
        }
        else if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
        {
            return DetectVMLinux();
        }

        return VMType.None;
    }

    [SupportedOSPlatform("windows")]
    private static VMType DetectVMWindows()
    {
        VMType vmType = CheckWindowsRegistry();
        if (vmType != VMType.None)
            return vmType;

        vmType = CheckWindowsFirmwareTable();
        if (vmType != VMType.None)
            return vmType;

        vmType = CheckWindowsWMIC();
        if (vmType != VMType.None)
            return vmType;

        vmType = CheckWindowsPowerShell();
        if (vmType != VMType.None)
            return vmType;

        return VMType.None;
    }

    [SupportedOSPlatform("windows")]
    private static VMType CheckWindowsRegistry()
    {
        try
        {
            using (var key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\VMware, Inc.\VMware Tools"))
            {
                if (key != null)
                    return VMType.VMware;
            }

            using (var key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Oracle\VirtualBox Guest Additions"))
            {
                if (key != null)
                    return VMType.VirtualBox;
            }

            using (var key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Virtual Machine\Guest\Parameters"))
            {
                if (key != null)
                    return VMType.HyperV;
            }

            using (var key = Registry.LocalMachine.OpenSubKey(@"HARDWARE\DESCRIPTION\System\BIOS"))
            {
                if (key != null)
                {
                    string systemManufacturer = key.GetValue("SystemManufacturer")?.ToString() ?? "";
                    string systemProductName = key.GetValue("SystemProductName")?.ToString() ?? "";

                    if (systemManufacturer.Contains("VMware") || systemProductName.Contains("VMware"))
                        return VMType.VMware;
                    if (systemManufacturer.Contains("innotek") || systemProductName.Contains("VirtualBox"))
                        return VMType.VirtualBox;
                    if (systemManufacturer.Contains("Microsoft") && systemProductName.Contains("Virtual"))
                        return VMType.HyperV;
                    if (systemManufacturer.Contains("Xen") || systemProductName.Contains("Xen"))
                        return VMType.Xen;
                    if (systemManufacturer.Contains("QEMU") || systemProductName.Contains("QEMU"))
                        return VMType.QEMU;
                    if (systemManufacturer.Contains("Parallels") || systemProductName.Contains("Parallels"))
                        return VMType.Parallels;
                }
            }

            using (var key = Registry.LocalMachine.OpenSubKey(@"HARDWARE\DEVICEMAP\Scsi\Scsi Port 0\Scsi Bus 0\Target Id 0\Logical Unit Id 0"))
            {
                if (key != null)
                {
                    string identifier = key.GetValue("Identifier")?.ToString() ?? "";

                    if (identifier.Contains("VMware"))
                        return VMType.VMware;
                    if (identifier.Contains("VBOX"))
                        return VMType.VirtualBox;
                }
            }
        }
        catch
        {
        }

        return VMType.None;
    }

    private static VMType CheckWindowsFirmwareTable()
    {
        try
        {
            const uint RSMB = 0x52534D42; // 'RSMB' signature for SMBIOS table

            // First get the size needed for the buffer
            uint size = GetSystemFirmwareTable(RSMB, 0, IntPtr.Zero, 0);
            if (size == 0)
                return VMType.None;

            // Allocate buffer and get the data
            IntPtr pData = Marshal.AllocHGlobal((int)size);
            try
            {
                GetSystemFirmwareTable(RSMB, 0, pData, size);

                // Parse SMBIOS header
                RawSMBIOSData header = Marshal.PtrToStructure<RawSMBIOSData>(pData);

                // Get raw data (after header)
                byte[] data = new byte[header.Length];
                IntPtr dataPtr = IntPtr.Add(pData, Marshal.SizeOf<RawSMBIOSData>());
                Marshal.Copy(dataPtr, data, 0, (int)header.Length);

                // Process SMBIOS data: manufacturer, product, version, etc.
                string biosVendor = ExtractSMBIOSString(data, FindSMBIOSStructure(data, 0, 0x04)); // Type 0 = BIOS, Field 0x04 = Vendor
                string systemManufacturer = ExtractSMBIOSString(data, FindSMBIOSStructure(data, 1, 0x04)); // Type 1 = System, Field 0x04 = Manufacturer
                string systemProductName = ExtractSMBIOSString(data, FindSMBIOSStructure(data, 1, 0x05)); // Type 1 = System, Field 0x05 = Product Name

                if (biosVendor.Contains("VMware") || systemManufacturer.Contains("VMware") || systemProductName.Contains("VMware"))
                    return VMType.VMware;
                if (biosVendor.Contains("VBOX") || biosVendor.Contains("VirtualBox") ||
                    systemManufacturer.Contains("innotek") || systemManufacturer.Contains("VirtualBox") ||
                    systemProductName.Contains("VirtualBox"))
                    return VMType.VirtualBox;
                if (biosVendor.Contains("Microsoft") ||
                    (systemManufacturer.Contains("Microsoft") && systemProductName.Contains("Virtual")))
                    return VMType.HyperV;
                if (biosVendor.Contains("Xen") || systemManufacturer.Contains("Xen") || systemProductName.Contains("Xen"))
                    return VMType.Xen;
                if (biosVendor.Contains("QEMU") || systemManufacturer.Contains("QEMU") || systemProductName.Contains("QEMU"))
                    return VMType.QEMU;
                if (biosVendor.Contains("Parallels") || systemManufacturer.Contains("Parallels") || systemProductName.Contains("Parallels"))
                    return VMType.Parallels;

                if (biosVendor.Contains("Virtual") || systemManufacturer.Contains("Virtual") || systemProductName.Contains("Virtual"))
                    return VMType.Other;
            }
            finally
            {
                Marshal.FreeHGlobal(pData);
            }
        }
        catch
        {
        }

        return VMType.None;
    }

    private static int FindSMBIOSStructure(byte[] data, byte type, byte fieldOffset)
    {
        int offset = 0;
        while (offset < data.Length)
        {
            byte currentType = data[offset];
            byte length = data[offset + 1];

            if (currentType == type && offset + fieldOffset < data.Length)
            {
                return offset + fieldOffset;
            }

            // Find the end of the formatted structure
            offset += length;

            // Find the end of the unformatted structure (ends with double null)
            while (offset < data.Length - 1 && !(data[offset] == 0 && data[offset + 1] == 0))
            {
                offset++;
            }

            // Skip the double null
            offset += 2;
        }

        return -1;
    }

    private static string ExtractSMBIOSString(byte[] data, int offset)
    {
        if (offset == -1 || offset >= data.Length)
            return string.Empty;

        byte stringIndex = data[offset];
        if (stringIndex == 0)
            return string.Empty;

        // Find the string table after the formatted structure
        int stringTableOffset = offset;
        while (stringTableOffset < data.Length && data[stringTableOffset] != 0)
        {
            stringTableOffset++;
        }

        stringTableOffset++; // Skip the ending null

        // Find the indexed string
        for (int i = 1; i < stringIndex; i++)
        {
            while (stringTableOffset < data.Length && data[stringTableOffset] != 0)
            {
                stringTableOffset++;
            }

            stringTableOffset++; // Skip the ending null
        }

        // Extract the string
        StringBuilder sb = new StringBuilder();
        while (stringTableOffset < data.Length && data[stringTableOffset] != 0)
        {
            sb.Append((char)data[stringTableOffset]);
            stringTableOffset++;
        }

        return sb.ToString();
    }

    private static VMType CheckWindowsWMIC()
    {
        try
        {
            string output = ExecuteCommand("wmic computersystem get manufacturer,model");

            if (output.Contains("VMware"))
                return VMType.VMware;
            if (output.Contains("VirtualBox") || output.Contains("innotek"))
                return VMType.VirtualBox;
            if (output.Contains("Microsoft") && output.Contains("Virtual"))
                return VMType.HyperV;
            if (output.Contains("Xen"))
                return VMType.Xen;
            if (output.Contains("QEMU"))
                return VMType.QEMU;
            if (output.Contains("Parallels"))
                return VMType.Parallels;

            output = ExecuteCommand("wmic bios get serialnumber,version,manufacturer");

            if (output.Contains("VMware"))
                return VMType.VMware;
            if (output.Contains("VirtualBox") || output.Contains("VBOX"))
                return VMType.VirtualBox;
            if (output.Contains("Xen"))
                return VMType.Xen;

            output = ExecuteCommand("wmic diskdrive get model,caption");

            if (output.Contains("VMware"))
                return VMType.VMware;
            if (output.Contains("VBOX"))
                return VMType.VirtualBox;

            output = ExecuteCommand("wmic path win32_VideoController get caption,description");

            if (output.Contains("VMware"))
                return VMType.VMware;
            if (output.Contains("VirtualBox") || output.Contains("VBox"))
                return VMType.VirtualBox;
            if (output.Contains("Hyper-V"))
                return VMType.HyperV;
        }
        catch
        {
        }

        return VMType.None;
    }

    private static VMType CheckWindowsPowerShell()
    {
        try
        {
            string output = ExecuteCommand("powershell -Command \"Get-CimInstance -ClassName Win32_ComputerSystem | Select-Object -Property Manufacturer,Model | Format-List\"");

            if (output.Contains("VMware"))
                return VMType.VMware;
            if (output.Contains("VirtualBox") || output.Contains("innotek"))
                return VMType.VirtualBox;
            if (output.Contains("Microsoft") && output.Contains("Virtual"))
                return VMType.HyperV;
            if (output.Contains("Xen"))
                return VMType.Xen;
            if (output.Contains("QEMU"))
                return VMType.QEMU;
            if (output.Contains("Parallels"))
                return VMType.Parallels;

            output = ExecuteCommand("powershell -Command \"Get-CimInstance -ClassName Win32_BIOS | Format-List\"");

            if (output.Contains("VMware"))
                return VMType.VMware;
            if (output.Contains("VirtualBox") || output.Contains("VBOX"))
                return VMType.VirtualBox;
            if (output.Contains("Xen"))
                return VMType.Xen;
        }
        catch
        {
            // ignored
        }

        return VMType.None;
    }

    private static VMType DetectVMMacOS()
    {
        VMType vmType = CheckMacOSSystemProfiler();
        if (vmType != VMType.None)
            return vmType;

        vmType = CheckMacOSFiles();
        if (vmType != VMType.None)
            return vmType;

        vmType = CheckMacOSProcesses();
        if (vmType != VMType.None)
            return vmType;

        vmType = CheckMacOSIORegistry();
        if (vmType != VMType.None)
            return vmType;

        return VMType.None;
    }

    private static VMType CheckMacOSSystemProfiler()
    {
        try
        {
            string output = ExecuteCommand("system_profiler SPHardwareDataType");

            if (output.Contains("VMware"))
                return VMType.VMware;
            if (output.Contains("VirtualBox"))
                return VMType.VirtualBox;
            if (output.Contains("Parallels"))
                return VMType.Parallels;
            if (output.Contains("QEMU"))
                return VMType.QEMU;
            if (output.Contains("Virtual Machine"))
                return VMType.Other;

            if (output.Contains("Mac") && Regex.IsMatch(output, @"Model\s+Identifier:\s+Parallels\w+"))
                return VMType.Parallels;

            if (Regex.IsMatch(output, @"Serial\s+Number\s+\(system\):\s+VMw"))
                return VMType.VMware;
            if (Regex.IsMatch(output, @"Hardware\s+UUID:\s+[0-9A-F-]+\-00000000"))
                return VMType.Other;
        }
        catch
        {
        }

        return VMType.None;
    }

    private static VMType CheckMacOSFiles()
    {
        if (Directory.Exists("/Library/Application Support/VMware Tools"))
            return VMType.VMware;

        if (Directory.Exists("/Library/Parallels Guest Tools"))
            return VMType.Parallels;

        if (Directory.Exists("/Library/VBoxGuestAdditions"))
            return VMType.VirtualBox;

        if (File.Exists("/dev/vmmon"))
            return VMType.VMware;

        return VMType.None;
    }

    private static VMType CheckMacOSProcesses()
    {
        try
        {
            string processes = ExecuteCommand("ps -ef");

            if (processes.Contains("vmware-tools") || processes.Contains("vmtoolsd"))
                return VMType.VMware;
            if (processes.Contains("VBoxService") || processes.Contains("VBoxClient"))
                return VMType.VirtualBox;
            if (processes.Contains("prl_tools") || processes.Contains("prl_client"))
                return VMType.Parallels;
        }
        catch
        {
        }

        return VMType.None;
    }

    private static VMType CheckMacOSIORegistry()
    {
        try
        {
            string ioregOutput = ExecuteCommand("ioreg -l");

            if (ioregOutput.Contains("VMware"))
                return VMType.VMware;
            if (ioregOutput.Contains("VirtualBox"))
                return VMType.VirtualBox;
            if (ioregOutput.Contains("Parallels"))
                return VMType.Parallels;

            if (ioregOutput.Contains("VMwareGfx") || ioregOutput.Contains("VMwareVGA"))
                return VMType.VMware;
            if (ioregOutput.Contains("VBoxGuest") || ioregOutput.Contains("VBoxVideo"))
                return VMType.VirtualBox;

            string smbiosOutput = ExecuteCommand("ioreg -d2 -c IOPlatformExpertDevice");
            if (smbiosOutput.Contains("VMware") || smbiosOutput.Contains("Virtual"))
                return VMType.VMware;
            if (smbiosOutput.Contains("VirtualBox"))
                return VMType.VirtualBox;
            if (smbiosOutput.Contains("Parallels"))
                return VMType.Parallels;
            if (smbiosOutput.Contains("QEMU"))
                return VMType.QEMU;
        }
        catch
        {
        }

        return VMType.None;
    }

    private static VMType DetectVMLinux()
    {
        try
        {
            if (File.Exists("/proc/scsi/scsi"))
            {
                string scsiContent = File.ReadAllText("/proc/scsi/scsi");
                if (scsiContent.Contains("VMware"))
                    return VMType.VMware;
                if (scsiContent.Contains("VBOX"))
                    return VMType.VirtualBox;
            }

            if (File.Exists("/sys/class/dmi/id/product_name"))
            {
                string productName = File.ReadAllText("/sys/class/dmi/id/product_name");
                if (productName.Contains("VMware"))
                    return VMType.VMware;
                if (productName.Contains("VirtualBox"))
                    return VMType.VirtualBox;
                if (productName.Contains("QEMU"))
                    return VMType.QEMU;
                if (productName.Contains("KVM"))
                    return VMType.KVM;
                if (productName.Contains("Xen"))
                    return VMType.Xen;
                if (productName.Contains("Virtual Machine"))
                    return VMType.HyperV;
            }

            if (File.Exists("/proc/cpuinfo"))
            {
                string cpuInfo = File.ReadAllText("/proc/cpuinfo");
                if (cpuInfo.Contains("hypervisor") || cpuInfo.Contains("QEMU") || cpuInfo.Contains("KVM"))
                    return VMType.Other;
            }

            string lsmodOutput = ExecuteCommand("lsmod");
            if (lsmodOutput.Contains("vmw_") || lsmodOutput.Contains("vmware"))
                return VMType.VMware;
            if (lsmodOutput.Contains("vboxguest") || lsmodOutput.Contains("vboxsf"))
                return VMType.VirtualBox;

            string virtOutput = ExecuteCommand("systemd-detect-virt");
            if (!string.IsNullOrEmpty(virtOutput))
            {
                virtOutput = virtOutput.Trim().ToLower();
                if (virtOutput == "vmware")
                    return VMType.VMware;
                if (virtOutput == "oracle")
                    return VMType.VirtualBox;
                if (virtOutput == "microsoft")
                    return VMType.HyperV;
                if (virtOutput == "xen")
                    return VMType.Xen;
                if (virtOutput == "kvm")
                    return VMType.KVM;
                if (virtOutput == "qemu")
                    return VMType.QEMU;
                if (virtOutput != "none")
                    return VMType.Other;
            }

            string lspciOutput = ExecuteCommand("lspci");
            if (lspciOutput.Contains("VMware"))
                return VMType.VMware;
            if (lspciOutput.Contains("VirtualBox"))
                return VMType.VirtualBox;
        }
        catch
        {
        }

        return VMType.None;
    }

    private static string ExecuteCommand(string command)
    {
        try
        {
            var processInfo = new ProcessStartInfo
            {
                FileName = RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? "cmd.exe" : "/bin/bash",
                Arguments = RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? $"/c {command}" : $"-c \"{command}\"",
                RedirectStandardOutput = true,
                UseShellExecute = false,
                CreateNoWindow = true
            };

            using (var process = Process.Start(processInfo))
            {
                using (var reader = process.StandardOutput)
                {
                    return reader.ReadToEnd();
                }
            }
        }
        catch
        {
            return string.Empty;
        }
    }
}