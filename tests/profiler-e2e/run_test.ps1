# Automated E2E test harness for the .aneprof profiler subsystem.
#
#   pwsh run_test.ps1                 # normal run: scenarios A + B + C + E + L
#   pwsh run_test.ps1 -Rebuild        # force rebuild
#   pwsh run_test.ps1 -SkipBuild      # launch + inspect only
#   pwsh run_test.ps1 -WithKillTest   # additionally run scenario D with kill
#   pwsh run_test.ps1 -KeepOutputs    # don't cleanup storage after pass
#
# The test app (TestProfilerApp.as) runs 5 normal scenarios, plus D with -WithKillTest:
#   A: short, timing-only
#   B: second capture in the same process (restart test)
#   C: longer, adds memory hook + periodic snapshots
#   E: hidden listener leak, objects removed from display but retained by a
#      strong listener on a long-lived dispatcher
#   L: memory hook + intentional retention, compared against C by analyzer
#   D: (only with -WithKillTest) captures 500 frames but never calls Stop
#      - harness force-terminates the process mid-capture to verify the
#      .aneprof has at least a valid header.

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
    $nativeBuildDir = if ($Arch -eq 'x86') { 'cmake-build-release-x32' } else { 'cmake-build-release-x64' }
    $winDll  = Join-Path $root "WindowsNative\$nativeBuildDir\AneAwesomeUtilsWindows.dll"

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
Get-ChildItem $storageDir -Filter "test_capture_*.aneprof" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
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
    $startArgs = @{
        FilePath = $exe
        RedirectStandardOutput = $stdoutLog
        RedirectStandardError = $stderrLog
        NoNewWindow = $true
        PassThru = $true
    }
    $p = Start-Process @startArgs
    if ($killMidway) {
        Start-Sleep -Seconds $killAfterSec
        if (-not $p.HasExited) {
            Write-Host "[harness] force-killing PID $($p.Id) after $killAfterSec s"
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        }
    }
    $waitMs = $timeoutSec * 1000
    $exited = $p.WaitForExit($waitMs)
    if (-not $exited) {
        Write-Warning "[harness] timeout - terminating"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }
    return $p.ExitCode
}

$validatePy = Join-Path $cliDir 'aneprof_validate.py'
$analyzePy  = Join-Path $cliDir 'aneprof_analyze.py'

function Summarise-Capture([string]$capturePath, [bool]$expectFooter = $true) {
    $summary = [ordered]@{ path=$capturePath; present=(Test-Path $capturePath) }
    if (-not $summary.present) { return $summary }
    $summary.size = (Get-Item $capturePath).Length
    Write-Host "[harness] validating $capturePath"
    if (-not $expectFooter) {
        $bytes = [System.IO.File]::ReadAllBytes($capturePath)
        $summary.validateExit = 0
        $summary.validateOutput = "partial capture: skipped footer validator"
        $summary.headerMagicOk = (
            $bytes.Length -ge 8 -and
            $bytes[0] -eq 0x41 -and
            $bytes[1] -eq 0x4e -and
            $bytes[2] -eq 0x45 -and
            $bytes[3] -eq 0x50
        )
        return $summary
    }
    $raw = & python $validatePy $capturePath 2>&1
    $summary.validateExit = $LASTEXITCODE
    $summary.validateOutput = ($raw -join "`n")
    $raw | ForEach-Object { Write-Host "    $_" }
    return $summary
}

function Analyze-Capture([string]$capturePath, [string]$name) {
    $jsonPath = Join-Path $outDir "analysis_$name.json"
    Remove-Item $jsonPath -ErrorAction SilentlyContinue
    Write-Host "[harness] analyzing $capturePath"
    $raw = & python $analyzePy $capturePath --require-free-events --json $jsonPath 2>&1
    $exit = $LASTEXITCODE
    $raw | ForEach-Object { Write-Host "    $_" }
    $result = $null
    if (Test-Path $jsonPath) {
        $result = Get-Content $jsonPath -Raw | ConvertFrom-Json
    }
    return [ordered]@{
        exit = $exit
        output = ($raw -join "`n")
        jsonPath = $jsonPath
        result = $result
    }
}

# --------------------------------------------------------------------------
# Run #1 - scenarios A + B + C + E + L in one process.
# --------------------------------------------------------------------------
Write-Host "`n======== RUN 1: scenarios A + B + C + E + L ========`n"
$exit1 = Invoke-TestApp -timeoutSec $TimeoutSec
Write-Host "[harness] run 1 exit code = $exit1"

