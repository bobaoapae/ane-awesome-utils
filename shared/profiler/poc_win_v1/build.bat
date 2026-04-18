@echo off
setlocal

cd /d "%~dp0"
set SRC_DIR=%CD%
set BUILD_DIR=%SRC_DIR%\build

set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvarsall.bat
set NINJA=C:\Users\Joao\AppData\Local\Programs\CLion\bin\ninja\win\x64\ninja.exe
set CMAKE=C:\Users\Joao\AppData\Local\Programs\CLion\bin\cmake\win\x64\bin\cmake.exe

call "%VCVARS%" x86_amd64
if errorlevel 1 exit /b 1

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

"%CMAKE%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_C_COMPILER=cl.exe -DCMAKE_CXX_COMPILER=cl.exe "%SRC_DIR%"
if errorlevel 1 exit /b 1

"%NINJA%" -j%NUMBER_OF_PROCESSORS%
if errorlevel 1 exit /b 1

echo.
echo Built artifacts:
dir /B *.dll *.exe 2>nul
endlocal
