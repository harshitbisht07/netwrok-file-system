# start_client.ps1 - run on the SECOND PC (the one that only has the client).
#
# Copy client.exe and this script into any folder on that PC, then:
#
#   .\start_client.ps1 -ServerIp 192.168.1.20 -Secret 'the-same-secret'
#
# ServerIp is the LAN IP of the primary PC (the one running launch_all.ps1 -
# it prints both the IP and the secret when it starts).

param(
    [Parameter(Mandatory = $true)][string] $ServerIp,
    [string] $Secret,
    [int]    $Port = 5568
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (-not (Test-Path .\client.exe)) {
    Write-Host "client.exe is not in this folder - copy it from the primary PC." -ForegroundColor Red
    exit 1
}

Write-Host "Checking that $ServerIp`:$Port is reachable..." -ForegroundColor DarkGray
if (-not (Test-NetConnection -ComputerName $ServerIp -Port $Port -InformationLevel Quiet -WarningAction SilentlyContinue -ErrorAction SilentlyContinue)) {
    Write-Host "Cannot reach $ServerIp on TCP $Port." -ForegroundColor Red
    Write-Host "On the primary PC: is the naming server running, and is the firewall open?" -ForegroundColor Yellow
    Write-Host "  (run there, as Administrator:  .\launch_all.ps1 -OpenFirewall )" -ForegroundColor Yellow
    exit 1
}

if ($Secret) { $env:NFS_SECRET = $Secret }
.\client.exe $ServerIp $Port
