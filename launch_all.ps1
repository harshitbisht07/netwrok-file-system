# Launch all NFS components, each in its own window, through WSL.
# (The programs are Linux binaries, so Windows needs WSL to run them.
#  The old version pointed at start_*.ps1 files that never existed.)
#
# Other PCs reach this one through its LAN IP, so the storage server must
# register with that IP (not 127.0.0.1): the naming server hands out the
# address it saw the storage server connect from.
# Needs networkingMode=mirrored in %UserProfile%\.wslconfig so WSL shares
# the Windows IP. Override the detected IP with:  $env:NFS_HOST_IP = '...'

$scriptPath = Split-Path -Parent $MyInvocation.MyCommand.Path

# This PC's LAN IP: the adapter that has a default gateway
# (skips VirtualBox/Hyper-V host-only adapters like 192.168.56.1).
$hostIp = $env:NFS_HOST_IP
if (-not $hostIp) {
    $hostIp = (Get-NetIPConfiguration |
        Where-Object { $_.IPv4DefaultGateway -and $_.NetAdapter.Status -eq 'Up' } |
        Select-Object -First 1).IPv4Address.IPAddress
}
if (-not $hostIp) {
    Write-Host "Could not detect this PC's LAN IP. Set it with: `$env:NFS_HOST_IP = '192.168.x.x'" -ForegroundColor Red
    exit 1
}

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    Write-Host "WSL is required (the binaries are Linux programs). Install it with: wsl --install" -ForegroundColor Red
    exit 1
}

# Shared secret: use $env:NFS_SECRET if set, otherwise generate a random one.
$secret = $env:NFS_SECRET
if (-not $secret) {
    $bytes = New-Object byte[] 16
    [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $secret = ($bytes | ForEach-Object { $_.ToString('x2') }) -join ''
}
Write-Host "This PC's LAN IP: $hostIp" -ForegroundColor Yellow
Write-Host "Shared secret for this session: $secret" -ForegroundColor Yellow

# Build (inside WSL) if any executable is missing
wsl.exe --cd "$scriptPath" bash -c "[ -x ./naming-server ] && [ -x ./server ] && [ -x ./client ] || bash bash.sh"
if ($LASTEXITCODE -ne 0) { Write-Host "Build failed." -ForegroundColor Red; exit 1 }

function Start-Component([string]$cmd) {
    $inner = "export NFS_SECRET='$secret'; $cmd; echo; echo 'Press Enter to close...'; read"
    Start-Process wsl.exe -ArgumentList @('--cd', "`"$scriptPath`"", 'bash', '-c', "`"$inner`"")
}

Write-Host "[1/3] Launching Naming Server..." -ForegroundColor Green
Start-Component "./naming-server 0.0.0.0"
Start-Sleep -Seconds 2

Write-Host "[2/3] Launching Storage Server..." -ForegroundColor Green
Start-Component "./server $hostIp 5566 5567 ./storage"
Start-Sleep -Seconds 2

Write-Host "[3/3] Launching Client..." -ForegroundColor Green
Start-Component "./client $hostIp 5568"

Write-Host ""
Write-Host "Launched 3 windows: Naming Server (5566/5568), Storage Server (5567), Client." -ForegroundColor Green
Write-Host ""
Write-Host "On the other PC (inside WSL, in this project folder):" -ForegroundColor Cyan
Write-Host "  NFS_SECRET='$secret' ./client $hostIp 5568" -ForegroundColor Cyan
