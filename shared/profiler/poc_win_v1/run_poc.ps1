# POC v1 end-to-end runner.
#
# 1. Writes %USERPROFILE%\.telemetry.cfg pointing to 127.0.0.1:17934
#    (backs up existing).
# 2. Starts the Python TCP listener on 17934, writing to scout_from_listener.bin.
# 3. Launches profiler_inject.exe with air_profiler_probe.dll and a packaged
#    AIR captive target. The injector spawns the target CREATE_SUSPENDED,
#    LoadLibrarys the hook DLL, then resumes.
# 4. Waits $TargetSeconds for the app to produce telemetry, then kills the
#    target. Shuts down the listener.
# 5. Restores .telemetry.cfg.
# 6. Compares raw_capture.bin (hook output) vs scout_from_listener.bin (real
#    TCP output). They should match byte-for-byte from some offset.

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$ProbePort      = 17934
$TargetSeconds  = 8
$ListenerPython = (Get-Command python3 -ErrorAction SilentlyContinue)
if (-not $ListenerPython) { $ListenerPython = Get-Command python }
if (-not $ListenerPython) { throw "python not on PATH" }

$HookDll    = Join-Path $here 'build\air_profiler_probe.dll'
$InjectExe  = Join-Path $here 'build\profiler_inject.exe'
$TargetExe  = 'C:\Users\Joao\IdeaProjects\ddtank-client\out\production\ddtankclient\loadingDebug64\LoadingDebugAir.exe'
if (-not (Test-Path $TargetExe)) {
    $TargetExe = 'C:\Users\Joao\IdeaProjects\ddtank-client\out\production\ddtankclient\loading64\LoadingAir.exe'
}
foreach ($p in @($HookDll, $InjectExe, $TargetExe)) {
    if (-not (Test-Path $p)) { throw "missing: $p" }
}

$RawOut      = Join-Path $here 'raw_capture.bin'
$ListenerOut = Join-Path $here 'scout_from_listener.bin'
$ListenerLog = Join-Path $here 'listener.log'
Remove-Item $RawOut, $ListenerOut, $ListenerLog -ErrorAction SilentlyContinue

# Tell the hook DLL where to write through an env var (inherited by child).
$env:ANE_POC_OUT_PATH = $RawOut

# --- 1. telemetry.cfg ----------------------------------------------------
$Cfg       = Join-Path $env:USERPROFILE '.telemetry.cfg'
$CfgBackup = "$Cfg.poc-backup"
$HadCfg    = Test-Path $Cfg
if ($HadCfg) { Copy-Item $Cfg $CfgBackup -Force }
@(
    "TelemetryAddress=127.0.0.1:$ProbePort"
    "SamplerEnabled=1"
    "CPUCapture=1"
    "DisplayObjectCapture=0"
    "Stage3DCapture=0"
    "ScriptObjectAllocationTraces=0"
) | Out-File -FilePath $Cfg -Encoding ASCII -NoNewline:$false

# --- 2. listener ---------------------------------------------------------
$listener = Start-Process -FilePath $ListenerPython.Source `
    -ArgumentList @(
        (Join-Path $here '..\..\..\tests\rva-probe\tcp_listener.py'),
        '--port', $ProbePort,
        '--out',  $ListenerOut,
        '--idle-timeout', '3.0'
    ) `
    -RedirectStandardOutput $ListenerLog `
    -RedirectStandardError  "$ListenerLog.err" `
    -NoNewWindow -PassThru
Write-Host "[poc] listener PID=$($listener.Id)"
Start-Sleep -Milliseconds 500

# --- 3. injector ---------------------------------------------------------
try {
    $inject = Start-Process -FilePath $InjectExe `
        -ArgumentList @($HookDll, $TargetExe) `
        -NoNewWindow -PassThru

    Write-Host "[poc] inject PID=$($inject.Id)  target=$TargetExe"

    # Let the target run to emit telemetry.
    if (-not $inject.WaitForExit($TargetSeconds * 1000)) {
        Write-Host "[poc] $TargetSeconds s elapsed — killing injector+target"
        # Inject waits INFINITE on the target, so killing inject doesn't kill
        # the target. We need to kill the target by name.
        Get-Process -Name 'LoadingDebugAir', 'LoadingAir', 'CaptiveAppEntry' `
            -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        if (-not $inject.HasExited) {
            Stop-Process -Id $inject.Id -Force -ErrorAction SilentlyContinue
        }
    }
}
finally {
    # --- 4. cleanup ------------------------------------------------------
    Start-Sleep -Milliseconds 800  # let DLL_PROCESS_DETACH flush
    if (-not $listener.HasExited) { Stop-Process -Id $listener.Id -Force -ErrorAction SilentlyContinue }
    if ($HadCfg) { Move-Item $CfgBackup $Cfg -Force }
    else         { Remove-Item $Cfg -ErrorAction SilentlyContinue }

    # --- 5. diff/summary ------------------------------------------------
    Write-Host "`n==== POC SUMMARY ===="
    $rawSize = if (Test-Path $RawOut)      { (Get-Item $RawOut).Length }      else { 0 }
    $lstSize = if (Test-Path $ListenerOut) { (Get-Item $ListenerOut).Length } else { 0 }
    Write-Host "hook output  (raw_capture.bin):       $rawSize bytes"
    Write-Host "listener    (scout_from_listener):   $lstSize bytes"

    if ($rawSize -gt 0 -and $lstSize -gt 0) {
        # They should be byte-identical from the point the hook patched.
        # Listener has the full stream; hook may miss the leading bytes
        # before the patch landed. Measure the skew.
        $raw = [System.IO.File]::ReadAllBytes($RawOut)
        $lst = [System.IO.File]::ReadAllBytes($ListenerOut)

        $skew = $lst.Length - $raw.Length
        Write-Host "listener leads by $skew bytes (bytes we missed before patch)"

        if ($skew -ge 0 -and $skew -lt $lst.Length) {
            # Check equality from the skew point
            $eq = $true
            for ($i = 0; $i -lt $raw.Length; $i++) {
                if ($raw[$i] -ne $lst[$skew + $i]) { $eq = $false; break }
            }
            if ($eq) {
                Write-Host "BYTE-MATCH: hook output == listener[skew..end]  -> hook is faithful"
            } else {
                Write-Host "MISMATCH at position $i"
            }
        }
    }
    Write-Host "====================="
    Remove-Item env:ANE_POC_OUT_PATH -ErrorAction SilentlyContinue
}
