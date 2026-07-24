#!/usr/bin/env bash
# Desktop Linux build -> <repo>/build/linux (Release) or build/linux_debug (Debug)
# Prereqs: cmake >= 3.22, ninja, a C++17 compiler, ALSA headers, jack2 headers
# (NOT pipewire-jack), wayland-client/wayland-cursor/xkbcommon dev headers,
# a Vulkan loader + headers, and the Slang shader compiler (slangc).
#
# Usage: scripts/linux/build.sh [--debug|--release] [--clean] [cmake args...]
# Default is --release. Matches scripts/windows/build.ps1's -Debug/-Release
# split: Ninja is a single-config generator, so flipping CMAKE_BUILD_TYPE in
# the same directory forces a near-total recompile — separate directories
# keep switching back and forth fast.
#
# Release: only matrix_player — audio_engine's smoke-test tool executables
#   (list_usb_devices, capture_alsa_to_wav, etc.) and the Windows-only
#   matrix_ab_test are switched off, so a Release build produces exactly the
#   one binary that ships.
# Debug: everything — smoke-test tools included, LTO off, debug info on, no
#   optimization — for actually stepping through any of it.
set -euo pipefail
cd "$(dirname "$0")/../.."

BUILD_TYPE=Release
BUILD_DIR=build/linux
CLEAN=0
CMAKE_ARGS=()

for arg in "$@"; do
    case "$arg" in
        --debug)   BUILD_TYPE=Debug; BUILD_DIR=build/linux_debug ;;
        --release) BUILD_TYPE=Release; BUILD_DIR=build/linux ;;
        --clean)   CLEAN=1 ;;
        *)         CMAKE_ARGS+=("$arg") ;;
    esac
done

if [[ "$CLEAN" -eq 1 && -d "$BUILD_DIR" ]]; then
    echo "Cleaning $BUILD_DIR..."
    rm -rf "$BUILD_DIR"
fi

if [[ "$BUILD_TYPE" == "Debug" ]]; then
    TOOLS_ARG=ON
    AB_TEST_ARG=ON
else
    TOOLS_ARG=OFF
    AB_TEST_ARG=OFF
fi

# vk_canvas resolves slangc from $VULKAN_SDK/bin/slangc, falling back to a
# hardcoded Windows path if VULKAN_SDK is unset. Point at it explicitly
# unless the caller already passed -DVCE_SLANGC or has VULKAN_SDK set.
SLANGC_ARG=()
if [[ "${CMAKE_ARGS[*]:-}" != *"VCE_SLANGC"* && -z "${VULKAN_SDK:-}" ]]; then
    if command -v slangc >/dev/null 2>&1; then
        SLANGC_ARG=(-DVCE_SLANGC="$(command -v slangc)")
    elif [[ -x /opt/shader-slang-bin/bin/slangc ]]; then
        SLANGC_ARG=(-DVCE_SLANGC=/opt/shader-slang-bin/bin/slangc)
    fi
fi

echo "Configuring CMake (Ninja, $BUILD_TYPE) -> $BUILD_DIR..."
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DAUDIO_ENGINE_BUILD_TOOLS="$TOOLS_ARG" \
    -DMATRIX_BUILD_AB_TEST="$AB_TEST_ARG" \
    "${SLANGC_ARG[@]}" "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR"
echo
echo "Binaries in $BUILD_DIR/:"
echo "  gui/matrix_player -- the full GUI, real Wayland window"
echo "  matrix_core, vk_canvas_core, vk_canvas_wayland -- static libs it links"
if [[ "$BUILD_TYPE" == "Debug" ]]; then
    echo "  audio_engine smoke-test tools (list_usb_devices, capture_alsa_to_wav, ...) -- Debug only"
fi
