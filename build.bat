@echo off
REM Build GE3HUD.asi (x64).
REM Relative paths only after the initial cd, so a space in the project path
REM ("K:\GE3 RE\...") cannot break the cl command line.
setlocal enabledelayedexpansion

cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere not found
    exit /b 1
)

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo ERROR: no Visual Studio with C++ tools found
    exit /b 1
)
echo Using %VSPATH%

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo ERROR: vcvars64 failed
    exit /b 1
)

if not exist build mkdir build

cl /nologo /std:c++17 /EHsc /O2 /MD /W3 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
   /Ithird_party\imgui /Ithird_party\imgui\backends /Ithird_party\detours /Isrc ^
   /Fobuild\ ^
   src\Main.cpp src\Overlay.cpp src\ActorList.cpp src\Rtti.cpp src\Signature.cpp ^
   third_party\imgui\imgui.cpp ^
   third_party\imgui\imgui_draw.cpp ^
   third_party\imgui\imgui_tables.cpp ^
   third_party\imgui\imgui_widgets.cpp ^
   third_party\imgui\backends\imgui_impl_dx11.cpp ^
   third_party\imgui\backends\imgui_impl_win32.cpp ^
   /LD /Febuild\GE3HUD.asi ^
   /link third_party\detours\detours.lib d3d11.lib dxgi.lib d3dcompiler.lib ^
         user32.lib gdi32.lib dwmapi.lib

if errorlevel 1 (
    echo.
    echo BUILD FAILED
    exit /b 1
)

echo.
echo BUILD OK -^> %~dp0build\GE3HUD.asi
endlocal
