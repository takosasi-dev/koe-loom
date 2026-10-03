# KoeLoom build helper (ASCII only: Windows PowerShell 5.1 reads BOM-less files as CP932).
#
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1                 # configure + build Release + copy to dist\
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Test           # ... then run --run-tests
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Test -Category Effects
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Name wt-effects-a   (separate build tree)
#
# The source must be reached through an ASCII path (JUCE's build steps break on some characters),
# so this script resolves its own repo root and expects it to be ASCII (a worktree, or a junction named koe-loom
# at the root of the same drive, made once with: mklink /J X:\koe-loom "<repo>").

param(
    [string]$Name = "app",
    [string]$Config = "Release",
    [switch]$Test,
    [string]$Category = "",
    [switch]$NoDist
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if ($repo -match '[^\x00-\x7F]') {
    $repo = Join-Path (Split-Path -Qualifier $repo) "koe-loom"
    if (-not (Test-Path $repo)) { throw "repo path is not ASCII; make the junction first: mklink /J $repo <repo>" }
    Write-Host "repo path is not ASCII, using junction $repo"
}
$build = Join-Path $env:LOCALAPPDATA "KoeLoom\build\$Name"

cmake -S $repo -B $build -G "Visual Studio 17 2022" -A x64 | Out-Host
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
cmake --build $build --config $Config --parallel | Out-Host
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$exe = Join-Path $build "KoeLoom_artefacts\$Config\KoeLoom.exe"
if (-not (Test-Path $exe)) { throw "exe not found: $exe" }

if (-not $NoDist) {
    $dist = Join-Path $repo "dist"
    New-Item -ItemType Directory -Force $dist | Out-Null
    try { Copy-Item $exe (Join-Path $dist "KoeLoom.exe") -Force } catch { Write-Host "dist copy skipped (exe in use?)" }
}

if ($Test) {
    $ref = Join-Path $repo "phase0\testdata\reference-speech.wav"
    if (Test-Path $ref) { $env:KOELOOM_REFERENCE_WAV = $ref }
    $env:KOELOOM_DATA_DIR = Join-Path $env:TEMP "KoeLoomTests"
    $args = @("--run-tests")
    if ($Category -ne "") { $args += $Category }
    $p = Start-Process -FilePath $exe -ArgumentList $args -Wait -PassThru -NoNewWindow
    $results = Join-Path (Split-Path $exe) "test-results.txt"
    if (Test-Path $results) { Get-Content $results | Select-Object -Last 40 | Out-Host }
    if ($p.ExitCode -ne 0) { throw "tests failed (exit $($p.ExitCode)); see $results" }
    Write-Host "tests passed"
}
Write-Host "exe: $exe"
