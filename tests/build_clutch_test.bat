@echo off
setlocal
cd /d "%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
if not exist build\clutch-test mkdir build\clutch-test
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /Fo:"build\clutch-test\\" /Fe:build\clutch-test\test.exe tests\clutch.cpp src\trigger.cpp user32.lib || exit /b 1
build\clutch-test\test.exe
exit /b %errorlevel%
