@echo off
setlocal
cd /d "%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
if not exist build\aim-assist-ui-test mkdir build\aim-assist-ui-test
set IMGUI=src\imgui\imgui.cpp src\imgui\imgui_draw.cpp src\imgui\imgui_tables.cpp src\imgui\imgui_widgets.cpp src\imgui\backends\imgui_impl_win32.cpp src\imgui\backends\imgui_impl_dx11.cpp
cl /nologo /std:c++17 /EHsc /W4 /Od /MT /I src\imgui /DIMGUI_ENABLE_TEST_ENGINE /Fo:"build\aim-assist-ui-test\\" /Fe:build\aim-assist-ui-test\test.exe tests\aim_assist_ui.cpp src\trigger.cpp src\lan_keyboard.cpp src\mapdl.cpp src\loot.cpp src\friends.cpp src\webmap.cpp %IMGUI% user32.lib gdi32.lib shell32.lib || exit /b 1
build\aim-assist-ui-test\test.exe
exit /b %errorlevel%
