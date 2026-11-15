# Gathers everything needed to find out what went wrong into one .zip on the desktop.
# Never includes the game's files or the saves.
$ErrorActionPreference = 'Continue'
$repo = Split-Path $PSScriptRoot -Parent
$data = $null
$dir = $PSScriptRoot
for ($i = 0; $i -lt 6 -and $dir; $i++) {
    $ini = Join-Path $dir 'installed.ini'
    if (Test-Path $ini) {
        foreach ($line in Get-Content $ini) { if ($line -match '^data=(.+)$') { $data = $matches[1].Trim() } }
        break
    }
    $dir = Split-Path $dir -Parent
}
if (-not $data) {
    $link = Join-Path $repo 'game'
    if (Test-Path $link) { try { $data = Split-Path (@((Get-Item $link).Target)[0]) -Parent } catch {} }
}
$logs = Join-Path $repo 'logs'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$work = Join-Path $env:TEMP "defjam-reporte-$stamp"
New-Item -ItemType Directory -Force -Path $work | Out-Null

# The newest runs: game logs, launcher transcripts, netplay files.
if (Test-Path $logs) {
    Get-ChildItem $logs -File -Filter 'run-*.log*' | Sort-Object LastWriteTime -Descending | Select-Object -First 6 |
        Copy-Item -Destination $work
    Get-ChildItem $logs -File -Filter 'launcher-*.txt' | Sort-Object LastWriteTime -Descending | Select-Object -First 4 |
        Copy-Item -Destination $work
    Get-ChildItem $logs -File -Filter 'netplay-*' | Where-Object { $_.LastWriteTime -gt (Get-Date).AddDays(-2) } |
        Copy-Item -Destination $work
    # The newest play-test session (a prueba de errores), without the long sound recording.
    $latest = Join-Path $logs 'sessions\LATEST'
    if (Test-Path $latest) {
        $name = (Get-Content $latest -TotalCount 1).Trim()
        $session = if (Test-Path $name) { $name } else { Join-Path $logs "sessions\$name" }
        if (Test-Path $session) {
            $dest = Join-Path $work ("session-" + (Split-Path $session -Leaf))
            Copy-Item $session -Destination $dest -Recurse -Exclude 'audio.pcm'
        }
    }
}
if ($data -and (Test-Path (Join-Path $data 'settings.ini'))) { Copy-Item (Join-Path $data 'settings.ini') $work }

# The machine.
$info = @()
$info += "Fecha: $(Get-Date)"
$info += "PC: $env:COMPUTERNAME  Usuario: $env:USERNAME"
try { $os = Get-CimInstance Win32_OperatingSystem; $info += "Windows: $($os.Caption) $($os.Version)  RAM: $([math]::Round($os.TotalVisibleMemorySize/1MB,1)) GB" } catch {}
try { Get-CimInstance Win32_Processor | ForEach-Object { $info += "CPU: $($_.Name)" } } catch {}
try { Get-CimInstance Win32_VideoController | ForEach-Object { $info += "GPU: $($_.Name) driver $($_.DriverVersion)" } } catch {}
try {
    Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.IPAddress -like '26.*' } |
        ForEach-Object { $info += "Radmin: $($_.IPAddress) ($($_.InterfaceAlias))" }
} catch {}
$exe = Join-Path $repo 'build\win-x64-release\defjam_recomp.exe'
if (Test-Path $exe) { $info += "Juego: $exe  compilado $((Get-Item $exe).LastWriteTime)  $((Get-Item $exe).Length) bytes" }
foreach ($f in 'installed.json') { $p = Join-Path (Split-Path $repo -Parent) $f; if (Test-Path $p) { Copy-Item $p $work } }
$info | Out-File -Encoding utf8 (Join-Path $work 'equipo.txt')

$zip = Join-Path ([Environment]::GetFolderPath('Desktop')) "DefJam-reporte-$env:COMPUTERNAME-$stamp.zip"
Compress-Archive -Path (Join-Path $work '*') -DestinationPath $zip -Force
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
Write-Host ''
Write-Host "Listo: $zip"
Write-Host 'Manda ese archivo para revisar que paso.'
