@echo off
setlocal EnableDelayedExpansion

rem Builds the TestProfilerApp SWF and packages it into a captive-runtime
rem bundle so that:
rem   - the bundled `Adobe AIR.dll` matches our Mode B RVAs, and
rem   - the app is fully self-contained (no adl, no installed AIR).
rem
rem Usage:
rem   build.bat                 -- x64 build (default)
rem   build.bat x86             -- x86 build
rem   build.bat clean           -- wipes output then x64
rem   build.bat clean x86       -- wipes output then x86

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
set DESCRIPTOR=TestProfilerApp-app.xml
if /I "%~1"=="clean" (
    if exist "%OUT%" rmdir /s /q "%OUT%"
    if /I "%~2"=="x86" set ARCH=x86
) else (
    if /I "%~1"=="x86" set ARCH=x86
)
if /I "%ARCH%"=="x86" set DESCRIPTOR=TestProfilerApp-app-x86.xml
echo [build] target architecture: %ARCH% (descriptor: %DESCRIPTOR%)

if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\TestProfilerApp" rmdir /s /q "%OUT%\TestProfilerApp"
if exist "%OUT%\TestProfilerApp.swf" del /q "%OUT%\TestProfilerApp.swf"
if exist "%EXT_DIR%" rmdir /s /q "%EXT_DIR%"
mkdir "%EXT_DIR%"

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
    %DESCRIPTOR% ^
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
