# Windows installer packaging -> dist\windows\matrix-player-setup-<version>.exe
# Thin driver, same scripts/<platform>/ convention as build.ps1: runs the
# Release build, stages the exact runtime file set (never a raw copy of
# build\ — see the comment below), then hands the stage off to Inno Setup.
#
# Usage: scripts\windows\package.ps1 [-SkipBuild] [-IsccPath <path>]
param(
    [switch]$SkipBuild,
    [string]$IsccPath
)

$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..\..")

if (-not $SkipBuild) {
    Write-Host "Building Release..."
    & (Join-Path $PSScriptRoot "build.ps1")
    if ($LASTEXITCODE -ne 0) { throw "Release build failed." }
}

$BuildDir = "build\gui"
if (-not (Test-Path (Join-Path $BuildDir "matrix_player.exe"))) {
    throw "$BuildDir\matrix_player.exe not found. Run without -SkipBuild, or run scripts\windows\build.ps1 first."
}

$Manifest = Get-Content "manifest.json" -Raw | ConvertFrom-Json
$Version = $Manifest.version
Write-Host "Packaging Matrix Player v$Version..."

# Explicit allowlist copy into a scratch directory — deliberately never a
# raw copy of build\. A build tree that has actually been run locally (for
# dev testing) accumulates matrix_player.db, matrix_player.log, and a glyph
# atlas cache next to the exe; none of those are build outputs, and a plain
# directory copy would silently ship whoever packaged the installer's own
# local listening history and library scan inside it.
$StageDir = "dist\windows\stage"
if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null

$FilesToStage = @(
    "matrix_player.exe",
    "libc++.dll",
    "libgcc_s_seh-1.dll",
    "libwinpthread-1.dll",
    "eq_profiles.json"
)
foreach ($f in $FilesToStage) {
    $src = Join-Path $BuildDir $f
    if (-not (Test-Path $src)) { throw "Expected build output missing: $src" }
    Copy-Item $src (Join-Path $StageDir $f)
}
foreach ($d in @("assets", "fonts")) {
    $src = Join-Path $BuildDir $d
    if (-not (Test-Path $src)) { throw "Expected build output missing: $src" }
    Copy-Item $src (Join-Path $StageDir $d) -Recurse
}
Write-Host "Staged $($FilesToStage.Count) files + assets\ + fonts\ into $StageDir"

if (-not $IsccPath) {
    # winget's default scope varies by machine (some install per-user under
    # LOCALAPPDATA\Programs, some per-machine under Program Files) — check
    # all three rather than assume one.
    $candidates = @(
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
    )
    $IsccPath = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $IsccPath) {
        $cmd = Get-Command iscc.exe -ErrorAction SilentlyContinue
        if ($cmd) { $IsccPath = $cmd.Source }
    }
}
if (-not $IsccPath -or -not (Test-Path $IsccPath)) {
    throw "Inno Setup's ISCC.exe not found. Install it (winget install --id JRSoftware.InnoSetup -e, or https://jrsoftware.org/isdl.php), or pass -IsccPath explicitly."
}

$OutDir = "dist\windows"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$StageDirAbs = (Resolve-Path $StageDir).Path
$OutDirAbs = (Resolve-Path $OutDir).Path

Write-Host "Compiling installer with Inno Setup..."
& $IsccPath "/DMyAppVersion=$Version" "/DMyStageDir=$StageDirAbs" "/DMyOutDir=$OutDirAbs" "packaging\windows\matrix-player.iss"
if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed." }

Write-Host ""
Write-Host "Success! Output: $OutDir\matrix-player-setup-$Version.exe"