$result1 = $null
$resultPath = Join-Path $storageDir 'test_result.json'
if (Test-Path $resultPath) {
    $result1 = Get-Content $resultPath -Raw | ConvertFrom-Json
}

$cap = @{}
foreach ($name in @("A", "B", "C", "E", "L")) {
    $capture = Join-Path $storageDir "test_capture_$name.aneprof"
    $cap[$name] = Summarise-Capture -capturePath $capture -expectFooter $true
    if ($name -eq "C" -or $name -eq "E" -or $name -eq "L") {
        $cap[$name]["analysis"] = Analyze-Capture -capturePath $capture -name $name
    }
}

# --------------------------------------------------------------------------
# Run #2 - scenario D (kill mid-capture). Optional.
# --------------------------------------------------------------------------
$exit2 = $null
if ($WithKillTest) {
    Write-Host "`n======== RUN 2: scenario D (kill mid-capture) ========`n"
    # Signal to the test app that it should add scenario D.
    New-Item -ItemType File -Force -Path (Join-Path $storageDir 'ANE_TEST_KILL') | Out-Null
    $exit2 = Invoke-TestApp -timeoutSec 30 -killMidway $true -killAfterSec 12
    Write-Host "[harness] run 2 exit code = $exit2"

    $captureD = Join-Path $storageDir 'test_capture_D.aneprof'
    $cap["D"] = Summarise-Capture -capturePath $captureD -expectFooter $false
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
$run1Ok = ($exit1 -eq 0) -or ($null -eq $exit1 -and $result1 -and $result1.allOk)
if (-not $run1Ok) { $allPass = $false }

foreach ($name in @("A", "B", "C", "E", "L")) {
    $s = $cap[$name]
    $ok = $s.present -and $s.validateExit -eq 0 -and $s.size -gt 128
    if ($name -eq "C" -or $name -eq "E" -or $name -eq "L") {
        $ok = $ok -and $s.analysis -and $s.analysis.exit -eq 0
        if ($s.analysis -and $s.analysis.result) {
            $as3Allocs = [double]$s.analysis.result.counts.as3_alloc
            $as3ReferenceEvents = [double]$s.analysis.result.counts.as3_reference
            $as3LiveOwnerRefs = [double]$s.analysis.result.as3_reference_edges_with_live_owner
            $as3Types = @($s.analysis.result.top_as3_allocation_types)
            $as3Stacks = @($s.analysis.result.top_as3_live_stacks)
            $hasAs3Stack = $false
            foreach ($stackItem in $as3Stacks) {
                if ($stackItem.stack -and -not $stackItem.stack.StartsWith('<no stack>')) {
                    $hasAs3Stack = $true
                    break
                }
            }
            $ok = $ok -and ($as3Allocs -gt 0) -and ($as3ReferenceEvents -gt 0) -and
                ($as3LiveOwnerRefs -gt 0) -and ($as3Types.Count -gt 0) -and $hasAs3Stack
        }
    }
    if (-not $ok) { $allPass = $false }
    Write-Host ("  Scenario {0}: present={1,-5} size={2,-7} validate={3}  {4}" -f `
        $name, $s.present, $s.size, $s.validateExit, $(if ($ok) {"OK"} else {"FAIL"}))
}

if ($cap["C"].analysis -and $cap["E"].analysis -and
    $cap["C"].analysis.result -and $cap["E"].analysis.result) {
    $baseAs3LiveCount = [double]$cap["C"].analysis.result.as3_live_allocations
    $eventAs3LiveCount = [double]$cap["E"].analysis.result.as3_live_allocations
    $deltaEventAs3LiveCount = $eventAs3LiveCount - $baseAs3LiveCount
    $baseAs3LiveBytes = [double]$cap["C"].analysis.result.as3_live_bytes
    $eventAs3LiveBytes = [double]$cap["E"].analysis.result.as3_live_bytes
    $deltaEventAs3LiveBytes = $eventAs3LiveBytes - $baseAs3LiveBytes

    $hiddenTypeSeen = $false
    foreach ($typeItem in @($cap["E"].analysis.result.top_as3_live_types)) {
        if ($typeItem.type_name -like "*HiddenListenerLeak*") {
            $hiddenTypeSeen = $true
            break
        }
    }

    $hiddenStackSeen = $false
    foreach ($stackItem in @($cap["E"].analysis.result.top_as3_live_stacks)) {
        if (($stackItem.stack -like "*HiddenListenerLeak*") -or
            ($stackItem.stack -like "*createHiddenListenerLeak*")) {
            $hiddenStackSeen = $true
            break
        }
    }

    $hiddenSuspectSeen = $false
    foreach ($suspect in @($cap["E"].analysis.result.leak_suspects)) {
        if (($suspect.site -like "*HiddenListenerLeak*") -or
            ($suspect.site -like "*createHiddenListenerLeak*") -or
            ($suspect.sample_stack -like "*HiddenListenerLeak*")) {
            $hiddenSuspectSeen = $true
            break
        }
    }

    $listenerLeakDetected = (
        ($deltaEventAs3LiveCount -ge 128 -or $deltaEventAs3LiveBytes -ge 8192) -and
        $hiddenTypeSeen -and
        $hiddenStackSeen -and
        $hiddenSuspectSeen
    )
    if (-not $listenerLeakDetected) { $allPass = $false }
    Write-Host ("  Listener leak C->E: AS3 count {0} -> {1} (delta={2}), bytes {3} -> {4} (delta={5}), type={6}, stack={7}, suspect={8} {9}" -f `
        $baseAs3LiveCount, $eventAs3LiveCount, $deltaEventAs3LiveCount,
        $baseAs3LiveBytes, $eventAs3LiveBytes, $deltaEventAs3LiveBytes,
        $hiddenTypeSeen, $hiddenStackSeen, $hiddenSuspectSeen,
        $(if ($listenerLeakDetected) {"OK"} else {"FAIL"}))
} else {
    $allPass = $false
    Write-Host "  Listener leak C->E: missing analyzer JSON FAIL"
}

if ($cap["C"].analysis -and $cap["L"].analysis -and
    $cap["C"].analysis.result -and $cap["L"].analysis.result) {
    $baseLiveBytes = [double]$cap["C"].analysis.result.live_bytes
    $leakLiveBytes = [double]$cap["L"].analysis.result.live_bytes
    $deltaLiveBytes = $leakLiveBytes - $baseLiveBytes
    $baseLiveCount = [double]$cap["C"].analysis.result.live_allocations
    $leakLiveCount = [double]$cap["L"].analysis.result.live_allocations
    $deltaLiveCount = $leakLiveCount - $baseLiveCount
    $leakDetected = ($deltaLiveBytes -ge 262144) -or ($deltaLiveCount -ge 64)
    $baseAs3LiveCount = [double]$cap["C"].analysis.result.as3_live_allocations
    $leakAs3LiveCount = [double]$cap["L"].analysis.result.as3_live_allocations
    $deltaAs3LiveCount = $leakAs3LiveCount - $baseAs3LiveCount
    $as3LeakDetected = ($deltaAs3LiveCount -ge 64) -or (@($cap["L"].analysis.result.top_as3_live_types).Count -gt 0)
    if (-not $leakDetected) { $allPass = $false }
    if (-not $as3LeakDetected) { $allPass = $false }
    Write-Host ("  Leak compare C->L: liveBytes {0} -> {1} (delta={2}), liveCount {3} -> {4} (delta={5}) {6}" -f `
        $baseLiveBytes, $leakLiveBytes, $deltaLiveBytes,
        $baseLiveCount, $leakLiveCount, $deltaLiveCount,
        $(if ($leakDetected) {"OK"} else {"FAIL"}))
    Write-Host ("  AS3 compare C->L : liveCount {0} -> {1} (delta={2}) {3}" -f `
        $baseAs3LiveCount, $leakAs3LiveCount, $deltaAs3LiveCount,
        $(if ($as3LeakDetected) {"OK"} else {"FAIL"}))

} else {
    $allPass = $false
    Write-Host "  Leak compare C->L: missing analyzer JSON FAIL"
}
if ($WithKillTest) {
    $sD = $cap["D"]
    # Kill-test: file must exist and start with ANEPROF magic. Footer is not
    # expected because the process is killed before profilerStop().
    $magicOk = $false
    if ($sD.present -and $sD.size -ge 8) {
        $bytes = [System.IO.File]::ReadAllBytes($sD.path)
        $magicOk = ($bytes[0] -eq 0x41 -and $bytes[1] -eq 0x4e -and
                    $bytes[2] -eq 0x45 -and $bytes[3] -eq 0x50)
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
            Write-Host ("  [{0}] frames={1}/{2} events={3} payloadBytes={4} dropped={5} memory={6}" -f `
                $r.scenario, $r.framesRan, $r.targetFrames,
                $r.postStop.events, $r.postStop.payloadBytes,
                $r.postStop.dropped, $r.preStop.memoryEnabled)
        }
    }
}

if (-not $KeepOutputs -and $allPass) {
    Get-ChildItem $storageDir -Filter "test_capture_*.aneprof" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
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
