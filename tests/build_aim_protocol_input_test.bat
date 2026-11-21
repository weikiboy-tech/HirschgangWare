@echo off
setlocal
cd /d "%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
if not exist build\aim-protocol-input-test mkdir build\aim-protocol-input-test
cl /nologo /std:c++17 /EHsc /O2 /W4 /MT /Fo:"build\aim-protocol-input-test\\" /Fe:build\aim-protocol-input-test\test.exe tests\aim_protocol_input.cpp user32.lib || exit /b 1
build\aim-protocol-input-test\test.exe
exit /b %errorlevel%
