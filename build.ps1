param([switch]$Test)
$ErrorActionPreference = 'Stop'
$toolRoot = Join-Path $PSScriptRoot '.build-cache\tools'
$version = '0.17.0'
$zig = Join-Path $toolRoot "zig-x86_64-windows-$version\zig.exe"
if (-not (Test-Path -LiteralPath $zig)) {
    New-Item -ItemType Directory -Path $toolRoot -Force | Out-Null
    $archive = Join-Path $toolRoot "zig-$version.zip"
    if (-not (Test-Path -LiteralPath $archive)) { Invoke-WebRequest "https://ziglang.org/download/$version/zig-x86_64-windows-$version.zip" -OutFile $archive }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428') { throw 'Toolchain SHA256 mismatch.' }
    Expand-Archive -LiteralPath $archive -DestinationPath $toolRoot
}
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $PSScriptRoot '.build-cache\zig-global'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $PSScriptRoot '.build-cache\zig-local'
$stage = Join-Path $PSScriptRoot '.build-cache\native'
$dist = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Path $stage,$dist -Force | Out-Null
Push-Location $PSScriptRoot
try {
    & $zig rc /fo (Join-Path $stage 'resources.res') 'native\resources.rc'
    if ($LASTEXITCODE -ne 0) { throw 'Resource build failed.' }
    & $zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -static -municode '-Wl,--subsystem,windows' 'native\core.cpp' 'native\app.cpp' 'native\selftest.cpp' (Join-Path $stage 'resources.res') -o (Join-Path $stage 'OnAirClock.exe') -luser32 -lgdi32 -lshell32 -lole32 -ladvapi32 -lcrypt32 -lwinhttp -lcomctl32 -luuid -s
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
    Copy-Item -LiteralPath (Join-Path $stage 'OnAirClock.exe') -Destination (Join-Path $dist 'OnAirClock.exe') -Force
    Get-Item (Join-Path $dist 'OnAirClock.exe') | Select-Object FullName,Length
    if ($Test) {
        $testProcess = Start-Process -FilePath (Join-Path $dist 'OnAirClock.exe') -ArgumentList '--self-test' -Wait -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $stage 'self-test.txt')
        Get-Content (Join-Path $stage 'self-test.txt')
        if ($testProcess.ExitCode -ne 0) { throw 'Native tests failed.' }
    }
} finally { Pop-Location }
