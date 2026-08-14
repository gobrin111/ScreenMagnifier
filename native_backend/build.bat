@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

where cl.exe >nul 2>nul
if errorlevel 1 (
    if not exist "%VSWHERE%" (
        echo Visual Studio 2022 Build Tools were not found.
        echo Install "Desktop development with C++" and a Windows 10 or 11 SDK.
        endlocal & exit /b 1
    )

    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
    if not defined VSINSTALL (
        echo Visual Studio C++ build tools were not found.
        echo Add the "Desktop development with C++" workload in Visual Studio Installer.
        endlocal & exit /b 1
    )
    call "!VSINSTALL!\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
    if errorlevel 1 (endlocal & exit /b 1)
)

if not exist build mkdir build

cl.exe /nologo /std:c++17 /EHsc /O2 /W4 /DUNICODE /D_UNICODE /Fo:build\main.obj ^
    /Fe:build\GpuMagnifierPrototype.exe main.cpp ^
    /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib

if errorlevel 1 (endlocal & exit /b 1)

cl.exe /nologo /std:c++17 /EHsc /O2 /W4 /DUNICODE /D_UNICODE /Fo:build\dwm_thumbnail.obj ^
    /Fe:build\DwmMagnifierPrototype.exe dwm_thumbnail.cpp ^
    /link dwmapi.lib user32.lib gdi32.lib

if errorlevel 1 (endlocal & exit /b 1)

cl.exe /nologo /std:c++17 /EHsc /O2 /W4 /DUNICODE /D_UNICODE /Fo:build\wgc_window.obj ^
    /Fe:build\WgcMagnifierPrototype.exe wgc_window.cpp ^
    /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib windowsapp.lib

if errorlevel 1 (endlocal & exit /b 1)

echo.
echo Built build\GpuMagnifierPrototype.exe
echo Built build\DwmMagnifierPrototype.exe
echo Built build\WgcMagnifierPrototype.exe
endlocal & exit /b 0
