@echo off
setlocal
cd /d "%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
if not exist build\dma-liveness-test mkdir build\dma-liveness-test
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /LD /Fo:"build\dma-liveness-test\\" /Fe:build\dma-liveness-test\vmm.dll tests\dma_liveness_mock.cpp /link /DEF:tests\dma_liveness_mock.def /IMPLIB:build\dma-liveness-test\vmm.lib || exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /I third_party\pcileech /Fo:"build\dma-liveness-test\\" /Fe:build\dma-liveness-test\test.exe tests\dma_liveness.cpp src\dma.cpp src\process.cpp src\telemetry.cpp user32.lib || exit /b 1
build\dma-liveness-test\test.exe
exit /b %errorlevel%
