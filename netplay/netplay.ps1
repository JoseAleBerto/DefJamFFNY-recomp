# Netplay launcher for Def Jam: Fight for NY (Recompiled). Called by "Netplay - Hospedar.bat" and
# "Netplay - Unirse.bat"; works from a source checkout and from an installed copy.
param([ValidateSet('host', 'join')][string]$Mode = 'host')
$ErrorActionPreference = 'Stop'

function Find-Game {
    # A source checkout: this folder's parent holds build\win-x64-release.
    $repo = Split-Path $PSScriptRoot -Parent
    $exe = Join-Path $repo 'build\win-x64-release\defjam_recomp.exe'
    $data = $null
    # An installed copy: installed.ini (UTF-16, [install] source= data= executable=) sits above.
    $dir = $PSScriptRoot
    for ($i = 0; $i -lt 6 -and $dir; $i++) {
        $ini = Join-Path $dir 'installed.ini'
        if (Test-Path $ini) {
            foreach ($line in Get-Content $ini) {
                if ($line -match '^data=(.+)$') { $data = $matches[1].Trim() }
                if ($line -match '^executable=(.+)$') { $exe = $matches[1].Trim() }
            }
            break
        }
        $dir = Split-Path $dir -Parent
    }
    return @{ Exe = $exe; Data = $data; Repo = $repo }
}

$game = Find-Game
if (-not (Test-Path $game.Exe)) {
    Write-Host "No encuentro el juego compilado ($($game.Exe))."
    Read-Host 'Enter para salir'
    exit 1
}
if ($game.Data) { $env:DEFJAM_DATA = $game.Data }
$env:RECOMP_NETPLAY = $Mode
if (-not $env:RECOMP_NETPLAY_NAME) { $env:RECOMP_NETPLAY_NAME = $env:USERNAME }

if ($Mode -eq 'host') {
    $players = Read-Host 'Cuantos jugadores en total, contandote a ti (2-4) [2]'
    if (-not $players) { $players = '2' }
    $env:RECOMP_NETPLAY_PLAYERS = $players
    $ips = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -like '26.*' } | Select-Object -ExpandProperty IPAddress
    Write-Host ''
    if ($ips) { Write-Host "Tu IP de Radmin: $($ips -join ', ')  <- pasasela a los demas" }
    else { Write-Host 'Pasales tu IP de Radmin VPN (la que empieza por 26.).' }
} else {
    $hostIp = Read-Host 'IP de Radmin del que hospeda (ej. 26.12.34.56)'
    if (-not $hostIp) { Write-Host 'Necesito la IP del host.'; Read-Host 'Enter para salir'; exit 1 }
    $env:RECOMP_NETPLAY_ADDR = $hostIp.Trim()
}
Write-Host ''
Write-Host 'Cada uno lleva sus perfiles (User ID) a la partida; en Battle escoge el tuyo.'
Write-Host 'Las partidas guardadas de cada quien no se tocan.'
Write-Host ''
Push-Location (Split-Path $game.Exe -Parent)
try {
    $p = Start-Process -FilePath $game.Exe -WorkingDirectory $game.Repo -NoNewWindow -PassThru
    $p.WaitForExit()
} finally { Pop-Location }
