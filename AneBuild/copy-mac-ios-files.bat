@echo off
REM SSH copy for macOS and iOS libraries (before packaging)
echo Copying macOS and iOS libraries via SSH

REM Copying iOS library
scp joaovitorborges@192.168.80.102:/Users/joaovitorborges/IdeaProjects/ane-awesome-utils/AppleNative/AneAwesomeUtils/DerivedData/AneAwesomeUtils/Build/Products/Debug-iphoneos/libAneAwesomeUtils-IOS.a .\ios\libAneAwesomeUtils-IOS.a
REM Delete the local macOS framework folder before copying
if exist .\macos\AneAwesomeUtils.framework (
    rmdir /S /Q .\macos\AneAwesomeUtils.framework
)

REM Copying macOS framework
scp -r joaovitorborges@192.168.80.102:/Users/joaovitorborges/IdeaProjects/ane-awesome-utils/AppleNative/AneAwesomeUtils/DerivedData/AneAwesomeUtils/Build/Products/Debug/AneAwesomeUtils.framework/Versions/Current .\macos\AneAwesomeUtils.framework