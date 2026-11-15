@echo off
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere not found & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (echo Visual Studio not found & exit /b 1)
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
set FLAGS=/nologo /std:c++17 /EHsc /O2 /W3 /MT /I "%~dp0..\third_party\pcileech"
set OUT=%~dp0
set OBJ=%TEMP%\keyprobe_obj
if exist "%OBJ%" rmdir /s /q "%OBJ%"
mkdir "%OBJ%"
cl %FLAGS% /Fo:"%OBJ%\\" /Fe:"%OUT%keyprobe.exe" "%~dp0keyprobe.cpp" /link /INCREMENTAL:NO /SUBSYSTEM:CONSOLE user32.lib || exit /b 1
rmdir /s /q "%OBJ%"
echo built %OUT%keyprobe.exe
exit /b 0
