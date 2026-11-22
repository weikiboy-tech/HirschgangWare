[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ListenAddress,
    [ValidateRange(1024, 65535)][int]$Port = 47832,
    [ValidatePattern('^(?:All|[A-Z0-9:, ]+|[A-Z0-9:, ]*Escape[A-Z0-9:, ]*)$')][string]$Keys = 'E,Y',
    [switch]$OpenFirewall,
    [ValidateSet('Private', 'Public')][string]$FirewallProfile = 'Private'
)

$ErrorActionPreference = 'Stop'
if ($Keys -eq 'All') { $Keys = 'All' }
$address = [System.Net.IPAddress]::Parse($ListenAddress)
$bytes = $address.GetAddressBytes()
if ($bytes.Length -ne 4 -or -not ($bytes[0] -eq 10 -or
    ($bytes[0] -eq 172 -and $bytes[1] -ge 16 -and $bytes[1] -le 31) -or
    ($bytes[0] -eq 192 -and $bytes[1] -eq 168) -or
    ($bytes[0] -eq 169 -and $bytes[1] -eq 254))) {
    throw 'Specify the second PC private IPv4 address.'
}
$ListenAddress = $address.ToString()
$build = Join-Path $PSScriptRoot '..\build'
$receiver = Join-Path $build 'HirschgangWare.exe'
$sender = Join-Path $build 'HirschgangWare-InputSender.exe'
$updater = Join-Path $build 'HirschgangWare-InputUpdater.exe'
if (!(Test-Path -LiteralPath $receiver) -or !(Test-Path -LiteralPath $sender) -or !(Test-Path -LiteralPath $updater)) {
    throw 'Build HirschgangWare and the input sender first.'
}
$receiver = (Resolve-Path -LiteralPath $receiver).Path
$build = Split-Path -Parent $receiver

if ($OpenFirewall) {
    $nic = Get-NetIPAddress -IPAddress $ListenAddress -AddressFamily IPv4 | Select-Object -First 1
    if (!$nic) { throw 'The selected IP does not belong to this PC.' }
    $rule = 'HirschgangWareLanKeyboard-' + $ListenAddress + '-' + $Port + '-' + $FirewallProfile
    if (!(Get-NetFirewallRule -Name $rule -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -Name $rule -DisplayName 'HirschgangWare LAN keyboard' -Direction Inbound `
            -Action Allow -Profile $FirewallProfile -Protocol TCP -LocalPort $Port -LocalAddress $ListenAddress `
            -RemoteAddress LocalSubnet -InterfaceAlias $nic.InterfaceAlias -Program $receiver | Out-Null
    }
}

$receiverIni = Join-Path $build 'lan-keyboard-receiver.ini'
$key = $null
if (Test-Path -LiteralPath $receiverIni) {
    $existingKey = Select-String -LiteralPath $receiverIni -Pattern '^key=([a-fA-F0-9]{64})$' | Select-Object -First 1
    if (!$existingKey) { throw 'Existing LAN keyboard configuration has no valid key.' }
    $key = $existingKey.Matches[0].Groups[1].Value
} else {
    $random = New-Object byte[] 32
    $generator = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try { $generator.GetBytes($random) } finally { $generator.Dispose() }
    $key = ([System.BitConverter]::ToString($random)).Replace('-', '').ToLowerInvariant()
}

@('[lan_keyboard]', "listen=$ListenAddress", "port=$Port", "key=$key") |
    Set-Content -LiteralPath $receiverIni -Encoding ascii
$bundle = Join-Path $build 'lan-keyboard-main-pc'
New-Item -ItemType Directory -Force -Path $bundle | Out-Null
Copy-Item -LiteralPath $sender -Destination $bundle -Force
Copy-Item -LiteralPath $updater -Destination $bundle -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'install_lan_keyboard_sender.ps1') -Destination $bundle -Force
@('[lan_keyboard]', "receiver=$ListenAddress", "port=$Port", "key=$key", "keys=$Keys") |
    Set-Content -LiteralPath (Join-Path $bundle 'lan-keyboard-sender.ini') -Encoding ascii
$zip = Join-Path $build 'HirschgangWare-LAN-MainPC.zip'
Compress-Archive -Path (Join-Path $bundle '*') -DestinationPath $zip -Force
Write-Output "Main-PC package: $zip"
