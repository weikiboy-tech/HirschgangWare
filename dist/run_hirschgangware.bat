@echo off
cd /d "%~dp0"
if exist HirschgangWare_pending.exe move /y HirschgangWare_pending.exe HirschgangWare.exe >nul
start "" HirschgangWare.exe --dma --fullscreen
