@echo off
:: —————— carrega o .env ——————
for /f "usebackq tokens=1,* delims==" %%A in ("%~dp0.env") do (
  if not "%%A"=="" (
    set "%%A=%%B"
  )
)

:: monta a variável com o jar
set "ADT_JAR=%AIRSDK_PATH%\lib\adt.jar"

REM Set the path to the 7z.exe file
set PATH=%PATH%;"C:\Program Files\7-Zip\"

copy ..\AndroidNative\app\build\outputs\aar\app-debug.aar .\android
copy ..\out\production\ane-awesome-utils\ane-awesome-utils.swc library.swc

REM Extract SWF from SWC for all platforms
7z e library.swc library.swf -o./default -aoa
7z e library.swc library.swf -o./android -aoa
7z e library.swc library.swf -o./windows-32 -aoa
7z e library.swc library.swf -o./windows-64 -aoa
7z e library.swc library.swf -o./macos -aoa
7z e library.swc library.swf -o./ios -aoa

REM Clean up temp directory
rmdir /S /Q temp

call signtool sign /fd sha256 /tr http://ts.ssl.com /td sha256 /n "SURFTANK LTDA" "windows-32/AwesomeAneUtils.dll" "windows-32/AneAwesomeUtilsWindows.dll" "windows-64/AwesomeAneUtils.dll" "windows-64/AneAwesomeUtilsWindows.dll"

REM Package the ANE
"%ADT_JAR%" -package -target ane br.com.redesurftank.aneawesomeutils.ane extension.xml -swc library.swc -platform default -C default . -platform Windows-x86 -C windows-32 . -platform Windows-x86-64 -C windows-64 . -platform MacOS-x86-64 -C macos . -platform iPhone-ARM -platformoptions platformIOS.xml -C ios . -platform Android -platformoptions platformAndroid.xml -C android .
