#!/usr/bin/env python3
"""
Full build pipeline for ane-awesome-utils ANE.

Usage:
  python build-all.py              # Full build
  python build-all.py windows-native   # Single step
  python build-all.py -v               # Verbose (show all subprocess output)
"""

import os
import sys
import time
import shutil
import subprocess
import re
import threading
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed

# ── Config ───────────────────────────────────────────────────────────────

PROJECT_ROOT = Path(__file__).parent.resolve()
ANE_BUILD = PROJECT_ROOT / "AneBuild"

VS2017_VCVARS = r"C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvarsall.bat"
NINJA = r"C:\Users\Joao\AppData\Local\Programs\CLion\bin\ninja\win\x64\ninja.exe"
CMAKE = r"C:\Users\Joao\AppData\Local\Programs\CLion\bin\cmake\win\x64\bin\cmake.exe"
SEVEN_ZIP = r"C:\Program Files\7-Zip\7z.exe"

MAC_PROJECT_PATH = "~/IdeaProjects/ane-awesome-utils"
MAC_DOTNET_PATH = "/usr/local/share/dotnet"
MAC_SSH_PREFIX = f"export PATH=$PATH:{MAC_DOTNET_PATH};"
MAC_CSHARP_PROJECT = f"{MAC_PROJECT_PATH}/CSharpLibrary/AwesomeAneUtils/AwesomeAneUtils/AwesomeAneUtils.csproj"
MAC_CSHARP_OUTPUT = f"{MAC_PROJECT_PATH}/CSharpLibrary/AwesomeAneUtils/AwesomeAneUtils/bin/Release/net10.0"

VERBOSE = "-v" in sys.argv or "--verbose" in sys.argv

# thread-safe print; suppressed in background threads when _quiet_threads is set
_print_lock = threading.Lock()
_quiet_threads = set()


def log(msg, **kwargs):
    if threading.current_thread().ident in _quiet_threads:
        return
    with _print_lock:
        print(msg, flush=True, **kwargs)


def header(msg):
    log(f"\n{'=' * 70}\n  {msg}\n{'=' * 70}")


# ── Helpers ──────────────────────────────────────────────────────────────

class BuildError(Exception):
    pass


def load_env():
    env_file = PROJECT_ROOT / ".env"
    if not env_file.exists():
        raise BuildError(f".env not found at {env_file}. Create from .env.example.")
    env = {}
    for line in env_file.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip()
    for var in ("MAC_HOST", "MAC_KEYCHAIN_PASS", "AIRSDK_PATH", "SIGNING_IDENTITY",
                "CODESIGNTOOL_PATH", "CODESIGNTOOL_USERNAME", "CODESIGNTOOL_PASSWORD", "CODESIGNTOOL_TOTP_SECRET"):
        if var not in env:
            raise BuildError(f"Missing '{var}' in .env")
    return env


