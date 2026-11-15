# Launcher for Def Jam: Fight for NY (Recompiled): netplay and "a prueba de errores" runs.
# Called by the .bat files beside it; works from a source checkout and from an installed copy.
#   -Mode host | join | solo     -Diag: record everything (play-test recorder + netplay diagnostics)
param([ValidateSet('host', 'join', 'solo')][string]$Mode = 'host', [switch]$Diag)
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
$logs = Join-Path $game.Repo 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
try { Start-Transcript -Path (Join-Path $logs "launcher-$stamp.txt") | Out-Null } catch {}
try {
    if (-not (Test-Path $game.Exe)) {
        Write-Host "No encuentro el juego compilado ($($game.Exe))."
        Read-Host 'Enter para salir'
        exit 1
    }
    if ($game.Data) { $env:DEFJAM_DATA = $game.Data }
    if ($Diag) {
        # The play-test recorder: crash/hang bundles with a minidump, the last seconds of pictures,
        # events and timings (docs/playtest-recorder.md). Fewer pictures a second to keep it light.
        $env:RECOMP_RECORD = '1'
        if (-not $env:RECOMP_RECORD_FPS) { $env:RECOMP_RECORD_FPS = '5' }
        $env:RECOMP_NETPLAY_DIAG = '1'
        # Registro de golpes: que movimiento salio y que teclas lo hicieron (logs\run-*.log.err, lineas [GOLPE]).
        $env:RECOMP_MOVE_LOG = '1'
        Write-Host 'MODO A PRUEBA DE ERRORES: se registra todo en la carpeta logs.'
        Write-Host 'Si algo raro pasa, aprieta F9 (o manten View + clic del stick izquierdo) para guardar ese momento.'
        Write-Host 'Al terminar, abre "Recoger reporte.bat" y manda el .zip que deja en el escritorio.'
        Write-Host ''
    }
    if ($Mode -ne 'solo') {
        $env:RECOMP_NETPLAY = $Mode
        if (-not $env:RECOMP_NETPLAY_NAME) { $env:RECOMP_NETPLAY_NAME = $env:USERNAME }
    }
    if ($Mode -eq 'host') {
        $players = Read-Host 'Cuantos jugadores en total, contandote a ti (2-4) [2]'
        if (-not $players) { $players = '2' }
        $env:RECOMP_NETPLAY_PLAYERS = $players
        $ips = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
            Where-Object { $_.IPAddress -like '26.*' } | Select-Object -ExpandProperty IPAddress
        Write-Host ''
        if ($ips) { Write-Host "Tu IP de Radmin: $($ips -join ', ')  <- pasasela a los demas" }
        else { Write-Host 'Pasales tu IP de Radmin VPN (la que empieza por 26.).' }
    } elseif ($Mode -eq 'join') {
        $hostIp = Read-Host 'IP de Radmin del que hospeda (ej. 26.12.34.56)'
        if (-not $hostIp) { Write-Host 'Necesito la IP del host.'; Read-Host 'Enter para salir'; exit 1 }
        $env:RECOMP_NETPLAY_ADDR = $hostIp.Trim()
    }
    if ($Mode -ne 'solo') {
        Write-Host ''
        Write-Host 'Cada uno lleva sus perfiles (User ID) a la partida; en Battle escoge el tuyo.'
        Write-Host 'Las partidas guardadas de cada quien no se tocan.'
        Write-Host ''
    }
    # The game's own log, as a double click would make it (logs\run-*.log / .log.err).
    $out = Join-Path $logs "run-$stamp.log"
    $p = Start-Process -FilePath $game.Exe -WorkingDirectory $game.Repo -NoNewWindow -PassThru `
        -RedirectStandardOutput $out -RedirectStandardError "$out.err"
    $null = $p.Handle      # keeps the exit code readable after the wait (PowerShell 5.1)
    $p.WaitForExit()
    Write-Host ''
    if ($p.ExitCode -ne 0) {
        Write-Host "El juego se cerro con un error (codigo $($p.ExitCode))."
        Write-Host 'Abre "Recoger reporte.bat" y manda el .zip que deja en el escritorio.'
    } else {
        Write-Host 'Juego cerrado.'
    }
} finally {
    try { Stop-Transcript | Out-Null } catch {}
}
