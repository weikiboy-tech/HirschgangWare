@echo off
setlocal
cd /d "%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
if not exist build\aim-assist-test mkdir build\aim-assist-test
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /Fo:"build\aim-assist-test\\" /Fe:build\aim-assist-test\test.exe tests\aim_assist.cpp user32.lib || exit /b 1
build\aim-assist-test\test.exe
exit /b %errorlevel%