def run(cmd, cwd=None, shell=False, check=True, quiet=False):
    """Run a local command. quiet=True captures output (shown only on error)."""
    if quiet and not VERBOSE:
        r = subprocess.run(cmd, cwd=cwd, shell=shell, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if check and r.returncode != 0:
            log(r.stdout)
            raise BuildError(f"Command failed (exit {r.returncode})")
        return r.returncode
    else:
        r = subprocess.run(cmd, cwd=cwd, shell=shell, stdout=sys.stdout, stderr=subprocess.STDOUT)
        if check and r.returncode != 0:
            raise BuildError(f"Command failed (exit {r.returncode})")
        return r.returncode


def ssh(env, command, check=True, quiet=False):
    """Run command on Mac via SSH."""
    host = env["MAC_HOST"]
    full = f"{MAC_SSH_PREFIX} {command}"
    if quiet and not VERBOSE:
        r = subprocess.run(
            ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", host, full],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        if check and r.returncode != 0:
            log(r.stdout)
            raise BuildError(f"SSH failed (exit {r.returncode})")
        return r.returncode
    else:
        r = subprocess.run(
            ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", host, full],
            stdout=sys.stdout, stderr=subprocess.STDOUT,
        )
        if check and r.returncode != 0:
            raise BuildError(f"SSH failed (exit {r.returncode})")
        return r.returncode


def ssh_capture(env, command, check=True):
    host = env["MAC_HOST"]
    r = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", host, f"{MAC_SSH_PREFIX} {command}"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    if check and r.returncode != 0:
        log(r.stdout)
        raise BuildError(f"SSH failed (exit {r.returncode})")
    return r.stdout.strip()


def scp(env, src, dst, quiet=False):
    if quiet and not VERBOSE:
        r = subprocess.run(
            ["scp", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", src, dst],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
    else:
        r = subprocess.run(
            ["scp", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", src, dst],
            stdout=sys.stdout, stderr=subprocess.STDOUT,
        )
    if r.returncode != 0:
        raise BuildError(f"SCP failed: {src} -> {dst}")


def scp_recursive(env, src, dst):
    r = subprocess.run(
        ["scp", "-r", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", src, dst],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    if r.returncode != 0:
        raise BuildError(f"SCP recursive failed: {src} -> {dst}")


def unlock_cmd(env):
    pw = env.get("MAC_KEYCHAIN_PASS", "")
    if not pw:
        return ""
    return (
        f"security unlock-keychain -p '{pw}' ~/Library/Keychains/login.keychain-db && "
        f"security list-keychains -d user -s ~/Library/Keychains/login.keychain-db && "
        f"security set-key-partition-list -S apple-tool:,apple: -s -k '{pw}' ~/Library/Keychains/login.keychain-db >/dev/null 2>&1 && "
    )


def timed(label):
    """Context manager that prints elapsed time."""
    class Timer:
        def __enter__(self):
            self.start = time.time()
            return self
        def __exit__(self, *_):
            s = time.time() - self.start
            log(f"  {label} done ({s:.1f}s)")
    return Timer()


# ── Steps ────────────────────────────────────────────────────────────────

def step_sync(env):
    header("Step 1: Syncing source to Mac")
    host = env["MAC_HOST"]
    tar_path = PROJECT_ROOT / "ane-sync.tar.gz"

    log("  Creating archive...")
    with timed("Sync"):
        # Build tar excluding build artifacts
        import tarfile
        with tarfile.open(tar_path, "w:gz") as tar:
            for folder in ("AppleNative", "CSharpLibrary"):
                folder_path = PROJECT_ROOT / folder
                if folder_path.exists():
                    tar.add(folder_path, arcname=folder,
                            filter=lambda ti: None if any(x in ti.name for x in ("/DerivedData", "/bin/", "/obj/", "/.cxx/")) else ti)

        ssh(env, f"rm -rf {MAC_PROJECT_PATH}/AppleNative {MAC_PROJECT_PATH}/CSharpLibrary && mkdir -p {MAC_PROJECT_PATH}", quiet=True)
        scp(env, str(tar_path), f"{host}:{MAC_PROJECT_PATH}/ane-sync.tar.gz", quiet=True)
        ssh(env, f"cd {MAC_PROJECT_PATH} && tar xzf ane-sync.tar.gz && rm ane-sync.tar.gz", quiet=True)
        tar_path.unlink(missing_ok=True)


def step_dotnet_mac(env):
    header("Step 2: .NET NativeAOT on Mac (macOS + iOS)")
    csproj = MAC_CSHARP_PROJECT
    out = MAC_CSHARP_OUTPUT
    for rid, flags in [
        ("osx-arm64", "-p:NativeLib=Shared"),
        ("osx-x64",   "-p:NativeLib=Shared"),
        ("ios-arm64",  "-p:NativeLib=Static -p:PublishAotUsingRuntimePack=true"),
    ]:
        log(f"  Building {rid}...")
        with timed(rid):
            ssh(env, f"cd {MAC_PROJECT_PATH}/AneBuild && dotnet publish -c Release -r {rid} {flags} {csproj}", quiet=True)

    arm64 = f"{out}/osx-arm64/native/AwesomeAneUtils.dylib"
    x64 = f"{out}/osx-x64/native/AwesomeAneUtils.dylib"
    uni_dir = f"{out}/macos-universal"
    uni = f"{uni_dir}/AwesomeAneUtils.dylib"
    ios_arm64 = f"{out}/ios-arm64/native/AwesomeAneUtils.a"
    ios_dir = f"{out}/ios-universal"
    ios_lib = f"{ios_dir}/AwesomeAneUtils.a"

    log("  Creating universal binary + preparing iOS lib...")
    with timed("lipo+prep"):
        ssh(env, f"mkdir -p {uni_dir} {ios_dir} && lipo -create -output {uni} {arm64} {x64} && install_name_tool -id '@loader_path/Frameworks/AwesomeAneUtils.dylib' {uni} && cp {ios_arm64} {ios_lib}", quiet=True)


def step_xcode(env):
    header("Step 3: Signing + Xcode builds")
    unlock = unlock_cmd(env)
    identity = env["SIGNING_IDENTITY"]
    xcode_dir = f"{MAC_PROJECT_PATH}/AppleNative/AneAwesomeUtils"
    out = MAC_CSHARP_OUTPUT
    uni = f"{out}/macos-universal/AwesomeAneUtils.dylib"
    ios_lib = f"{out}/ios-universal/AwesomeAneUtils.a"

    log("  Signing macOS dylib...")
    with timed("sign-mac"):
        ssh(env, f"{unlock}codesign --force --sign '{identity}' --options=runtime --timestamp {uni}", quiet=True)

    log("  Signing iOS lib...")
    with timed("sign-ios"):
        ssh(env, f"{unlock}codesign --force --sign '{identity}' --options=runtime --timestamp {ios_lib}", quiet=True)

    log("  Building macOS framework...")
    with timed("xcode-mac"):
        ssh(env, f"{unlock}cd {xcode_dir} && xcodebuild build -project AneAwesomeUtils.xcodeproj -scheme AneAwesomeUtils -configuration Debug -derivedDataPath DerivedData-macOS -destination 'generic/platform=macOS' ONLY_ACTIVE_ARCH=NO -quiet 2>&1", quiet=True)

    log("  Building iOS static library...")
    with timed("xcode-ios"):
        ssh(env, f"{unlock}cd {xcode_dir} && xcodebuild build -project AneAwesomeUtils.xcodeproj -scheme AneAwesomeUtils-IOS -configuration Debug -derivedDataPath DerivedData-iOS -destination 'generic/platform=iOS' ONLY_ACTIVE_ARCH=NO -quiet 2>&1", quiet=True)


def step_copy_mac(env):
    header("Step 4: Copying Mac/iOS binaries")
    host = env["MAC_HOST"]
    mac_prod = f"{MAC_PROJECT_PATH}/AppleNative/AneAwesomeUtils/DerivedData-macOS/Build/Products"
    ios_prod = f"{MAC_PROJECT_PATH}/AppleNative/AneAwesomeUtils/DerivedData-iOS/Build/Products"

    with timed("Copy"):
        scp(env, f"{host}:{ios_prod}/Debug-iphoneos/libAneAwesomeUtils-IOS.a",
            str(ANE_BUILD / "ios" / "libAneAwesomeUtils-IOS.a"), quiet=True)

        macos_fw = ANE_BUILD / "macos" / "AneAwesomeUtils.framework"
        if macos_fw.exists():
            shutil.rmtree(macos_fw)
        scp_recursive(env, f"{host}:{mac_prod}/Debug/AneAwesomeUtils.framework/Versions/Current", str(macos_fw))

        # iOS NativeCsharp runtime libs (exclude main AOT output - already in libAneAwesomeUtils-IOS.a)
        ios_native_dir = ANE_BUILD / "ios" / "NativeCsharp"
        mac_ios_pub = f"{MAC_CSHARP_OUTPUT}/ios-arm64/native"
        file_list = ssh_capture(env, f"find {mac_ios_pub} -maxdepth 1 \\( -name '*.a' -o -name '*.o' \\) ! -name 'AwesomeAneUtils.a' -exec basename {{}} \\;")
        for f in file_list.splitlines():
            f = f.strip()
            if f and re.match(r".*\.(a|o)$", f):
                scp(env, f"{host}:{mac_ios_pub}/{f}", str(ios_native_dir / f), quiet=True)
        dup = ios_native_dir / "AwesomeAneUtils.a"
        if dup.exists():
            dup.unlink()


def step_windows_native(env):
    header("Step 5: Windows native C++ (x86 + x64)")
    win_native = PROJECT_ROOT / "WindowsNative"

    for arch in ("x86", "x64"):
        build_dir = win_native / ("cmake-build-release-x32" if arch == "x86" else "cmake-build-release-x64")
        vc_arch = "x86" if arch == "x86" else "x86_amd64"
        build_dir.mkdir(parents=True, exist_ok=True)

        bat = build_dir / "build.bat"
        bat.write_text(f"""@echo off
call "{VS2017_VCVARS}" {vc_arch}
if errorlevel 1 exit /b 1
cd /d "{build_dir}"
"{CMAKE}" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="{NINJA}" -DCMAKE_C_COMPILER=cl.exe -DCMAKE_CXX_COMPILER=cl.exe "{win_native}"
if errorlevel 1 exit /b 1
"{NINJA}" -j%NUMBER_OF_PROCESSORS%
if errorlevel 1 exit /b 1
""", encoding="ascii")

        log(f"  Building {arch}...")
        with timed(arch):
            run(f'"{bat}"', shell=True, quiet=True)

        target = "windows-32" if arch == "x86" else "windows-64"
        shutil.copy2(build_dir / "AneAwesomeUtilsWindows.dll", ANE_BUILD / target / "AneAwesomeUtilsWindows.dll")


def step_dotnet_win(env):
    header("Step 6: .NET Windows (x86 + x64)")
    csproj = str(PROJECT_ROOT / "CSharpLibrary" / "AwesomeAneUtils" / "AwesomeAneUtils" / "AwesomeAneUtils.csproj")

    vswhere = r"C:\Program Files (x86)\Microsoft Visual Studio\Installer"
    if os.path.isdir(vswhere):
        os.environ["PATH"] = vswhere + ";" + os.environ.get("PATH", "")

    for rid, label in [("win-x86", "x86"), ("win-x64", "x64")]:
        log(f"  Building {label}...")
        rid_flag = f"-r {rid}" if rid != "win-x86" else ""
        with timed(label):
            run(f'dotnet publish /p:NativeLib=Shared /p:Configuration=Release {rid_flag} "{csproj}"', shell=True, quiet=True)

    cs_base = PROJECT_ROOT / "CSharpLibrary" / "AwesomeAneUtils" / "AwesomeAneUtils" / "bin" / "Release" / "net10.0"
    shutil.copy2(cs_base / "win-x86" / "publish" / "AwesomeAneUtils.dll", ANE_BUILD / "windows-32" / "AwesomeAneUtils.dll")
    shutil.copy2(cs_base / "win-x64" / "publish" / "AwesomeAneUtils.dll", ANE_BUILD / "windows-64" / "AwesomeAneUtils.dll")


def step_android(env):
    header("Step 7: Android (Gradle)")
    android_dir = PROJECT_ROOT / "AndroidNative"
    gradlew = android_dir / "gradlew.bat"

    log("  Building...")
    with timed("Gradle"):
        run(f'cd /d "{android_dir}" && "{gradlew}" assembleDebug -q', shell=True, quiet=True)

    shutil.copy2(android_dir / "app" / "build" / "outputs" / "aar" / "app-debug.aar",
                 ANE_BUILD / "android" / "app-debug.aar")


def step_as3(env):
    header("Step 8: AS3 to SWC")
    airsdk = env["AIRSDK_PATH"]
    compc = os.path.join(airsdk, "bin", "compc.bat")
    air_global = os.path.join(airsdk, "frameworks", "libs", "air", "airglobal.swc")
    src_dir = str(PROJECT_ROOT / "src")
    out_dir = PROJECT_ROOT / "out" / "production" / "ane-awesome-utils"
    out_dir.mkdir(parents=True, exist_ok=True)
    swc_output = str(out_dir / "ane-awesome-utils.swc")

    log("  Compiling...")
    with timed("compc"):
        run(f'"{compc}" -output "{swc_output}" -include-sources "{src_dir}" -external-library-path "{air_global}"', shell=True, quiet=True)

    shutil.copy2(swc_output, ANE_BUILD / "library.swc")
    for plat in ("default", "android", "windows-32", "windows-64", "macos", "ios"):
        plat_dir = ANE_BUILD / plat
        plat_dir.mkdir(parents=True, exist_ok=True)
        subprocess.run(f'"{SEVEN_ZIP}" e "{ANE_BUILD / "library.swc"}" "library.swf" "-o{plat_dir}" -aoa',
                       shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def step_package(env):
    header("Step 9: Packaging ANE")
    airsdk = env["AIRSDK_PATH"]
    adt_jar = os.path.join(airsdk, "lib", "adt.jar")
    codesigntool = os.path.join(env["CODESIGNTOOL_PATH"], "CodeSignTool.bat")
    cst_user = env["CODESIGNTOOL_USERNAME"]
    cst_pass = env["CODESIGNTOOL_PASSWORD"]
    cst_totp = env["CODESIGNTOOL_TOTP_SECRET"]

    dlls = [
        "windows-32\\AwesomeAneUtils.dll",
        "windows-32\\AneAwesomeUtilsWindows.dll",
        "windows-64\\AwesomeAneUtils.dll",
        "windows-64\\AneAwesomeUtilsWindows.dll",
    ]
    sign_tmp = ANE_BUILD / "codesign-tmp"
    if sign_tmp.exists():
        shutil.rmtree(sign_tmp)
    sign_tmp.mkdir(parents=True)

    cst_home = env["CODESIGNTOOL_PATH"]
    log("  Signing Windows DLLs...")
    # SSL.com eSigner treats each TOTP code as single-use: once the tool
    # consumes a code to authenticate, calling sign() again inside the
    # same 30 s TOTP window fails with "Unexpected character (<) at
    # position 0" — the API serves an HTML error page instead of JSON.
    # We wait just past the 30 s period between calls so the TOTP
    # generator has rotated to a fresh code.
    import time as _time
    TOTP_WINDOW_SEC = 32

    def _sanitize_codesign_output(text):
        text = text or ""
        for secret in (cst_user, cst_pass, cst_totp):
            if secret:
                text = text.replace(secret, "<redacted>")
        return text

    def _sign_dll(src, out_dir):
        cmd = (f'set "CODE_SIGN_TOOL_PATH={cst_home}" && '
               f'"{codesigntool}" sign -input_file_path="{src}"'
               f' -output_dir_path="{out_dir}"'
               f' -username="{cst_user}" -password="{cst_pass}"'
               f' -totp_secret="{cst_totp}"')
        return subprocess.run(
            cmd,
            shell=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

    with timed("codesigntool"):
        for idx, dll in enumerate(dlls):
            if idx > 0:
                log(f"  waiting {TOTP_WINDOW_SEC}s for TOTP window to rotate...")
                _time.sleep(TOTP_WINDOW_SEC)
            src = ANE_BUILD / dll
            before = src.stat().st_mtime
            signed = None
            last_output = ""
            for attempt in range(1, 4):
                out_dir = sign_tmp / f"{idx}-{attempt}"
                if out_dir.exists():
                    shutil.rmtree(out_dir)
                out_dir.mkdir(parents=True)
                r = _sign_dll(src, out_dir)
                last_output = _sanitize_codesign_output(r.stdout)
                candidate = out_dir / src.name
                if r.returncode == 0 and candidate.exists():
                    signed = candidate
                    break
                if attempt < 3:
                    log(f"  CodeSignTool produced no output for {dll}; retrying after TOTP rotation...")
                    _time.sleep(TOTP_WINDOW_SEC)
                else:
                    if last_output:
                        log(last_output)
                    if r.returncode != 0:
                        raise BuildError(f"CodeSignTool failed for {dll} (exit {r.returncode})")
                    raise BuildError(f"CodeSignTool produced no output for {dll}")
            shutil.move(str(signed), str(src))
            if src.stat().st_mtime == before:
                raise BuildError(f"Signed DLL did not replace original: {dll}")
        shutil.rmtree(sign_tmp)

    log("  Running ADT...")
    with timed("ADT"):
        run(f'java -jar "{adt_jar}"'
            f" -package -target ane br.com.redesurftank.aneawesomeutils.ane extension.xml -swc library.swc"
            f" -platform default -C default ."
            f" -platform Windows-x86 -C windows-32 ."
            f" -platform Windows-x86-64 -C windows-64 ."
            f" -platform MacOS-x86-64 -C macos ."
            f" -platform iPhone-ARM -platformoptions platformIOS.xml -C ios ."
            f" -platform Android -platformoptions platformAndroid.xml -C android .",
            cwd=ANE_BUILD, shell=True, quiet=True)

    ane = ANE_BUILD / "br.com.redesurftank.aneawesomeutils.ane"
    log(f"  ANE: {ane.name} ({ane.stat().st_size / 1048576:.1f} MB)")


# ── Registry ─────────────────────────────────────────────────────────────

STEPS = {
    "sync": step_sync, "dotnet-mac": step_dotnet_mac, "xcode": step_xcode,
    "copy-mac": step_copy_mac, "windows-native": step_windows_native,
    "dotnet-win": step_dotnet_win, "android": step_android,
    "as3": step_as3, "package": step_package,
}
MAC_STEPS = ["dotnet-mac", "xcode", "copy-mac"]
LOCAL_STEPS = ["windows-native", "dotnet-win", "android", "as3"]


# ── Main ─────────────────────────────────────────────────────────────────

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    step = args[0] if args else "all"

    if step not in STEPS and step != "all":
        print(f"Unknown step: {step}\nValid: all, {', '.join(STEPS.keys())}")
        sys.exit(1)

    env = load_env()
    start = time.time()

    try:
        if step != "all":
            STEPS[step](env)
        else:
            step_sync(env)

            # Local builds in background threads (captured output, no interleaving)
            log(f"\n  >> Local builds: {', '.join(LOCAL_STEPS)} (background)")
            log(f"  >> Mac builds: {', '.join(MAC_STEPS)} (foreground)\n")

            local_errors = {}

            def run_local_quiet(step_name, env_dict):
                """Run a local step silently (no console output from threads)."""
                _quiet_threads.add(threading.current_thread().ident)
                try:
                    STEPS[step_name](env_dict)
                finally:
                    _quiet_threads.discard(threading.current_thread().ident)

            with ThreadPoolExecutor(max_workers=4) as pool:
                futures = {pool.submit(run_local_quiet, s, env): s for s in LOCAL_STEPS}

                # Mac foreground
                for s in MAC_STEPS:
                    STEPS[s](env)

                # Collect local results
                log("")
                for future in as_completed(futures):
                    name = futures[future]
                    try:
                        future.result()
                        log(f"  [OK] {name}")
                    except Exception as e:
                        log(f"  [FAIL] {name}: {e}")
                        local_errors[name] = str(e)

            if local_errors:
                raise BuildError(f"Failed: {', '.join(local_errors.keys())}")

            step_package(env)

        elapsed = time.time() - start
        log(f"\n{'=' * 70}\n  BUILD COMPLETE in {elapsed / 60:.1f} minutes\n{'=' * 70}")

    except Exception as e:
        elapsed = time.time() - start
        log(f"\n{'=' * 70}\n  BUILD FAILED: {e}\n  Elapsed: {elapsed / 60:.1f} minutes\n{'=' * 70}")
        sys.exit(1)


if __name__ == "__main__":
    main()
