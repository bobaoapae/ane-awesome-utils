# Automated E2E test harness for the Scout profiler subsystem.
#
#   pwsh run_test.ps1                 # normal run: scenarios A + B + C
#   pwsh run_test.ps1 -Rebuild        # force rebuild
#   pwsh run_test.ps1 -SkipBuild      # launch + inspect only
#   pwsh run_test.ps1 -WithKillTest   # additionally run scenario D with kill
#   pwsh run_test.ps1 -KeepOutputs    # don't cleanup storage after pass
#
# The test app (TestProfilerApp.as) runs 3-4 scenarios:
#   A: short, mode-B with sampler+cpu
#   B: second capture in the same process (restart test)
#   C: longer, adds displayObjectCapture
#   D: (only with -WithKillTest) captures 500 frames but never calls Stop
#      — harness force-terminates the process mid-capture to verify the
#      .flmc is still at least parseable to its last whole record.

[CmdletBinding()]
param(
    [int]    $TimeoutSec = 60,
    [ValidateSet('x64', 'x86')][string] $Arch = 'x64',
    [switch] $Rebuild,
    [switch] $SkipBuild,
    [switch] $KeepOutputs,
    [switch] $WithKillTest
)

$ErrorActionPreference = 'Stop'
$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$root   = Resolve-Path (Join-Path $here '..\..')
$appDir = Join-Path $here 'app'
$outDir = Join-Path $here 'output'
$cliDir = Join-Path $root 'tools\profiler-cli'

Write-Host "[harness] root    : $root"
Write-Host "[harness] app dir : $appDir"
Write-Host "[harness] out dir : $outDir"

# ---- 1. Make sure ANE + SWC are fresh -----------------------------------
if (-not $SkipBuild) {
    if ($Rebuild) {
        Remove-Item -Recurse -Force $outDir -ErrorAction SilentlyContinue
    }

    $anePath = Join-Path $root 'AneBuild\br.com.redesurftank.aneawesomeutils.ane'
    $swcPath = Join-Path $root 'AneBuild\library.swc'
    $winDll  = Join-Path $root 'WindowsNative\cmake-build-release-x64\AneAwesomeUtilsWindows.dll'

    if (-not (Test-Path $winDll) -or $Rebuild) {
        Write-Host "[harness] (re)building Windows native"
        Push-Location $root
        python build-all.py windows-native
        if ($LASTEXITCODE -ne 0) { throw "windows-native build failed" }
        Pop-Location
    }

    if (-not (Test-Path $swcPath) -or $Rebuild) {
        Write-Host "[harness] (re)building AS3 SWC"
        Push-Location $root
        python build-all.py as3
        if ($LASTEXITCODE -ne 0) { throw "as3 build failed" }
        Pop-Location
    }

    if (-not (Test-Path $anePath) -or $Rebuild) {
        Write-Host "[harness] (re)packaging ANE"
        Push-Location $root
        python build-all.py dotnet-win
        if ($LASTEXITCODE -ne 0) { throw "dotnet-win build failed" }
        python build-all.py package
        if ($LASTEXITCODE -ne 0) { throw "package step failed" }
        Pop-Location
    }

    Write-Host "[harness] building test app (arch=$Arch)"
    $buildBat = Join-Path $appDir 'build.bat'
    $buildLog = & cmd.exe /c "`"$buildBat`" $Arch" 2>&1
    $buildExit = $LASTEXITCODE
    $buildLog | ForEach-Object { Write-Host "  [build] $_" }
    if ($buildExit -ne 0) { throw "app build.bat failed" }
}

$exe = Join-Path $outDir 'TestProfilerApp\TestProfilerApp.exe'
if (-not (Test-Path $exe)) { throw "packaged exe not found: $exe" }

$storageDir = Join-Path $env:APPDATA 'br.com.redesurftank.testprofiler\Local Store'
Write-Host "[harness] storage : $storageDir"
New-Item -ItemType Directory -Force -Path $storageDir | Out-Null

# Clean storage outputs from prior runs.
Get-ChildItem $storageDir -Filter "test_capture_*.flmc"   -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
Get-ChildItem $storageDir -Filter "test_result*.json"     -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $storageDir 'ANE_TEST_KILL') -ErrorAction SilentlyContinue

$stdoutLog = Join-Path $outDir 'app_stdout.log'
$stderrLog = Join-Path $outDir 'app_stderr.log'
Remove-Item $stdoutLog, $stderrLog -ErrorAction SilentlyContinue

# --------------------------------------------------------------------------
# Helper: run the app once, wait for exit, return (exitCode, result.json)
# --------------------------------------------------------------------------
function Invoke-TestApp([int]$timeoutSec, [bool]$killMidway = $false, [int]$killAfterSec = 3) {
    Write-Host "[harness] launching $exe (killMidway=$killMidway)"
    $p = Start-Process -FilePath $exe `
        -RedirectStandardOutput $stdoutLog `
        -RedirectStandardError  $stderrLog `
        -NoNewWindow -PassThru
    if ($killMidway) {
        Start-Sleep -Seconds $killAfterSec
        if (-not $p.HasExited) {
            Write-Host "[harness] force-killing PID $($p.Id) after $killAfterSec s"
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        }
    }
    if (-not $p.WaitForExit($timeoutSec * 1000)) {
        Write-Warning "[harness] timeout — terminating"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }
    return $p.ExitCode
}

$validatePy = Join-Path $cliDir 'flmc_validate.py'

