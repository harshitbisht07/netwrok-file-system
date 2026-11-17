# launch_all.ps1 - start the whole file system on THIS PC (the primary one):
#                  Naming Server + Storage Server + a local Client,
#                  each in its own window.  Native .exe files, no WSL.
#
#   .\launch_all.ps1                     # normal run
#   .\launch_all.ps1 -OpenFirewall       # also add the inbound firewall rules (admin)
#   .\launch_all.ps1 -RemoteFullAccess   # other PCs get delete/copy too
#   .\launch_all.ps1 -Secret 'my-shared-secret'
#
# Other PCs reach this one through its LAN IP, so the storage server registers
# with that IP (not 127.0.0.1): the naming server hands clients the address it
# saw the storage server connect from.

param(
    [string] $Secret,
    [string] $HostIp,
    [switch] $OpenFirewall,
    [switch] $RemoteFullAccess
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

# ---- this PC's LAN IP: the adapter that has a default gateway ----------
# (skips VirtualBox / Hyper-V host-only adapters such as 192.168.56.1)
if (-not $HostIp) { $HostIp = $env:NFS_HOST_IP }
if (-not $HostIp) {
    $HostIp = (Get-NetIPConfiguration |
        Where-Object { $_.IPv4DefaultGateway -and $_.NetAdapter.Status -eq 'Up' } |
        Select-Object -First 1).IPv4Address.IPAddress
}
if (-not $HostIp) {
    Write-Host "Could not detect this PC's LAN IP. Pass it: .\launch_all.ps1 -HostIp 192.168.x.x" -ForegroundColor Red
    exit 1
}

# ---- build if needed --------------------------------------------------
foreach ($exe in 'naming-server.exe', 'server.exe', 'client.exe') {
    if (-not (Test-Path $exe)) { Write-Host "Building..." -ForegroundColor Yellow; & .\build.ps1; break }
}

# ---- shared secret ---------------------------------------------------
if (-not $Secret) { $Secret = $env:NFS_SECRET }
if (-not $Secret) {
    $bytes = New-Object byte[] 16
    [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $Secret = ($bytes | ForEach-Object { $_.ToString('x2') }) -join ''
}

# ---- firewall (inbound 5566 / 5567 / 5568) ---------------------------
if ($OpenFirewall) {
    try {
        foreach ($p in 5566, 5567, 5568) {
            $name = "NFS-DFS-$p"
            if (-not (Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue)) {
                New-NetFirewallRule -DisplayName $name -Direction Inbound -Action Allow `
                    -Protocol TCP -LocalPort $p -Profile Any | Out-Null
            }
        }
        Write-Host "Firewall: inbound TCP 5566, 5567, 5568 allowed." -ForegroundColor Green
    } catch {
        Write-Host "Could not add the firewall rules - run PowerShell as Administrator for -OpenFirewall." -ForegroundColor Red
    }
}

$policy = if ($RemoteFullAccess) { '--full-access' } else { '--remote-rw' }

Write-Host ""
Write-Host "  This PC's LAN IP : $HostIp" -ForegroundColor Yellow
Write-Host "  Shared secret    : $Secret" -ForegroundColor Yellow
Write-Host "  Other PCs        : $(if ($RemoteFullAccess) {'full access'} else {'read / write only (no delete, no copy)'})" -ForegroundColor Yellow
Write-Host ""

function Start-Component([string]$title, [string]$cmd) {
    $inner = "`$host.UI.RawUI.WindowTitle = '$title'; " +
             "`$env:NFS_SECRET = '$Secret'; " +
             "Set-Location '$PSScriptRoot'; $cmd"
    Start-Process powershell -ArgumentList '-NoExit', '-Command', $inner | Out-Null
}

Write-Host "[1/3] Naming Server  (SS 5566, clients 5568)" -ForegroundColor Green
Start-Component 'NFS - Naming Server' ".\naming-server.exe 0.0.0.0 5566 5568 $policy"
Start-Sleep -Seconds 2

Write-Host "[2/3] Storage Server (clients 5567, storage .\storage)" -ForegroundColor Green
Start-Component 'NFS - Storage Server' ".\server.exe $HostIp 5566 5567 .\storage"
Start-Sleep -Seconds 2

Write-Host "[3/3] Client" -ForegroundColor Green
Start-Component 'NFS - Client' ".\client.exe $HostIp 5568"

Write-Host ""
Write-Host "Three windows opened. On the OTHER PC, copy client.exe over and run:" -ForegroundColor Cyan
Write-Host "  `$env:NFS_SECRET = '$Secret'" -ForegroundColor White
Write-Host "  .\client.exe $HostIp 5568" -ForegroundColor White
Write-Host ""
Write-Host "Stop everything by closing the windows (or Ctrl+C in each)." -ForegroundColor DarkGray
