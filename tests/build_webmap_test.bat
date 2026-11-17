@echo off
setlocal
cd /d "%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
if not exist build\webmap-test mkdir build\webmap-test
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /Fo:"build\webmap-test\\" /Fe:build\webmap-test\server.exe tests\webmap_fixture.cpp src\webmap.cpp src\loot.cpp
exit /b %errorlevel%
