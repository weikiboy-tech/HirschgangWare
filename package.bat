@echo off
setlocal
cd /d "%~dp0"
if not exist build\HirschgangWare.exe (echo build\HirschgangWare.exe missing - run build.bat first & exit /b 1)
for /f "tokens=3 delims= " %%v in ('findstr HIRSCHGANGWARE_VERSION src\version.h') do set "VER=%%~v"
set "HG_ZIP=HirschgangWare-%VER%.zip"
powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $temp=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()); $root=Join-Path $temp ('hirschgangware-pkg-'+[Guid]::NewGuid().ToString('N')); $stage=Join-Path $root 'HirschgangWare'; try { New-Item -ItemType Directory -Path $stage -Force | Out-Null; Copy-Item -LiteralPath 'build/HirschgangWare.exe','laptop/run_hirschgangware.bat','laptop/dayz_rare.txt','laptop/dayz_loot_cats.txt','LICENSE','THIRD_PARTY.md' -Destination $stage; $dma=New-Item -ItemType Directory -Path (Join-Path $stage 'dma'); Get-ChildItem -LiteralPath 'build/dma' -File | Copy-Item -Destination $dma.FullName; Copy-Item -LiteralPath 'web','dist/maps' -Destination $stage -Recurse; Add-Type -AssemblyName System.IO.Compression.FileSystem; $archive=Join-Path $root $env:HG_ZIP; [IO.Compression.ZipFile]::CreateFromDirectory($stage,$archive,[IO.Compression.CompressionLevel]::Optimal,$true); Copy-Item -LiteralPath $archive -Destination $env:HG_ZIP -Force } finally { if ([IO.Path]::GetDirectoryName($root).TrimEnd('\') -eq $temp.TrimEnd('\') -and (Test-Path -LiteralPath $root)) { Remove-Item -LiteralPath $root -Recurse -Force } }" || exit /b 1
echo packaged %HG_ZIP%
exit /b 0
