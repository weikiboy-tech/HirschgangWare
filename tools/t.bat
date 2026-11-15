@echo off
setlocal
echo dp0=[%~dp0]
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
echo VSW=[%VSWHERE%]
if not exist "%VSWHERE%" (echo vswhere not found & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
echo VSROOT=[%VSROOT%]
if not defined VSROOT (echo Visual Studio not found & exit /b 1)
echo OK
exit /b 0
