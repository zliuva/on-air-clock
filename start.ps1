$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot 'dist\OnAirClock.exe'
if (-not (Test-Path -LiteralPath $exe)) { & (Join-Path $PSScriptRoot 'build.ps1') }
Start-Process -FilePath $exe -ArgumentList @('--data-dir', ('"' + (Join-Path $PSScriptRoot 'data') + '"')) -WorkingDirectory $PSScriptRoot -WindowStyle Hidden
