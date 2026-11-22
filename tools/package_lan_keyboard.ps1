[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$build = Join-Path $PSScriptRoot '..\build'
$dist = Join-Path $build 'lan-keyboard-public'
$folder = Join-Path $dist 'package'
$sender = Join-Path $build 'HirschgangWare-InputSender.exe'
$updater = Join-Path $build 'HirschgangWare-InputUpdater.exe'
$revision = (Get-Item -LiteralPath $sender).VersionInfo
$declared = Select-String -LiteralPath (Join-Path $PSScriptRoot 'lan_keyboard_version.h') -Pattern '^#define HG_LAN_SENDER_REVISION (\d+)$'
if (!$declared -or $revision.FileMajorPart -ne 1 -or $revision.FileMinorPart -ne 0 -or
    $revision.FileBuildPart -ne 0 -or $revision.FilePrivatePart -le 0 -or
    $revision.FilePrivatePart -ne [int]$declared.Matches[0].Groups[1].Value) {
    throw 'Rebuild the sender with the declared revision before packaging.'
}

New-Item -ItemType Directory -Path $folder -Force | Out-Null
Copy-Item -LiteralPath $sender,$updater,(Join-Path $PSScriptRoot 'install_lan_keyboard_sender.ps1') -Destination $folder -Force
@'
[lan_keyboard]
receiver=SECOND_PC_LAN_IPV4
port=47832
key=PRIVATE_PAIRING_KEY_FROM_SECOND_PC
keys=E,Y
'@ | Set-Content -LiteralPath (Join-Path $folder 'lan-keyboard-sender.ini.example') -Encoding ascii
@'
HirschgangWare LAN keyboard sender

Run install_lan_keyboard_sender.ps1 once from this folder. Use -AllKeys
if you want any supported keyboard key to be bindable. AllKeys forwards
key states to the paired PC even outside the game while Windows is unlocked.

The installer reuses the existing pairing. For a first install only,
place the private lan-keyboard-sender.ini from the second PC in this folder.
The .example file contains placeholders only.

The installed sender checks GitHub at startup and hourly. New versions
are applied after one minute of Windows input inactivity. The pairing
and the enabled keys are kept. The latest update status is in
%LOCALAPPDATA%\HirschgangWareInput\lan-keyboard-update.log.
'@ | Set-Content -LiteralPath (Join-Path $folder 'INSTALL.txt') -Encoding ascii

$files = 'HirschgangWare-InputSender.exe','HirschgangWare-InputUpdater.exe','install_lan_keyboard_sender.ps1','INSTALL.txt','lan-keyboard-sender.ini.example'
Compress-Archive -LiteralPath @($files | ForEach-Object { Join-Path $folder $_ }) -DestinationPath (Join-Path $dist 'HirschgangWare-LAN-MainPC.zip') -Force
Copy-Item -LiteralPath $sender -Destination $dist -Force
$sha256 = (Get-FileHash -LiteralPath (Join-Path $dist 'HirschgangWare-InputSender.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
"$($revision.FilePrivatePart) $sha256" | Set-Content -LiteralPath (Join-Path $dist 'lan-keyboard-version.txt') -Encoding ascii
Write-Output "Sender revision $($revision.FilePrivatePart) staged for publication in $dist"
