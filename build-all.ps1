<#
.SYNOPSIS
    Full build pipeline for ane-awesome-utils ANE.
    Builds all platforms (Windows, macOS, iOS, Android) and packages the final .ane.

.DESCRIPTION
    Steps:
    1. Sync code to Mac (git push + pull via SSH)
    2. Build .NET NativeAOT on Mac (macOS dylib + iOS static lib)
    3. Build Xcode on Mac (macOS framework + iOS static lib)
    4. Copy Mac/iOS binaries back to Windows via SCP
    5. Build Windows native C++ (CMake + Ninja + VS2017)
    6. Build .NET for Windows (dotnet publish x86 + x64)
    7. Build Android (Gradle assembleDebug)
    8. Compile AS3 to SWC (AS3CompilerCLI)
    9. Package ANE (ADT)

.PARAMETER Step
    Run a specific step only. Values: sync, dotnet-mac, xcode, copy-mac, windows-native, dotnet-win, android, as3, package, all
    Default: all

.PARAMETER Step
    Run a specific step only.

.EXAMPLE
    .\build-all.ps1                          # Full build
    .\build-all.ps1 -Step windows-native     # Only Windows C++ build
    .\build-all.ps1 -Step package            # Only package step
#>

param(
    [ValidateSet("sync","dotnet-mac","xcode","copy-mac","windows-native","dotnet-win","android","as3","package","all")]
    [string]$Step = "all"
)

$ErrorActionPreference = "Stop"
$ProjectRoot = $PSScriptRoot
$AneBuild = Join-Path $ProjectRoot "AneBuild"

# ── Load .env ────────────────────────────────────────────────────────────
$envFile = Join-Path $ProjectRoot ".env"
if (-not (Test-Path $envFile)) {
    throw ".env file not found at $envFile. Create it from .env.example."
}
Get-Content $envFile | ForEach-Object {
    $line = $_.Trim()
    if ($line -and -not $line.StartsWith("#")) {
        $parts = $line -split "=", 2
        if ($parts.Count -eq 2) {
            Set-Variable -Name $parts[0].Trim() -Value $parts[1].Trim() -Scope Script
        }
    }
}

# ── Validate required env vars ───────────────────────────────────────────
foreach ($var in @("MAC_HOST", "MAC_KEYCHAIN_PASS", "AIRSDK_PATH", "SIGNING_IDENTITY", "WIN_SIGN_NAME")) {
    if (-not (Get-Variable -Name $var -ValueOnly -ErrorAction SilentlyContinue)) {
        throw "Missing required variable '$var' in .env"
    }
}

$MacHost         = $MAC_HOST
$MacKeychainPass = $MAC_KEYCHAIN_PASS
$AirSdkPath      = $AIRSDK_PATH
$SigningIdentity  = $SIGNING_IDENTITY
$WinSignName      = $WIN_SIGN_NAME
$MacProjectPath   = "~/IdeaProjects/ane-awesome-utils"

# ── Paths ────────────────────────────────────────────────────────────────
$VS2017VcVars  = "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvarsall.bat"
$Ninja         = "C:\Users\Joao\AppData\Local\Programs\CLion\bin\ninja\win\x64\ninja.exe"
$CMake         = "C:\Users\Joao\AppData\Local\Programs\CLion\bin\cmake\win\x64\bin\cmake.exe"
$SevenZip      = "C:\Program Files\7-Zip\7z.exe"
$AS3Compiler   = Join-Path $AirSdkPath "as3Compiler\publish\AS3CompilerCLI.exe"
$AdtJar        = Join-Path $AirSdkPath "lib\adt.jar"
$AirSdkInclude = Join-Path $AirSdkPath "include"

# Mac dotnet needs explicit PATH
$MacDotnetPath = "/usr/local/share/dotnet"
$MacSshPrefix  = "export PATH=`$PATH:$MacDotnetPath;"

# Xcode build paths on Mac
$MacXcodeProject    = "$MacProjectPath/AppleNative/AneAwesomeUtils/AneAwesomeUtils.xcodeproj"
$MacDerivedData     = "$MacProjectPath/AppleNative/AneAwesomeUtils/DerivedData"
$MacCSharpProject   = "$MacProjectPath/CSharpLibrary/AwesomeAneUtils/AwesomeAneUtils/AwesomeAneUtils.csproj"
$MacCSharpOutputDir = "$MacProjectPath/CSharpLibrary/AwesomeAneUtils/AwesomeAneUtils/bin/Release/net10.0"

