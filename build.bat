@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found. Install Visual Studio 2022 or later with the C++ tools.
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo No Visual Studio installation with C++ and CMake was found.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "CM=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%CM%\CMake\bin;%CM%\Ninja;%PATH%"
cd /d "%~dp0"
set "CFG=%~1"
if "%CFG%"=="" set "CFG=Release"
if not exist vcpkg_installed\x64-windows-static-md (
    set "VCPKG_ROOT=%~dp0third_party\vcpkg"
    third_party\vcpkg\vcpkg.exe install --triplet x64-windows-static-md --x-install-root=vcpkg_installed || exit /b 1
)
if not exist build\build.ninja (
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=%CFG% || exit /b 1
)
cmake --build build || exit /b 1
