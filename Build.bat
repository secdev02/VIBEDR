@echo off
REM Build script for MinimalEDR Driver
REM Requires Windows Driver Kit (WDK) and Visual Studio Build Tools

echo Building MinimalEDR Driver...

REM Set WDK paths (adjust as needed)
set WDK_PATH=C:\Program Files (x86)\Windows Kits\10
set VS_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community

REM Check if WDK is installed
if not exist "%WDK_PATH%" (
    echo ERROR: Windows Driver Kit not found at %WDK_PATH%
    echo Please install WDK from: https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk
    exit /b 1
)

REM Set up build environment
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"

REM Build driver
cd Driver
msbuild MinimalEDR.vcxproj /p:Configuration=Release /p:Platform=x64

if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Driver build failed
    exit /b 1
)

echo Driver built successfully
cd ..

REM Build service
cd Service
cl.exe /nologo /W4 /O2 /D_CRT_SECURE_NO_WARNINGS EDRService.c /Fe:EDRService.exe user32.lib advapi32.lib wininet.lib

if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Service build failed
    exit /b 1
)

echo Service built successfully
cd ..

echo.
echo Build complete!
echo Driver: Driver\x64\Release\MinimalEDR.sys
echo Service: Service\EDRService.exe