function Write-StepHeader($msg) {
    Write-Host ""
    Write-Host ("=" * 70) -ForegroundColor Cyan
    Write-Host "  $msg" -ForegroundColor Cyan
    Write-Host ("=" * 70) -ForegroundColor Cyan
}

function Invoke-SSH($command) {
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $result = ssh -o BatchMode=yes -o ConnectTimeout=10 $MacHost "$MacSshPrefix $command" 2>&1 | Out-String
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $prevEAP
    if ($exitCode -ne 0) {
        Write-Host "SSH command failed (exit $exitCode):" -ForegroundColor Red
        Write-Host $result -ForegroundColor Red
        throw "SSH command failed: $command"
    }
    return $result.Trim()
}

function Invoke-SCP($src, $dst) {
    $prevEAP = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    scp -o BatchMode=yes -o ConnectTimeout=10 $src $dst 2>&1 | Out-String | Write-Host
    $ec = $LASTEXITCODE; $ErrorActionPreference = $prevEAP
    if ($ec -ne 0) { throw "SCP failed: $src -> $dst" }
}

function Invoke-SCPRecursive($src, $dst) {
    $prevEAP = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    scp -r -o BatchMode=yes -o ConnectTimeout=10 $src $dst 2>&1 | Out-String | Write-Host
    $ec = $LASTEXITCODE; $ErrorActionPreference = $prevEAP
    if ($ec -ne 0) { throw "SCP recursive failed: $src -> $dst" }
}

# ── Step 1: Sync code to Mac ─────────────────────────────────────────────
function Step-Sync {
    Write-StepHeader "Step 1: Syncing code to Mac via git"

    # Push local changes
    Write-Host "Pushing to origin..."
    $ErrorActionPreference = "Continue"
    git -C $ProjectRoot push 2>&1 | ForEach-Object { Write-Host $_ }
    $ErrorActionPreference = "Stop"

    # Hard reset + Pull on Mac (discard all local changes - build will regenerate all binaries)
    Write-Host "Pulling on Mac (hard reset to match remote)..."
    $pullResult = Invoke-SSH "cd $MacProjectPath && git reset --hard HEAD && git clean -fd && git pull"
    if ($pullResult) { Write-Host $pullResult }

    Write-Host "Sync complete." -ForegroundColor Green
}

# ── Step 2: Build .NET on Mac (NativeAOT for macOS + iOS) ────────────────
function Step-DotnetMac {
    Write-StepHeader "Step 2: Building .NET NativeAOT on Mac (macOS + iOS)"

    # Build macOS ARM64
    Write-Host "Building dotnet osx-arm64..."
    Invoke-SSH "cd $MacProjectPath/AneBuild && dotnet publish -c Release -r osx-arm64 -p:NativeLib=Shared $MacCSharpProject" | Write-Host

    # Build macOS x64
    Write-Host "Building dotnet osx-x64..."
    Invoke-SSH "cd $MacProjectPath/AneBuild && dotnet publish -c Release -r osx-x64 -p:NativeLib=Shared $MacCSharpProject" | Write-Host

    # Build iOS ARM64
    Write-Host "Building dotnet ios-arm64..."
    Invoke-SSH "cd $MacProjectPath/AneBuild && dotnet publish -c Release -r ios-arm64 -p:NativeLib=Static -p:PublishAotUsingRuntimePack=true $MacCSharpProject" | Write-Host

    # Create universal macOS binary
    Write-Host "Creating macOS universal binary with lipo..."
    $arm64 = "$MacCSharpOutputDir/osx-arm64/native/AwesomeAneUtils.dylib"
    $x64   = "$MacCSharpOutputDir/osx-x64/native/AwesomeAneUtils.dylib"
    $universalDir = "$MacCSharpOutputDir/macos-universal"
    $universal = "$universalDir/AwesomeAneUtils.dylib"

    Invoke-SSH "mkdir -p $universalDir && lipo -create -output $universal $arm64 $x64" | Write-Host

    # Fix install name
    Invoke-SSH "install_name_tool -id '@loader_path/Frameworks/AwesomeAneUtils.dylib' $universal" | Write-Host

    # Unlock keychain + Sign macOS dylib
    Write-Host "Signing macOS dylib..."
    $unlockCmd = ""
    if ($MacKeychainPass) {
        $unlockCmd = "security unlock-keychain -p '$MacKeychainPass' ~/Library/Keychains/login.keychain-db && "
    }
    Invoke-SSH "${unlockCmd}codesign --force --sign '$SigningIdentity' --options=runtime --timestamp $universal" | Write-Host

    # Copy and sign iOS lib
    $iosArm64 = "$MacCSharpOutputDir/ios-arm64/native/AwesomeAneUtils.a"
    $iosDir   = "$MacCSharpOutputDir/ios-universal"
    $iosLib   = "$iosDir/AwesomeAneUtils.a"
    Invoke-SSH "mkdir -p $iosDir && cp $iosArm64 $iosLib && ${unlockCmd}codesign --force --sign '$SigningIdentity' --options=runtime --timestamp $iosLib" | Write-Host

    Write-Host ".NET Mac builds complete." -ForegroundColor Green
}

