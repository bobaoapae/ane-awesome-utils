# RVA validation probe orchestrator.
#
# Run with: pwsh -File .\run_probe.ps1
#
# What it does:
#   1. Writes $env:USERPROFILE\.telemetry.cfg pointing to 127.0.0.1:17934
#      (backing up any existing file).
#   2. Starts a Python TCP listener on 17934 in a background job.
#   3. Launches cdb.exe attached to a packaged AIR captive app (loading64 or
#      loadingDebug64 from ddtank-client output). CDB runs probe.cdb which
#      sets a BP on Adobe AIR!+0x493060 and logs each hit.
#   4. Waits for CDB to exit (script stops after 20 hits, or hard timeout 30s).
#   5. Restores the .telemetry.cfg, stops the listener, and prints a short
#      summary of what was captured.

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
if (-not $here) { $here = Split-Path -Parent $MyInvocation.MyCommand.Path }

$ProbePort = 17934
$CdbExe    = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
$Python    = Get-Command python3 -ErrorAction SilentlyContinue
if (-not $Python) { $Python = Get-Command python -ErrorAction SilentlyContinue }
if (-not $Python) { throw "python interpreter not found on PATH" }
$PythonExe = $Python.Source

# Default target: ddtank-client debug loading (PACKAGED, captive runtime).
# SHA256 matches our RVAs at the required offsets (verified offline).
$TargetExe = 'C:\Users\Joao\IdeaProjects\ddtank-client\out\production\ddtankclient\loadingDebug64\LoadingDebugAir.exe'
if (-not (Test-Path $TargetExe)) {
    $TargetExe = 'C:\Users\Joao\IdeaProjects\ddtank-client\out\production\ddtankclient\loading64\LoadingAir.exe'
}
if (-not (Test-Path $TargetExe)) {
    throw "No packaged AIR target found. Build via AirBuildTool first."
}

Write-Host "[probe] target: $TargetExe"
Write-Host "[probe] cdb:    $CdbExe"

# ---- 1. telemetry.cfg ---------------------------------------------------
$Cfg       = Join-Path $env:USERPROFILE '.telemetry.cfg'
$CfgBackup = "$Cfg.probe-backup"
$HadCfg    = Test-Path $Cfg
if ($HadCfg) {
    Copy-Item $Cfg $CfgBackup -Force
    Write-Host "[probe] backed up existing .telemetry.cfg to $CfgBackup"
}
@(
    "TelemetryAddress=127.0.0.1:$ProbePort"
    "SamplerEnabled=1"
    "CPUCapture=1"
    "DisplayObjectCapture=0"
    "Stage3DCapture=0"
    "ScriptObjectAllocationTraces=0"
) | Out-File -FilePath $Cfg -Encoding ASCII -NoNewline:$false
Write-Host "[probe] wrote $Cfg"

# ---- 2. listener --------------------------------------------------------
$ListenerLog = Join-Path $here 'listener.log'
$ListenerOut = Join-Path $here 'scout_bytes.bin'
Remove-Item $ListenerOut -ErrorAction SilentlyContinue
Remove-Item (Join-Path $here 'probe.log') -ErrorAction SilentlyContinue

$listener = Start-Process -FilePath $PythonExe `
    -ArgumentList @(
        (Join-Path $here 'tcp_listener.py'),
        '--port', $ProbePort,
        '--out',  $ListenerOut,
        '--idle-timeout', '3.0'
    ) `
    -RedirectStandardOutput $ListenerLog `
    -RedirectStandardError  "$ListenerLog.err" `
    -NoNewWindow -PassThru
Write-Host "[probe] listener PID=$($listener.Id)"
Start-Sleep -Milliseconds 500  # give it time to bind

# ---- 3. cdb -------------------------------------------------------------
try {
    $cdbScript = Join-Path $here 'probe.cdb'
    $cdbArgs = @(
        '-G',                             # ignore the last exit breakpoint
        '-o',                             # debug child processes too
        '-cf', $cdbScript,                # execute our CDB script at first prompt
        $TargetExe
    )
    Write-Host "[probe] launching: $CdbExe $($cdbArgs -join ' ')"
    $cdb = Start-Process -FilePath $CdbExe -ArgumentList $cdbArgs -NoNewWindow -PassThru
    Write-Host "[probe] cdb PID=$($cdb.Id)"

    $timeoutS = 30
    if (-not $cdb.WaitForExit($timeoutS * 1000)) {
        Write-Warning "[probe] cdb did not exit in ${timeoutS}s, killing"
        Stop-Process -Id $cdb.Id -Force -ErrorAction SilentlyContinue
    }
    Write-Host "[probe] cdb exited with code $($cdb.ExitCode)"
}
finally {
    # ---- 4. cleanup -----------------------------------------------------
    Start-Sleep -Milliseconds 500
    if (-not $listener.HasExited) {
        Stop-Process -Id $listener.Id -Force -ErrorAction SilentlyContinue
    }

    # Restore .telemetry.cfg
    if ($HadCfg) {
        Move-Item $CfgBackup $Cfg -Force
        Write-Host "[probe] restored original .telemetry.cfg"
    } else {
        Remove-Item $Cfg -ErrorAction SilentlyContinue
        Write-Host "[probe] removed temporary .telemetry.cfg"
    }

    # ---- 5. summary -----------------------------------------------------
    Write-Host "`n==== PROBE SUMMARY ===="
    $probeLog = Join-Path $here 'probe.log'
    if (Test-Path $probeLog) {
        $hits = (Select-String -Path $probeLog -Pattern '^\[hit' | Measure-Object).Count
        Write-Host "CDB hits in send_bytes: $hits"
        Write-Host "First CDB log lines:"
        Get-Content $probeLog -TotalCount 40 | ForEach-Object { "  $_" } | Write-Host
    } else {
        Write-Host "CDB log NOT created (probe.log missing)"
    }
    if (Test-Path $ListenerOut) {
        $bytes = (Get-Item $ListenerOut).Length
        Write-Host "Listener captured: $bytes bytes in $ListenerOut"
    } else {
        Write-Host "Listener captured NO bytes"
    }
    Write-Host "=======================`n"
}