function Summarise-Capture([string]$flmcPath, [bool]$expectFooter = $true) {
    $summary = [ordered]@{ path=$flmcPath; present=(Test-Path $flmcPath) }
    if (-not $summary.present) { return $summary }
    $summary.size = (Get-Item $flmcPath).Length
    Write-Host "[harness] validating $flmcPath"
    $extraArgs = @()
    if (-not $expectFooter) { $extraArgs += '--allow-partial' }
    $raw = & python $validatePy $flmcPath @extraArgs 2>&1
    $summary.validateExit = $LASTEXITCODE
    $summary.validateOutput = ($raw -join "`n")
    $raw | ForEach-Object { Write-Host "    $_" }
    return $summary
}

# --------------------------------------------------------------------------
# Run #1 — scenarios A + B + C in one process.
# --------------------------------------------------------------------------
Write-Host "`n======== RUN 1: scenarios A + B + C ========`n"
$exit1 = Invoke-TestApp -timeoutSec $TimeoutSec
Write-Host "[harness] run 1 exit code = $exit1"

$result1 = $null
$resultPath = Join-Path $storageDir 'test_result.json'
if (Test-Path $resultPath) {
    $result1 = Get-Content $resultPath -Raw | ConvertFrom-Json
}

$cap = @{}
foreach ($name in @("A", "B", "C")) {
    $flmc = Join-Path $storageDir "test_capture_$name.flmc"
    $cap[$name] = Summarise-Capture -flmcPath $flmc -expectFooter $true
}

# --------------------------------------------------------------------------
# Run #2 — scenario D (kill mid-capture). Optional.
# --------------------------------------------------------------------------
$exit2 = $null
if ($WithKillTest) {
    Write-Host "`n======== RUN 2: scenario D (kill mid-capture) ========`n"
    # Signal to the test app that it should add scenario D.
    New-Item -ItemType File -Force -Path (Join-Path $storageDir 'ANE_TEST_KILL') | Out-Null
    $exit2 = Invoke-TestApp -timeoutSec 30 -killMidway $true -killAfterSec 12
    Write-Host "[harness] run 2 exit code = $exit2"

    $flmcD = Join-Path $storageDir 'test_capture_D.flmc'
    $cap["D"] = Summarise-Capture -flmcPath $flmcD -expectFooter $false
    Remove-Item (Join-Path $storageDir 'ANE_TEST_KILL') -ErrorAction SilentlyContinue
}

# --------------------------------------------------------------------------
# Verdict.
# --------------------------------------------------------------------------
Write-Host ""
Write-Host "================ E2E VERDICT ================"
Write-Host "Run 1 exit           : $exit1"
if ($WithKillTest) { Write-Host "Run 2 exit (killed)  : $exit2" }

$allPass = $true
if ($exit1 -ne 0) { $allPass = $false }

foreach ($name in @("A", "B", "C")) {
    $s = $cap[$name]
    $ok = $s.present -and $s.validateExit -eq 0 -and $s.size -gt 128
    if (-not $ok) { $allPass = $false }
    Write-Host ("  Scenario {0}: present={1,-5} size={2,-7} validate={3}  {4}" -f `
        $name, $s.present, $s.size, $s.validateExit, $(if ($ok) {"OK"} else {"FAIL"}))
}
if ($WithKillTest) {
    $sD = $cap["D"]
    # Kill-test: file must exist, start with FLMC magic, inflate cleanly via
    # the partial-mode validator (exit 0 with --allow-partial).
    $magicOk = $false
    if ($sD.present -and $sD.size -ge 12) {
        $bytes = [System.IO.File]::ReadAllBytes($sD.path)
        $magicOk = ($bytes[0] -eq 0x46 -and $bytes[1] -eq 0x4c -and
                    $bytes[2] -eq 0x4d -and $bytes[3] -eq 0x43)
    }
    $okD = $sD.present -and $sD.size -gt 128 -and $magicOk -and $sD.validateExit -eq 0
    if (-not $okD) { $allPass = $false }
    Write-Host ("  Scenario D: present={0,-5} size={1,-7} magic={2} validate={3} {4}" -f `
        $sD.present, $sD.size, $magicOk, $sD.validateExit,
        $(if ($okD) {"OK-partial"} else {"FAIL"}))
}

if ($result1) {
    Write-Host "`nScenario detail from test_result.json:"
    foreach ($r in $result1.scenarios) {
        if ($r.failed) {
            Write-Host "  [$($r.scenario)] FAILED: $($r.reason)"
        } else {
            Write-Host ("  [{0}] frames={1}/{2} records={3} bytesIn={4} bytesOut={5} drops={6} modeBActive={7}" -f `
                $r.scenario, $r.framesRan, $r.targetFrames,
                $r.postStop.records, $r.postStop.bytesIn, $r.postStop.bytesOut,
                $r.postStop.drops, $r.preStop.modeBActive)
        }
    }
}

if (-not $KeepOutputs -and $allPass) {
    Get-ChildItem $storageDir -Filter "test_capture_*.flmc" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $storageDir 'test_result.json') -ErrorAction SilentlyContinue
}

if ($allPass) {
    Write-Host "`n============ PASS ============" -ForegroundColor Green
    exit 0
} else {
    Write-Host "`n============ FAIL ============" -ForegroundColor Red
    Write-Host "`n--- stdout ---"
    if (Test-Path $stdoutLog) { Get-Content $stdoutLog }
    Write-Host "`n--- stderr ---"
    if (Test-Path $stderrLog) { Get-Content $stderrLog }
    exit 1
}