# ── Step 3: Build Xcode on Mac ──────────────────────────────────────────
function Step-Xcode {
    Write-StepHeader "Step 3: Building Xcode targets on Mac"

    # Unlock keychain for Xcode codesign
    $unlockCmd = ""
    if ($MacKeychainPass) {
        $unlockCmd = "security unlock-keychain -p '$MacKeychainPass' ~/Library/Keychains/login.keychain-db && "
    }

    # Build macOS framework
    Write-Host "Building macOS framework (AneAwesomeUtils)..."
    Invoke-SSH "${unlockCmd}cd $MacProjectPath/AppleNative/AneAwesomeUtils && xcodebuild -project AneAwesomeUtils.xcodeproj -scheme AneAwesomeUtils -configuration Debug -derivedDataPath DerivedData ONLY_ACTIVE_ARCH=NO 2>&1" | Write-Host

    # Build iOS static library
    Write-Host "Building iOS static library (AneAwesomeUtils-IOS)..."
    Invoke-SSH "${unlockCmd}cd $MacProjectPath/AppleNative/AneAwesomeUtils && xcodebuild -project AneAwesomeUtils.xcodeproj -scheme AneAwesomeUtils-IOS -configuration Debug -derivedDataPath DerivedData -sdk iphoneos ONLY_ACTIVE_ARCH=NO 2>&1" | Write-Host

    Write-Host "Xcode builds complete." -ForegroundColor Green
}

# ── Step 4: Copy Mac/iOS binaries to Windows ─────────────────────────────
function Step-CopyMac {
    Write-StepHeader "Step 4: Copying Mac/iOS binaries to Windows"

    $macBuildProducts = "$MacProjectPath/AppleNative/AneAwesomeUtils/DerivedData/AneAwesomeUtils/Build/Products"

    # Copy iOS static library
    Write-Host "Copying iOS library..."
    Invoke-SCP "${MacHost}:${macBuildProducts}/Debug-iphoneos/libAneAwesomeUtils-IOS.a" "$AneBuild\ios\libAneAwesomeUtils-IOS.a"

    # Delete old macOS framework
    $macosFramework = Join-Path $AneBuild "macos\AneAwesomeUtils.framework"
    if (Test-Path $macosFramework) {
        Remove-Item -Recurse -Force $macosFramework
    }

    # Copy macOS framework
    Write-Host "Copying macOS framework..."
    Invoke-SCPRecursive "${MacHost}:${macBuildProducts}/Debug/AneAwesomeUtils.framework/Versions/Current" "$macosFramework"

    # Copy iOS NativeCsharp .a/.o files from dotnet build
    Write-Host "Copying iOS NativeCsharp runtime libraries..."
    $iosNativeCsharpDir = Join-Path $AneBuild "ios\NativeCsharp"
    $macIosPublishDir = "$MacCSharpOutputDir/ios-arm64/native"
    # Use find to avoid zsh glob errors
    $fileList = Invoke-SSH "find $macIosPublishDir -maxdepth 1 \( -name '*.a' -o -name '*.o' \) -exec basename {} \;"
    $fileList -split "`n" | ForEach-Object {
        $filename = $_.Trim()
        if ($filename -and $filename -ne "" -and $filename -match '\.(a|o)$') {
            Write-Host "  Copying $filename..."
            Invoke-SCP "${MacHost}:${macIosPublishDir}/${filename}" "$iosNativeCsharpDir\$filename"
        }
    }

    Write-Host "Mac/iOS binaries copied." -ForegroundColor Green
}

