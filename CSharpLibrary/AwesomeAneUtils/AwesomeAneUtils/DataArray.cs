using System;
using System.Runtime.InteropServices;

namespace AwesomeAneUtils;

[StructLayout(LayoutKind.Sequential)]
public struct DataArray
{
    public IntPtr DataPointer;
    public int Size;
}