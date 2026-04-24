@echo off
setlocal EnableDelayedExpansion

rem Builds the TestProfilerApp SWF and packages it into a captive-runtime
rem bundle so the app is fully self-contained (no adl, no installed AIR).
rem
rem Usage:
rem   build.bat                      -- x64 build, maxD3D 14 (default)
rem   build.bat x86                  -- x86 build, maxD3D 14
rem   build.bat d3d9                 -- x64 build, maxD3D 9
rem   build.bat clean x86 d3d9       -- clean, x86 build, maxD3D 9

cd /d "%~dp0"

set SDK=C:\AIRSDKs\AIRSDK_51.1.3.10
set ROOT=%~dp0..\..\..
set ANE_FILE=%ROOT%\AneBuild\br.com.redesurftank.aneawesomeutils.ane
set SWC_LIB=%ROOT%\AneBuild\library.swc
set AIRGLOBAL_SWC=%SDK%\frameworks\libs\air\airglobal.swc
set OUT=%~dp0..\output
set EXT_DIR=%OUT%\.extdir
set CERT=%OUT%\.testcert.p12
set CERT_PASS=testpass

set ARCH=x64
set CLEAN=0
set MAXD3D=14

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="clean" set CLEAN=1
if /I "%~1"=="x86" set ARCH=x86
if /I "%~1"=="x64" set ARCH=x64
if /I "%~1"=="d3d9" set MAXD3D=9
if /I "%~1"=="d3d11" set MAXD3D=14
shift
goto parse_args

:args_done
set DESCRIPTOR=TestProfilerApp-app.xml
if /I "%ARCH%"=="x86" set DESCRIPTOR=TestProfilerApp-app-x86.xml
set GENERATED_DESCRIPTOR=%OUT%\TestProfilerApp-%ARCH%-d3d%MAXD3D%-app.xml
if "%CLEAN%"=="1" (
    if exist "%OUT%" rmdir /s /q "%OUT%"
)
echo [build] target architecture: %ARCH% (descriptor: %DESCRIPTOR%, maxD3D=%MAXD3D%)

if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\TestProfilerApp" rmdir /s /q "%OUT%\TestProfilerApp"
if exist "%OUT%\TestProfilerApp.swf" del /q "%OUT%\TestProfilerApp.swf"
if exist "%EXT_DIR%" rmdir /s /q "%EXT_DIR%"
mkdir "%EXT_DIR%"

set SOURCE_DESCRIPTOR=%CD%\%DESCRIPTOR%
set MAXD3D_VALUE=%MAXD3D%
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$xml = [IO.File]::ReadAllText($env:SOURCE_DESCRIPTOR); $nl = [Environment]::NewLine; $block = '  <windows>' + $nl + '    <maxD3D>' + $env:MAXD3D_VALUE + '</maxD3D>' + $nl + '  </windows>' + $nl; if ($xml -match '<windows>') { $xml = [regex]::Replace($xml, '(?s)\s*<windows>.*?</windows>\s*', $nl + $block) } else { $xml = $xml -replace '</initialWindow>', ('</initialWindow>' + $nl + $block) }; $enc = New-Object System.Text.UTF8Encoding($false); [IO.File]::WriteAllText($env:GENERATED_DESCRIPTOR, $xml, $enc)"
if errorlevel 1 exit /b 1
if not exist "%GENERATED_DESCRIPTOR%" (
    echo [build] ERROR: generated descriptor missing: %GENERATED_DESCRIPTOR%
    exit /b 1
)

if not exist "%ANE_FILE%" (
    echo [build] ERROR: ANE not found at %ANE_FILE%. Run `python build-all.py package`.
    exit /b 1
)
if not exist "%SWC_LIB%" (
    echo [build] ERROR: SWC not found at %SWC_LIB%. Run `python build-all.py as3`.
    exit /b 1
)

copy /Y "%ANE_FILE%" "%EXT_DIR%\" >nul

rem -------- 1. Compile SWF -------------------------------------------------
echo [build] compiling SWF
call "%SDK%\bin\amxmlc.bat" ^
    -library-path+=%SWC_LIB% ^
    -external-library-path+=%AIRGLOBAL_SWC% ^
    -output=%OUT%\TestProfilerApp.swf ^
    TestProfilerApp.as
if errorlevel 1 exit /b 1

rem -------- 2. Self-signed cert for packaging ------------------------------
if not exist "%CERT%" (
    echo [build] generating self-signed cert
    call "%SDK%\bin\adt.bat" -certificate -cn TestProfiler 2048-RSA "%CERT%" %CERT_PASS%
    if errorlevel 1 exit /b 1
)

rem -------- 3. Package as captive-runtime bundle ---------------------------
rem `-tsa none` disables RFC-3161 timestamping (Adobe's old TSA server is dead).
echo [build] packaging captive bundle (%ARCH%)
call "%SDK%\bin\adt.bat" -package ^
    -storetype pkcs12 -keystore "%CERT%" -storepass %CERT_PASS% -tsa none ^
    -target bundle ^
    "%OUT%\TestProfilerApp" ^
    "%GENERATED_DESCRIPTOR%" ^
    -extdir "%EXT_DIR%" ^
    -C "%OUT%" TestProfilerApp.swf
if errorlevel 1 exit /b 1

if not exist "%OUT%\TestProfilerApp\TestProfilerApp.exe" (
    echo [build] ERROR: packaged exe missing
    exit /b 1
)

echo.
echo [build] OK: %OUT%\TestProfilerApp\TestProfilerApp.exe
endlocal