# ── Step 5: Build Windows native C++ ──────────────────────────────────────
function Step-WindowsNative {
    Write-StepHeader "Step 5: Building Windows native C++ (x86 + x64)"

    $windowsNative = Join-Path $ProjectRoot "WindowsNative"

    foreach ($arch in @("x86", "x64")) {
        $hostArch = "x86" # VS2017 Host is x86
        if ($arch -eq "x86") {
            $buildDir = Join-Path $windowsNative "cmake-build-release-x32"
            $vcArch = "x86"
        } else {
            $buildDir = Join-Path $windowsNative "cmake-build-release-x64"
            $vcArch = "x86_amd64"
        }

        Write-Host "Building $arch..."

        # Create build dir if needed
        if (-not (Test-Path $buildDir)) {
            New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
        }

        # Write a temp .bat to set up VS environment and run cmake + ninja
        $batFile = Join-Path $buildDir "build.bat"
        @"
@echo off
call "$VS2017VcVars" $vcArch
if errorlevel 1 exit /b 1
cd /d "$buildDir"
"$CMake" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="$Ninja" -DCMAKE_C_COMPILER=cl.exe -DCMAKE_CXX_COMPILER=cl.exe "$windowsNative"
if errorlevel 1 exit /b 1
"$Ninja" -j%NUMBER_OF_PROCESSORS%
if errorlevel 1 exit /b 1
"@ | Set-Content -Path $batFile -Encoding ASCII
        cmd /c "`"$batFile`""
        if ($LASTEXITCODE -ne 0) { throw "Windows $arch build failed" }

        # Copy to AneBuild
        $targetDir = if ($arch -eq "x86") { "windows-32" } else { "windows-64" }
        Copy-Item "$buildDir\AneAwesomeUtilsWindows.dll" "$AneBuild\$targetDir\AneAwesomeUtilsWindows.dll" -Force
        Write-Host "$arch build complete." -ForegroundColor Green
    }
}

# ── Step 6: Build .NET for Windows ──────────────────────────────────────
function Step-DotnetWin {
    Write-StepHeader "Step 6: Building .NET for Windows (x86 + x64)"

    $csproj = Join-Path $ProjectRoot "CSharpLibrary\AwesomeAneUtils\AwesomeAneUtils\AwesomeAneUtils.csproj"

    # Add vswhere to PATH so NativeAOT linker can find VS
    $vswhereDir = "C:\Program Files (x86)\Microsoft Visual Studio\Installer"
    if (Test-Path $vswhereDir) {
        $env:PATH = "$vswhereDir;$env:PATH"
    }

    # x86
    Write-Host "Building dotnet win-x86..."
    dotnet publish /p:NativeLib=Shared /p:Configuration=Release $csproj
    if ($LASTEXITCODE -ne 0) { throw "dotnet win-x86 build failed" }

    # x64
    Write-Host "Building dotnet win-x64..."
    dotnet publish /p:NativeLib=Shared /p:Configuration=Release -r win-x64 $csproj
    if ($LASTEXITCODE -ne 0) { throw "dotnet win-x64 build failed" }

    # Copy to AneBuild
    $csOutputBase = Join-Path $ProjectRoot "CSharpLibrary\AwesomeAneUtils\AwesomeAneUtils\bin\Release\net10.0"
    Copy-Item "$csOutputBase\win-x86\publish\AwesomeAneUtils.dll" "$AneBuild\windows-32\AwesomeAneUtils.dll" -Force
    Copy-Item "$csOutputBase\win-x64\publish\AwesomeAneUtils.dll" "$AneBuild\windows-64\AwesomeAneUtils.dll" -Force

    Write-Host ".NET Windows builds complete." -ForegroundColor Green
}

# ── Step 7: Build Android ─────────────────────────────────────────────────
function Step-Android {
    Write-StepHeader "Step 7: Building Android (Gradle)"

    $androidDir = Join-Path $ProjectRoot "AndroidNative"
    $gradlew = Join-Path $androidDir "gradlew.bat"

    cmd /c "cd /d `"$androidDir`" && `"$gradlew`" assembleDebug"
    if ($LASTEXITCODE -ne 0) { throw "Android build failed" }

    # Copy AAR to AneBuild
    Copy-Item "$androidDir\app\build\outputs\aar\app-debug.aar" "$AneBuild\android\app-debug.aar" -Force

    Write-Host "Android build complete." -ForegroundColor Green
}

# ── Step 8: Compile AS3 to SWC ────────────────────────────────────────────
function Step-AS3 {
    Write-StepHeader "Step 8: Compiling AS3 to SWC"

    $srcDir = Join-Path $ProjectRoot "src"
    $outDir = Join-Path $ProjectRoot "out\production\ane-awesome-utils"
    $swcOutput = Join-Path $outDir "ane-awesome-utils.swc"
    $airGlobal = Join-Path $AirSdkPath "frameworks\libs\air\airglobal.swc"

    if (-not (Test-Path $outDir)) {
        New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    }

    # Compile using custom AS3 compiler (COMPC mode for SWC)
    & $AS3Compiler "-output=$swcOutput" "-include-sources=$srcDir" "-external-library-path=$airGlobal"
    if ($LASTEXITCODE -ne 0) { throw "AS3 compilation failed" }

    # Copy SWC to AneBuild
    Copy-Item $swcOutput "$AneBuild\library.swc" -Force

    # Extract library.swf from SWC into all platform dirs
    $platforms = @("default", "android", "windows-32", "windows-64", "macos", "ios")
    foreach ($platform in $platforms) {
        $platDir = Join-Path $AneBuild $platform
        if (-not (Test-Path $platDir)) {
            New-Item -ItemType Directory -Path $platDir -Force | Out-Null
        }
        & $SevenZip e "$AneBuild\library.swc" "library.swf" "-o$platDir" -aoa | Out-Null
    }

    Write-Host "AS3 compilation complete." -ForegroundColor Green
}

# ── Step 9: Package ANE ───────────────────────────────────────────────────
function Step-Package {
    Write-StepHeader "Step 9: Packaging ANE"

    Push-Location $AneBuild
    try {
        # Sign Windows DLLs
        Write-Host "Signing Windows DLLs..."
        signtool sign /fd sha256 /tr http://ts.ssl.com /td sha256 /n "$WinSignName" `
            "windows-32\AwesomeAneUtils.dll" `
            "windows-32\AneAwesomeUtilsWindows.dll" `
            "windows-64\AwesomeAneUtils.dll" `
            "windows-64\AneAwesomeUtilsWindows.dll"
        if ($LASTEXITCODE -ne 0) { throw "Windows DLL signing failed" }

        # Package ANE with ADT
        Write-Host "Running ADT to package ANE..."
        java -jar $AdtJar `
            -package -target ane `
            br.com.redesurftank.aneawesomeutils.ane `
            extension.xml `
            -swc library.swc `
            -platform default -C default . `
            -platform Windows-x86 -C windows-32 . `
            -platform Windows-x86-64 -C windows-64 . `
            -platform MacOS-x86-64 -C macos . `
            -platform iPhone-ARM -platformoptions platformIOS.xml -C ios . `
            -platform Android -platformoptions platformAndroid.xml -C android .
        if ($LASTEXITCODE -ne 0) { throw "ANE packaging failed" }

        $aneFile = Join-Path $AneBuild "br.com.redesurftank.aneawesomeutils.ane"
        $aneSize = (Get-Item $aneFile).Length / 1MB
        Write-Host "ANE packaged: $aneFile ($([math]::Round($aneSize, 2)) MB)" -ForegroundColor Green

    } finally {
        Pop-Location
    }
}

# ── Main ──────────────────────────────────────────────────────────────────
$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

try {
    if ($Step -eq "all" -or $Step -eq "sync")           { Step-Sync }
    if ($Step -eq "all" -or $Step -eq "dotnet-mac")      { Step-DotnetMac }
    if ($Step -eq "all" -or $Step -eq "xcode")           { Step-Xcode }
    if ($Step -eq "all" -or $Step -eq "copy-mac")        { Step-CopyMac }
    if ($Step -eq "all" -or $Step -eq "windows-native")  { Step-WindowsNative }
    if ($Step -eq "all" -or $Step -eq "dotnet-win")      { Step-DotnetWin }
    if ($Step -eq "all" -or $Step -eq "android")         { Step-Android }
    if ($Step -eq "all" -or $Step -eq "as3")             { Step-AS3 }
    if ($Step -eq "all" -or $Step -eq "package")         { Step-Package }

    $stopwatch.Stop()
    Write-Host ""
    Write-Host ("=" * 70) -ForegroundColor Green
    Write-Host "  BUILD COMPLETE in $([math]::Round($stopwatch.Elapsed.TotalMinutes, 1)) minutes" -ForegroundColor Green
    Write-Host ("=" * 70) -ForegroundColor Green

} catch {
    $stopwatch.Stop()
    Write-Host ""
    Write-Host ("=" * 70) -ForegroundColor Red
    Write-Host "  BUILD FAILED: $_" -ForegroundColor Red
    Write-Host "  Elapsed: $([math]::Round($stopwatch.Elapsed.TotalMinutes, 1)) minutes" -ForegroundColor Red
    Write-Host ("=" * 70) -ForegroundColor Red
    exit 1
}
