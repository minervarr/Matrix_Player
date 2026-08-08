# Desktop Windows build -> <repo>\build\windows (Release) or \build\windows_debug
# (Debug). Thin launcher: locates MSYS2's UCRT64 Clang toolchain, then
# cmake+ninja does everything - no source lives here. Matches this repo's
# scripts/<platform>/ convention, and nests under build\ the same way
# scripts/linux/build.sh's build/linux and build/linux_debug do (one shared
# top-level build\ ignore entry covers every platform's output).
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
# Usage: scripts\windows\build.ps1 [-Debug|-Release] [-Clean]
#                                   [-Native|-V3|-V4|-Custom <march>]
#                                   [-Msys2Root <path>]
#
# Passing an explicit mode flag (-Debug/-Release/-Native/-V3/-V4/-Custom;
# scripts, CI) always skips straight to the build. Run with no mode flag on
# an interactive console and two prompts run in sequence - microarchitecture
# target, then build type - mirroring scripts/linux/build.sh's Scene
# 1/Scene 2 menu (Native+Debug is a legitimate combination, not just
# Universal+Release, so the two questions are asked independently):
#   Scene 1 - microarch target:
#     1) Universal (default) -- portable generic x86-64 baseline
#     2) Native               -- tuned to this exact CPU (-march=native)
#     3) Custom                -- enter any -march value (v2/v3/v4/znver4/...)
#   Scene 2 - build type:
#     1) Release (default)
#     2) Debug
# (Linux's menu also offers "All"/"Packages" - building every psABI variant
# at once, or Arch packages - neither of which this script does: there's no
# multi-variant Windows build or Windows-equivalent packaging step here.
# scripts\windows\package.ps1 is the separate, always-Release installer
# driver, and always passes -Release so it never hits this prompt.)
#
# Deliberately no [CmdletBinding()]: it auto-adds a reserved -Debug common
# parameter, which collides with this script's own [switch]$Debug and made
# every invocation with -Debug fail outright ("parameter... defined multiple
# times") - a latent bug in the original script that nothing had ever
# actually run before this port.
param(
    [switch]$Debug,
    [switch]$Release,
    [switch]$Clean,
    [switch]$Native,
    [switch]$V3,   # x86-64-v3 equivalent (AVX2, FMA3, BMI2)
    [switch]$V4,   # x86-64-v4 equivalent (AVX-512)
    [string]$Custom = "",   # any -march value CMake's check_cxx_compiler_flag accepts
    [string]$Msys2Root = "C:\msys64"
)

$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..\..")

$ModeSet = $Debug -or $Release -or $Native -or $V3 -or $V4 -or ($Custom -ne "")
$IsDebug = $Debug.IsPresent
$ArchLevel = $null

# No mode flag given: ask, if there's actually someone at the console to
# answer - a non-interactive caller (CI, package.ps1) falls through to the
# Release/Universal default below instead of hanging on Read-Host.
# package.ps1 always passes -Release explicitly for exactly this reason:
# it's invoked in-process, so IsInputRedirected alone can't tell "called
# from an unattended script" apart from "the outer console is interactive".
if (-not $ModeSet -and -not [Console]::IsInputRedirected) {
    Write-Host "Select microarchitecture target:"
    Write-Host "  1) Universal (default) -- portable generic x86-64 baseline"
    Write-Host "  2) Native -- tuned to this exact CPU (-march=native)"
    Write-Host "  3) Custom -- enter a specific -march value (v2/v3/v4/znver4/...)"
    $archChoice = Read-Host "Enter choice [1-3, default 1]"
    switch ($archChoice) {
        { $_ -in "", "1" } { }
        "2" { $ArchLevel = "native" }
        "3" {
            $customLevel = Read-Host "Enter -march value (e.g. v3, v4, znver4)"
            if ([string]::IsNullOrWhiteSpace($customLevel)) {
                Write-Error "error: no value entered"
                exit 2
            }
            $ArchLevel = $customLevel
        }
        default { Write-Error "error: invalid choice '$archChoice'"; exit 2 }
    }

    Write-Host "Select build type:"
    Write-Host "  1) Release (default)"
    Write-Host "  2) Debug"
    $typeChoice = Read-Host "Enter choice [1-2, default 1]"
    switch ($typeChoice) {
        { $_ -in "", "1" } { $IsDebug = $false }
        "2" { $IsDebug = $true }
        default { Write-Error "error: invalid choice '$typeChoice'"; exit 2 }
    }
} else {
    if ($Native) { $ArchLevel = "native" }
    elseif ($V4) { $ArchLevel = "v4" }
    elseif ($V3) { $ArchLevel = "v3" }
    elseif ($Custom -ne "") { $ArchLevel = $Custom }
}

$BuildType = if ($IsDebug) { "Debug" } else { "Release" }
# Ninja is a single-config generator: flipping CMAKE_BUILD_TYPE in the same
# directory forces a near-total recompile (Debug/Release use incompatible
# runtime libraries). Separate directories keep switching back and forth fast.
$BuildDir = if ($IsDebug) { "build\windows_debug" } else { "build\windows" }

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning $BuildDir..."
    Remove-Item -Recurse -Force $BuildDir
}

# Routes through MATRIX_ARCH_LEVEL (root CMakeLists.txt), not a raw
# -march=/arch: flag - that mechanism already validates the compiler actually
# supports the requested -march (check_cxx_compiler_flag) and fails loudly
# instead of silently ignoring an unsupported request.
$ArchFlags = @()
if ($ArchLevel) { $ArchFlags = @("-DMATRIX_ARCH_LEVEL=$ArchLevel") }

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
Write-Host "Success! Output: $BuildDir\gui\matrix_player.exe"
