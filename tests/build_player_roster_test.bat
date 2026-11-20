@echo off
setlocal
cd /d "%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
if not exist build\player-roster-test mkdir build\player-roster-test
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /Fo:"build\player-roster-test\\" /Fe:build\player-roster-test\test.exe tests\player_roster.cpp src\process.cpp || exit /b 1
build\player-roster-test\test.exe
exit /b %errorlevel%
