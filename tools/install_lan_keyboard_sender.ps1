[CmdletBinding()]
param([switch]$Uninstall, [switch]$AllKeys)

$ErrorActionPreference = 'Stop'
$target = Join-Path $env:LOCALAPPDATA 'HirschgangWareInput'
$exe = Join-Path $target 'HirschgangWare-InputSender.exe'
$updater = Join-Path $target 'HirschgangWare-InputUpdater.exe'
$sourceExe = Join-Path $PSScriptRoot 'HirschgangWare-InputSender.exe'
$sourceUpdater = Join-Path $PSScriptRoot 'HirschgangWare-InputUpdater.exe'
$sourceIni = Join-Path $PSScriptRoot 'lan-keyboard-sender.ini'
$installedIni = Join-Path $target 'lan-keyboard-sender.ini'
$shortcut = Join-Path ([Environment]::GetFolderPath('Startup')) 'HirschgangWare LAN Keyboard.lnk'

if (!$Uninstall) {
    if (!(Test-Path -LiteralPath $sourceExe)) { throw 'Missing HirschgangWare-InputSender.exe in the Main-PC package.' }
    if (!(Test-Path -LiteralPath $sourceUpdater)) { throw 'Missing HirschgangWare-InputUpdater.exe in the Main-PC package.' }
    $pairingIni = if (Test-Path -LiteralPath $sourceIni) { $sourceIni } else { $installedIni }
    if (!(Test-Path -LiteralPath $pairingIni)) {
        throw 'Missing private lan-keyboard-sender.ini. Put the pairing file next to the installer.'
    }
    if ($AllKeys) {
        $pairing = [IO.File]::ReadAllText($pairingIni)
        $keyLine = [regex]'(?im)^[ \t]*keys[ \t]*=[^\r\n]*'
        if ($keyLine.Matches($pairing).Count -ne 1) { throw 'The pairing file must contain one keys= entry.' }
        $pairing = $keyLine.Replace($pairing, 'keys=All', 1)
    }
}
# Stop the installed updater first so it cannot relaunch the sender during setup.
foreach ($binary in $updater, $exe) {
    Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($binary)) -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $binary -and $_.SessionId -eq (Get-Process -Id $PID).SessionId } |
        ForEach-Object {
            Stop-Process -InputObject $_ -ErrorAction Stop
            if (!$_.WaitForExit(5000)) { throw "Could not stop $($_.ProcessName)." }
        }
}
if ($Uninstall) {
    if (Test-Path -LiteralPath $shortcut) { Remove-Item -LiteralPath $shortcut }
    return
}

New-Item -ItemType Directory -Force -Path $target | Out-Null
Copy-Item -LiteralPath $sourceExe -Destination $exe -Force
Copy-Item -LiteralPath $sourceUpdater -Destination $updater -Force
if ($AllKeys) {
    [IO.File]::WriteAllText($installedIni, $pairing)
} elseif (Test-Path -LiteralPath $sourceIni) {
    Copy-Item -LiteralPath $sourceIni -Destination $installedIni -Force
}
$shell = New-Object -ComObject WScript.Shell
$link = $shell.CreateShortcut($shortcut)
$link.TargetPath = $updater
$link.WorkingDirectory = $target
$link.Description = 'Start and update the HirschgangWare LAN keyboard sender'
$link.Save()
Start-Process -FilePath $updater -WorkingDirectory $target -WindowStyle Hidden
Write-Output 'LAN input sender with automatic updates started and registered for Windows sign-in.'
if ($AllKeys) { Write-Output 'All supported keyboard keys are forwarded to the paired PC.' }
