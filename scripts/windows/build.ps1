# Desktop Windows build -> <repo>\build (Release) or \build_debug (Debug)
# Thin launcher: locates MSVC via vswhere, then cmake+ninja does everything —
# no source lives here. Replaces the old root build.bat 1:1 in behavior,
# minus its interactive prompt (pass -Debug/-Release explicitly; default is
# Release), matching this repo's scripts/<platform>/ convention.
#
# Usage: scripts\windows\build.ps1 [-Debug] [-Clean] [-V3] [-V4]
[CmdletBinding()]
param(
    [switch]$Debug,
    [switch]$Clean,
    [switch]$V3,   # x86-64-v3 equivalent (AVX2, FMA3, BMI2)
    [switch]$V4    # x86-64-v4 equivalent (AVX-512)
)

$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..\..")

$BuildType = if ($Debug) { "Debug" } else { "Release" }
# Ninja is a single-config generator: flipping CMAKE_BUILD_TYPE in the same
# directory forces a near-total recompile (Debug/Release use incompatible
# runtime libraries). Separate directories keep switching back and forth fast.
$BuildDir = if ($Debug) { "build_debug" } else { "build" }

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning $BuildDir..."
    Remove-Item -Recurse -Force $BuildDir
}

$ArchFlags = @()
if ($V3) { $ArchFlags = @("-DCMAKE_CXX_FLAGS=/arch:AVX2", "-DCMAKE_C_FLAGS=/arch:AVX2") }
if ($V4) { $ArchFlags = @("-DCMAKE_CXX_FLAGS=/arch:AVX512", "-DCMAKE_C_FLAGS=/arch:AVX512") }

Write-Host "Locating MSVC environment (vswhere)..."
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) { throw "Visual Studio or C++ Build Tools not found via vswhere." }

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "Found VS at '$vsPath', but vcvars64.bat is missing." }

# Import vcvars64's environment (PATH/INCLUDE/LIB) into this PowerShell session.
cmd /c "call `"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match "^(.*?)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2])
    }
}

if (Get-Command git -ErrorAction SilentlyContinue) {
    Write-Host "Updating submodules..."
    git submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { throw "git submodule update failed." }
} else {
    Write-Warning "git not found in PATH — skipping submodule update."
}

Write-Host "Configuring CMake (Ninja, $BuildType)..."
cmake -G Ninja -B $BuildDir -DCMAKE_BUILD_TYPE=$BuildType @ArchFlags
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }

Write-Host "Building..."
cmake --build $BuildDir --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

Write-Host ""
Write-Host "Success! Output: $BuildDir\matrix_player.exe"
