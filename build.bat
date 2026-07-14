@echo off
setlocal enabledelayedexpansion

:: --- Configuration ---
set "TARGET_EXE=matrix_player_windows.exe"
set "ARCH_FLAG="
set "BUILD_TYPE="
set "CLEAN_REQUESTED="

:: --- Argument Parsing ---
:ParseArgs
if "%~1"=="" goto :DoneParsing
if "%~1"=="-h" goto :ShowHelp
if "%~1"=="--help" goto :ShowHelp

if "%~1"=="clean" (
    set "CLEAN_REQUESTED=1"
    shift
    goto :ParseArgs
)

if "%~1"=="debug" (
    set "BUILD_TYPE=Debug"
    shift
    goto :ParseArgs
)

if "%~1"=="release" (
    set "BUILD_TYPE=Release"
    shift
    goto :ParseArgs
)

if "%~1"=="v3" (
    :: x86-64-v3 equivalent for MSVC (Enables AVX2, FMA3, BMI2)
    set "ARCH_FLAG=-DCMAKE_CXX_FLAGS=/arch:AVX2 -DCMAKE_C_FLAGS=/arch:AVX2"
    echo [Config] Target Microarchitecture: x86-64-v3 (AVX2)
    shift
    goto :ParseArgs
)

if "%~1"=="v4" (
    :: x86-64-v4 equivalent for MSVC (Enables AVX-512)
    set "ARCH_FLAG=-DCMAKE_CXX_FLAGS=/arch:AVX512 -DCMAKE_C_FLAGS=/arch:AVX512"
    echo [Config] Target Microarchitecture: x86-64-v4 (AVX-512)
    shift
    goto :ParseArgs
)

:: Catch unknown arguments
echo Unknown argument: %1
goto :ShowHelp

:DoneParsing

:: If debug/release wasn't given on the command line, ask interactively —
:: a plain double-click or bare `build.bat` should still let you choose.
:: Pass `debug`/`release` explicitly to skip this prompt (scripts/CI).
if not defined BUILD_TYPE (
    echo ==========================================
    echo Choose build configuration
    echo ==========================================
    echo   [1] Release  - optimized, no debug symbols ^(default^)
    echo   [2] Debug    - unoptimized, full debug symbols
    set "BUILD_CHOICE="
    set /p "BUILD_CHOICE=Enter 1 or 2 (Enter = Release): "
    if "!BUILD_CHOICE!"=="2" (
        set "BUILD_TYPE=Debug"
    ) else (
        set "BUILD_TYPE=Release"
    )
)
echo [Config] Build type: %BUILD_TYPE%

:: Separate build directories per config: Ninja is a single-config generator,
:: so flipping CMAKE_BUILD_TYPE in the SAME directory forces it to recompile
:: nearly every object next time (Debug/Release use incompatible runtime
:: libraries, /MDd vs /MD, among other flag differences). Two directories
:: mean switching back and forth is fast after each has been built once.
if /i "%BUILD_TYPE%"=="Debug" (
    set "BUILD_DIR=build_debug"
) else (
    set "BUILD_DIR=build"
)

if defined CLEAN_REQUESTED (
    echo ==========================================
    echo Cleaning previous build ^(%BUILD_DIR%^)...
    echo ==========================================
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    echo Clean finished.
)

echo ==========================================
echo Locating MSVC Environment (vswhere)
echo ==========================================
set "VS_PATH="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_PATH=%%i"
)

if not defined VS_PATH (
    echo ERROR: Visual Studio or C++ Build Tools not found via vswhere.
    exit /b 1
)

set "VCVARS_BAT=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS_BAT%" (
    echo ERROR: Found VS at "%VS_PATH%", but vcvars64.bat is missing.
    exit /b 1
)

call "%VCVARS_BAT%" >nul
if errorlevel 1 (
    echo ERROR: Failed to initialize MSVC environment.
    exit /b 1
)

echo.
echo ==========================================
echo Updating Submodules
echo ==========================================
where git >nul 2>&1
if errorlevel 1 (
    echo WARNING: Git not found in PATH. Skipping submodule update.
) else (
    git submodule update --init --recursive
    if errorlevel 1 (
        echo ERROR: Git submodule update failed.
        exit /b 1
    )
)

echo.
echo ==========================================
echo Configuring CMake (Ninja, %BUILD_TYPE%)
echo ==========================================
:: Injected %ARCH_FLAG% to pass optimization parameters to CMake
cmake -G Ninja -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% %ARCH_FLAG%
if errorlevel 1 (
    echo ERROR: CMake configuration failed.
    exit /b 1
)

echo.
echo ==========================================
echo Building Project
echo ==========================================
cmake --build "%BUILD_DIR%" --parallel
if errorlevel 1 (
    echo ERROR: Build failed.
    exit /b 1
)

echo.
echo ==========================================
echo Success!
echo Output: %BUILD_DIR%\%TARGET_EXE%
echo ==========================================
endlocal
exit /b 0

:ShowHelp
echo Usage: build.bat [clean] [debug ^| release] [v3 ^| v4] [--help]
echo.
echo Options:
echo   clean      Deletes the build directory for the chosen config before configuring.
echo   debug      Debug build (unoptimized, full symbols) -- skips the interactive prompt.
echo   release    Release build (optimized) -- skips the interactive prompt.
echo   v3         Optimizes for x86-64-v3 processors (Enables AVX2 instruction sets).
echo   v4         Optimizes for x86-64-v4 processors (Enables AVX-512 instruction sets).
echo   --help     Shows this help message.
echo.
echo With no debug/release argument, you'll be asked interactively which to build.
echo Debug and Release live in separate directories (build_debug\ / build\), so
echo switching between them doesn't force a full rebuild every time.
endlocal
exit /b 0
