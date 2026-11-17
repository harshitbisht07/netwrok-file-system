# build.ps1 - build the three Windows .exe files with MinGW-w64 gcc.
#
#   .\build.ps1
#
# Needs gcc from MSYS2 (UCRT64 or MINGW64) or any MinGW-w64 toolchain on PATH.
# Install with:  winget install MSYS2.MSYS2    then in the MSYS2 UCRT64 shell:
#                pacman -S mingw-w64-ucrt-x86_64-gcc
# and add C:\msys64\ucrt64\bin to PATH.
#
# -static links libwinpthread in, so the .exe files run on any Windows 10/11 PC
# with nothing else installed - client.exe can simply be copied to the 2nd PC.

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$gcc = Get-Command gcc -ErrorAction SilentlyContinue
if (-not $gcc) {
    foreach ($p in 'C:\msys64\ucrt64\bin\gcc.exe', 'C:\msys64\mingw64\bin\gcc.exe',
                   'C:\mingw-w64\bin\gcc.exe', 'C:\MinGW\bin\gcc.exe') {
        if (Test-Path $p) { $gcc = Get-Item $p; break }
    }
}
if (-not $gcc) {
    Write-Host "gcc not found. Install MSYS2 and the mingw-w64 gcc package," -ForegroundColor Red
    Write-Host "then add C:\msys64\ucrt64\bin to PATH." -ForegroundColor Red
    exit 1
}
$GCC = $gcc.Source
Write-Host "Using $GCC" -ForegroundColor DarkGray

$cflags = @('-Wall', '-Wextra', '-O2', '-static', '-I', './helper files')
$libs   = @('-lws2_32', '-lbcrypt')

$targets = @(
    @{ Src = './code/Naming_server.c'; Out = 'naming-server.exe'; Thread = $true;  Label = 'Naming Server' },
    @{ Src = './code/SS1.c';           Out = 'server.exe';        Thread = $true;  Label = 'Storage Server' },
    @{ Src = './code/client.c';        Out = 'client.exe';        Thread = $false; Label = 'Client' }
)

foreach ($t in $targets) {
    Write-Host ("==> {0,-15} {1} -> {2}" -f $t.Label, $t.Src, $t.Out)
    $args = @($cflags)
    if ($t.Thread) { $args += '-pthread' }
    $args += @($t.Src, '-o', $t.Out) + $libs
    & $GCC @args
    if ($LASTEXITCODE -ne 0) { Write-Host "Build failed." -ForegroundColor Red; exit 1 }
}

Write-Host ""
Write-Host "Build complete: naming-server.exe, server.exe, client.exe" -ForegroundColor Green
Write-Host ""
Write-Host "Run everything on this PC with:  .\launch_all.ps1" -ForegroundColor Cyan
Write-Host "Or by hand, in three terminals (same secret in each):" -ForegroundColor Cyan
Write-Host '  $env:NFS_SECRET = "a-long-random-string"'
Write-Host '  .\naming-server.exe 0.0.0.0 5566 5568 --remote-rw'
Write-Host '  .\server.exe <this-PC-LAN-IP> 5566 5567 .\storage'
Write-Host '  .\client.exe <this-PC-LAN-IP> 5568'
