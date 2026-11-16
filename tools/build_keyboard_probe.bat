@echo off
setlocal
cd /d "%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
if not exist build mkdir build
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /I third_party\pcileech /Fo:build\keyboard_probe.obj /Fe:build\keyboard_probe.exe tools\keyboard_probe.cpp user32.lib
exit /b %errorlevel%
