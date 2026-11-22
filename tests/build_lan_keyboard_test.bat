@echo off
setlocal
cd /d "%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
if not exist build\lan-keyboard-test mkdir build\lan-keyboard-test
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /Fo:"build\lan-keyboard-test\\" /Fe:build\lan-keyboard-test\test.exe tests\lan_keyboard_test.cpp src\lan_keyboard.cpp user32.lib || exit /b 1
if /i "%~1"=="build-only" exit /b 0
build\lan-keyboard-test\test.exe
exit /b %errorlevel%
