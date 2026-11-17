@echo off
setlocal
cd /d "%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
if not exist build\dayz-camera-test mkdir build\dayz-camera-test
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /Fo:"build\dayz-camera-test\\" /Fe:build\dayz-camera-test\test.exe tests\dayz_camera.cpp src\trigger.cpp user32.lib || exit /b 1
build\dayz-camera-test\test.exe
exit /b %errorlevel%
