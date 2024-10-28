@echo off
REM Set the path to the 7z.exe file
set PATH=%PATH%;"C:\Program Files\7-Zip\"

REM Copy the Android AAR and extract classes.jar and JNI libs
xcopy ..\AndroidNative\app\build\outputs\aar\app-debug.aar app-debug.aar /Y /F
7z e app-debug.aar classes.jar -o./android -aoa
7z x app-debug.aar jni\* -o./temp -aoa
xcopy ..\out\production\ane-awesome-utils\ane-awesome-utils.swc library.swc /Y /F
xcopy .\temp\jni\* android\libs\ /E /I /S /Y

REM Extract SWF from SWC for all platforms
7z e library.swc library.swf -o./default -aoa
7z e library.swc library.swf -o./android -aoa
7z e library.swc library.swf -o./windows-32 -aoa
7z e library.swc library.swf -o./macos -aoa

REM Clean up temp directory
rmdir /S /Q temp

REM Package the ANE
adt -package -target ane br.com.redesurftank.aneawesomeutils.ane extension.xml -swc library.swc -platform Android-ARM -platformoptions platformAndroid.xml -C android . -platform Android-ARM64 -platformoptions platformAndroid.xml -C android . -platform Android-x86 -platformoptions platformAndroid.xml -C android . -platform Android-x64 -platformoptions platformAndroid.xml -C android . -platform default -C default . -platform Windows-x86 -C windows-32 . -platform MacOS-x86-64 -C macos .
