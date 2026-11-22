@echo off
setlocal
cd /d "%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
if not exist build\lan-keyboard-updater-test mkdir build\lan-keyboard-updater-test
rc /nologo /fo build\lan-keyboard-updater-test\sender.res tools\lan_keyboard_sender.rc || exit /b 1
cl /nologo /std:c++17 /W4 /EHsc /O2 /MT /Fo:"build\lan-keyboard-updater-test\\" /Fe:build\lan-keyboard-updater-test\test.exe tests\lan_keyboard_updater.cpp build\lan-keyboard-updater-test\sender.res || exit /b 1
if /i "%~1"=="build-only" exit /b 0
build\lan-keyboard-updater-test\test.exe
exit /b %errorlevel%
