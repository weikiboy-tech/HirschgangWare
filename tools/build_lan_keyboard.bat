@echo off
setlocal
cd /d "%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 1
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
if not exist build mkdir build
rc /nologo /fo build\lan_keyboard_sender.res tools\lan_keyboard_sender.rc || exit /b 1
cl /nologo /std:c++17 /W4 /EHsc /O2 /MT /Fo:build\lan_keyboard_sender.obj /Fe:build\HirschgangWare-InputSender.exe tools\lan_keyboard_sender.cpp build\lan_keyboard_sender.res /link /SUBSYSTEM:WINDOWS /INCREMENTAL:NO || exit /b 1
cl /nologo /std:c++17 /W4 /EHsc /O2 /MT /Fo:build\lan_keyboard_updater.obj /Fe:build\HirschgangWare-InputUpdater.exe tools\lan_keyboard_updater.cpp /link /SUBSYSTEM:WINDOWS /INCREMENTAL:NO
exit /b %errorlevel%
