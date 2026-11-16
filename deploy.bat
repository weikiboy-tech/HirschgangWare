@echo off
setlocal
cd /d "%~dp0"
set "TARGET=%~1"
if "%TARGET%"=="" if exist laptop\address.txt set /p TARGET=<laptop\address.txt
if "%TARGET%"=="" (echo usage: deploy.bat ^<laptop-name-or-ip^> & exit /b 1)
set "DEST=\\%TARGET%\acesp"

if not exist "%DEST%\" (
    echo cannot reach %DEST% - is the laptop on, the folder shared as "acesp", and the network set to Private?
    echo if Windows needs a login for it, run once:  net use %DEST% /user:%TARGET%\^<laptop-user^>
    exit /b 1
)
>laptop\address.txt echo %TARGET%

copy /y build\HirschgangWare.exe "%DEST%\HirschgangWare_pending.exe" >nul || (echo copy failed - cannot reach %DEST% & exit /b 1)
copy /y laptop\run_hirschgangware.bat "%DEST%\" >nul
if not exist "%DEST%\dayz_rare.txt" copy /y laptop\dayz_rare.txt "%DEST%\" >nul
if not exist "%DEST%\dayz_loot_cats.txt" copy /y laptop\dayz_loot_cats.txt "%DEST%\" >nul
if exist build\dma\ robocopy build\dma "%DEST%\dma" /e /xo /njh /njs /ndl /nfl >nul
robocopy web "%DEST%\web" /e /xo /njh /njs /ndl /nfl >nul
if errorlevel 8 exit /b 1
robocopy dist\maps "%DEST%\maps" /e /xo /njh /njs /ndl /nfl >nul
if errorlevel 8 exit /b 1
echo deployed to %DEST%
exit /b 0
