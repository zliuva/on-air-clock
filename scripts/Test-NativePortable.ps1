$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root 'dist\OnAirClock.exe'
$isolated = Join-Path $root '.build-cache\portable-smoke'
New-Item -ItemType Directory -Path $isolated -Force | Out-Null
$exe = Join-Path $isolated 'OnAirClock.exe'
Copy-Item -LiteralPath $source -Destination $exe -Force
$info = [Diagnostics.ProcessStartInfo]::new($exe, '--self-test')
$info.WorkingDirectory = $isolated
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardOutput = $true
$info.Environment['PATH'] = Join-Path $env:SystemRoot 'System32'
$info.Environment['DOTNET_ROOT'] = Join-Path $isolated 'no-dotnet-installed'
$info.Environment['DOTNET_ROOT_X64'] = $info.Environment['DOTNET_ROOT']
$info.Environment['DOTNET_BUNDLE_EXTRACT_BASE_DIR'] = Join-Path $isolated 'no-extraction'
$process = [Diagnostics.Process]::Start($info)
$text = $process.StandardOutput.ReadToEnd()
$process.WaitForExit()
Write-Output $text
if ($process.ExitCode -ne 0) { throw 'Copied single EXE failed its isolated self-test.' }
$files = @(Get-ChildItem -LiteralPath $isolated -Force)
if ($files.Count -ne 1 -or $files[0].Name -ne 'OnAirClock.exe') { throw 'Unexpected files extracted by the portable EXE.' }
Write-Output 'PASS copied EXE runs with only Windows on PATH; no runtime or payload was extracted.'
