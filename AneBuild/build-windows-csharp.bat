@echo off
copy ..\WindowsNative\cmake-build-release-x32\AwesomeAneUtils.dll .\windows-32\AwesomeAneUtils.dll /Y /F
dotnet publish /p:NativeLib=Shared /p:Configuration=Release ..\CSharpLibrary\AwesomeAneUtils\AwesomeAneUtils\AwesomeAneUtils.csproj
copy ..\CSharpLibrary\AwesomeAneUtils\AwesomeAneUtils\bin\Release\net9.0\win-x86\publish\AwesomeAneUtils.dll .\windows-32\AwesomeAneUtilsCsharp.dll /Y /F