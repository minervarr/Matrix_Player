# Desktop Windows build -> <repo>\build (Release) or \build_debug (Debug)
# Thin launcher: locates MSYS2's UCRT64 Clang toolchain, then cmake+ninja does
# everything - no source lives here. Matches this repo's scripts/<platform>/
# convention.
#
# Toolchain is MSYS2 UCRT64 Clang (clang.exe/clang++.exe targeting
# x86_64-w64-windows-gnu - NOT clang-cl), not MSVC. Root CMakeLists.txt
# already routes MATRIX_ARCH_LEVEL/-fno-math-errno/-ffp-contract=off through
# this compiler correctly (gated on NOT MSVC, which Clang invoked this way
# satisfies), and statically links the MinGW/UCRT runtime (-static
# -static-libgcc -static-libstdc++) plus -stdlib=libc++ globally for WIN32 -
# see the comments there. This script only needs to point CMake at the
# compiler; it doesn't need to know about any of that.
#
# Usage: scripts\windows\build.ps1 [-Debug] [-Clean] [-V3] [-V4] [-Msys2Root <path>]
#
# Deliberately no [CmdletBinding()]: it auto-adds a reserved -Debug common
# parameter, which collides with this script's own [switch]$Debug and made
# every invocation with -Debug fail outright ("parameter... defined multiple
# times") - a latent bug in the original script that nothing had ever
# actually run before this port.
param(
    [switch]$Debug,
    [switch]$Clean,
    [switch]$V3,   # x86-64-v3 equivalent (AVX2, FMA3, BMI2)
    [switch]$V4,   # x86-64-v4 equivalent (AVX-512)
    [string]$Msys2Root = "C:\msys64"
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

# -V3/-V4 route through MATRIX_ARCH_LEVEL (root CMakeLists.txt), not a raw
# -march=/arch: flag - that mechanism already validates the compiler actually
# supports the requested -march=x86-64-v3/v4 (check_cxx_compiler_flag) and
# fails loudly instead of silently ignoring an unsupported request.
$ArchFlags = @()
if ($V3) { $ArchFlags = @("-DMATRIX_ARCH_LEVEL=v3") }
if ($V4) { $ArchFlags = @("-DMATRIX_ARCH_LEVEL=v4") }

Write-Host "Locating MSYS2 UCRT64 Clang..."
$Ucrt64Bin = Join-Path $Msys2Root "ucrt64\bin"
# Forward slashes: CMake writes these paths verbatim into generated files
# like CMakeRCCompiler.cmake, where a backslash is parsed as an escape
# character - "C:\msys64\...\windres.exe" broke with "Invalid character
# escape '\m'" the first time this hit a truly fresh build dir (build_debug
# had already cached its toolchain files from earlier manual cmake calls
# with different quoting, so this went unnoticed until the first real
# Release configure). CMake accepts forward slashes on Windows natively.
$ClangExe = (Join-Path $Ucrt64Bin "clang.exe") -replace '\\', '/'
$ClangxxExe = (Join-Path $Ucrt64Bin "clang++.exe") -replace '\\', '/'
$WindresExe = (Join-Path $Ucrt64Bin "windres.exe") -replace '\\', '/'

if (-not (Test-Path $ClangxxExe)) {
    $installHint = "pacman -S mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-libc++"
    $msg = "MSYS2 UCRT64 Clang not found at '$ClangxxExe'. Install MSYS2 (https://www.msys2.org/), then from an MSYS2 UCRT64 shell run: $installHint . Or pass -Msys2Root if MSYS2 is installed somewhere other than '$Msys2Root'."
    throw $msg
}

# CMake/Ninja: prefer MSYS2's own (same UCRT64 bin dir) if present, else
# whatever's already on PATH (a standalone cmake/ninja install works fine -
# only the C/C++/RC compiler needs pointing at UCRT64 explicitly, since
# that's the part that isn't autodetectable/on PATH by default).
if ($env:PATH -notlike "*$Ucrt64Bin*") {
    $env:PATH = "$Ucrt64Bin;$env:PATH"
}

if (Get-Command git -ErrorAction SilentlyContinue) {
    Write-Host "Updating submodules..."
    git submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { throw "git submodule update failed." }
} else {
    Write-Warning "git not found in PATH - skipping submodule update."
}

Write-Host "Configuring CMake (Ninja, $BuildType, Clang/UCRT64)..."
$ConfigureArgs = @(
    "-G", "Ninja",
    "-B", $BuildDir,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_C_COMPILER=$ClangExe",
    "-DCMAKE_CXX_COMPILER=$ClangxxExe",
    "-DCMAKE_RC_COMPILER=$WindresExe"
) + $ArchFlags
cmake @ConfigureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }

Write-Host "Building..."
cmake --build $BuildDir --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

Write-Host ""
Write-Host "Success! Output: $BuildDir\matrix_player.exe"
