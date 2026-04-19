"""Launch a Windows process with an injected DLL via CreateRemoteThread.

Used here to validate that loading InjectTest.dll into the captive
TestProfilerApp.exe early enough makes Adobe AIR read .telemetry.cfg
and wire up the sampler.

Flow:
  1. CreateProcess target_exe with CREATE_SUSPENDED
  2. VirtualAllocEx + WriteProcessMemory write the DLL path into the
     target's address space
  3. CreateRemoteThread with LoadLibraryA as the start address and our
     path string as the argument — this fires when the target is still
     suspended; the injected DLL's DllMain runs under the remote thread
  4. Close the remote thread handle
  5. ResumeThread on the main thread — target runs as normal, but with
     our DLL already mapped and initialized

Usage:
  python inject_and_run.py <target_exe> <dll_to_inject>
"""
import ctypes
import ctypes.wintypes as wt
import sys

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

PROCESS_ALL_ACCESS = 0x1F0FFF
MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
PAGE_READWRITE = 0x04
CREATE_SUSPENDED = 0x00000004

class STARTUPINFOA(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPSTR), ("lpDesktop", wt.LPSTR),
                ("lpTitle", wt.LPSTR), ("dwX", wt.DWORD), ("dwY", wt.DWORD),
                ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD),
                ("dwXCountChars", wt.DWORD), ("dwYCountChars", wt.DWORD),
                ("dwFillAttribute", wt.DWORD), ("dwFlags", wt.DWORD),
                ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
                ("lpReserved2", wt.LPBYTE), ("hStdInput", wt.HANDLE),
                ("hStdOutput", wt.HANDLE), ("hStdError", wt.HANDLE)]

class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE),
                ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


def main():
    target_exe, dll_path = sys.argv[1], sys.argv[2]
    dll_path_bytes = dll_path.encode("ascii") + b"\x00"

    si = STARTUPINFOA(); si.cb = ctypes.sizeof(si)
    pi = PROCESS_INFORMATION()
    cmdline = ctypes.create_string_buffer(f'"{target_exe}"'.encode("ascii"))
    # NOT suspended — we need kernel32 loaded in the target before we
    # CreateRemoteThread. A few ms race with Adobe's init_telemetry is
    # acceptable: init_telemetry runs LATE in startup (after SWF parse,
    # AvmCore alloc). Injection completes in < 10ms on a modern box.
    ok = kernel32.CreateProcessA(
        target_exe.encode("ascii"), cmdline,
        None, None, False, 0, None, None,
        ctypes.byref(si), ctypes.byref(pi))
    if not ok:
        raise SystemExit(f"CreateProcess failed: {ctypes.get_last_error()}")
    print(f"spawned pid={pi.dwProcessId} (suspended)")

    remote_mem = kernel32.VirtualAllocEx(
        pi.hProcess, None, len(dll_path_bytes),
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
    if not remote_mem:
        raise SystemExit(f"VirtualAllocEx failed: {ctypes.get_last_error()}")
    written = ctypes.c_size_t(0)
    ok = kernel32.WriteProcessMemory(
        pi.hProcess, ctypes.c_void_p(remote_mem),
        dll_path_bytes, len(dll_path_bytes), ctypes.byref(written))
    if not ok:
        raise SystemExit(f"WriteProcessMemory failed: {ctypes.get_last_error()}")
    print(f"wrote dll path at remote addr 0x{remote_mem:x}")

    # The target is a 32-bit process; we're running as 64-bit Python.
    # GetProcAddress on our 64-bit kernel32 gives the wrong address.
    # Instead, parse the 32-bit kernel32.dll in SysWOW64 to get the RVA
    # of LoadLibraryA, then add the module's base inside the target.
    import pefile
    sw = pefile.PE(r"C:\Windows\SysWOW64\kernel32.dll", fast_load=True)
    sw.parse_data_directories(
        directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
    load_library_rva = None
    for e in sw.DIRECTORY_ENTRY_EXPORT.symbols:
        if e.name == b"LoadLibraryA":
            load_library_rva = e.address
            break
    if load_library_rva is None:
        raise SystemExit("LoadLibraryA not found in SysWOW64 kernel32")
    sw.close()

    # Enumerate the target's 32-bit modules to find kernel32's base there.
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    LIST_MODULES_32BIT = 0x01
    target_kernel32_base = None
    import time
    for attempt in range(200):  # up to ~2s — wait for kernel32 to load
        modules = (wt.HMODULE * 512)()
        needed = wt.DWORD()
        ok = psapi.EnumProcessModulesEx(
            pi.hProcess, modules, ctypes.sizeof(modules),
            ctypes.byref(needed), LIST_MODULES_32BIT)
        if ok:
            for i in range(needed.value // ctypes.sizeof(wt.HMODULE)):
                buf = ctypes.create_string_buffer(260)
                psapi.GetModuleBaseNameA(pi.hProcess, modules[i], buf, 260)
                name = buf.value.decode("ascii", "ignore").lower()
                if name == "kernel32.dll":
                    target_kernel32_base = modules[i] & 0xFFFFFFFF
                    break
            if target_kernel32_base:
                break
        time.sleep(0.01)
    if target_kernel32_base is None:
        raise SystemExit("kernel32.dll not found in target process")
    load_library_a = target_kernel32_base + load_library_rva
    print(f"target kernel32 base 0x{target_kernel32_base:x}, "
          f"LoadLibraryA @ 0x{load_library_a:x}")

    remote_thread = kernel32.CreateRemoteThread(
        pi.hProcess, None, 0,
        ctypes.c_void_p(load_library_a),
        ctypes.c_void_p(remote_mem), 0, None)
    if not remote_thread:
        raise SystemExit(f"CreateRemoteThread failed: {ctypes.get_last_error()}")
    print(f"remote thread created; waiting for DllMain to finish")
    kernel32.WaitForSingleObject(remote_thread, 5000)
    exit_code = wt.DWORD(0)
    kernel32.GetExitCodeThread(remote_thread, ctypes.byref(exit_code))
    print(f"remote thread exit (= hmodule of loaded DLL) = 0x{exit_code.value:x}")
    kernel32.CloseHandle(remote_thread)

    kernel32.CloseHandle(pi.hThread)

    # Wait for process exit
    kernel32.WaitForSingleObject(pi.hProcess, 60000)
    proc_exit = wt.DWORD(0)
    kernel32.GetExitCodeProcess(pi.hProcess, ctypes.byref(proc_exit))
    print(f"process exit = 0x{proc_exit.value:x}")
    kernel32.CloseHandle(pi.hProcess)


main()
