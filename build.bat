@echo off
setlocal
cd /d "%~dp0"
set ARCH=%1
if "%ARCH%"=="" set ARCH=x64
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere not found - install Visual Studio with the C++ workload & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (echo Visual Studio not found & exit /b 1)
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" %ARCH% >nul || exit /b 1

set FLAGS=/nologo /std:c++17 /EHsc /O2 /W4 /MT /I third_party\pcileech
set "OBJ=%~dp0build\obj"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist build mkdir build

set IMGUI=src\imgui\imgui.cpp src\imgui\imgui_draw.cpp src\imgui\imgui_tables.cpp src\imgui\imgui_widgets.cpp src\imgui\backends\imgui_impl_win32.cpp src\imgui\backends\imgui_impl_dx11.cpp
rc /nologo /fo "%OBJ%\HirschgangWare.res" HirschgangWare.rc || exit /b 1
cl %FLAGS% /I src\imgui /Fo:"%OBJ%\\" /Fe:build\HirschgangWare.exe src\esp.cpp src\gpu_canvas.cpp src\radar.cpp src\webmap.cpp src\mapdl.cpp src\loot.cpp src\friends.cpp src\trigger.cpp %IMGUI% src\esp_launcher.cpp src\esp_panel.cpp src\ui.cpp src\process.cpp src\dma.cpp src\image.cpp src\rtti.cpp src\discover.cpp src\telemetry.cpp src\speedtest.cpp "%OBJ%\HirschgangWare.res" /link /INCREMENTAL:NO /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /MAP:build\HirschgangWare.map || exit /b 1
robocopy dma build\dma /e /xo /njh /njs /ndl /nfl >nul
copy /y laptop\run_hirschgangware.bat build\ >nul
for %%f in (dayz_rare.txt dayz_loot_cats.txt) do if not exist build\%%f copy /y laptop\%%f build\ >nul
robocopy web build\web /e /xo /njh /njs /ndl /nfl >nul
if errorlevel 8 exit /b 1
robocopy dist\maps build\maps /e /xo /njh /njs /ndl /nfl >nul
if errorlevel 8 exit /b 1
exit /b 0
