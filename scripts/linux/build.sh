#!/usr/bin/env bash
# Desktop Linux build -> <repo>/build/linux
# Prereqs: cmake >= 3.22, ninja, a C++17 compiler, ALSA headers, jack2 headers
# (NOT pipewire-jack), wayland-client/wayland-cursor/xkbcommon dev headers,
# a Vulkan loader + headers, and the Slang shader compiler (slangc).
set -euo pipefail
cd "$(dirname "$0")/../.."

# vk_canvas resolves slangc from $VULKAN_SDK/bin/slangc, falling back to a
# hardcoded Windows path if VULKAN_SDK is unset. Point at it explicitly
# unless the caller already passed -DVCE_SLANGC or has VULKAN_SDK set.
SLANGC_ARG=()
if [[ "$*" != *"VCE_SLANGC"* && -z "${VULKAN_SDK:-}" ]]; then
    if command -v slangc >/dev/null 2>&1; then
        SLANGC_ARG=(-DVCE_SLANGC="$(command -v slangc)")
    elif [[ -x /opt/shader-slang-bin/bin/slangc ]]; then
        SLANGC_ARG=(-DVCE_SLANGC=/opt/shader-slang-bin/bin/slangc)
    fi
fi

cmake -S . -B build/linux -G Ninja "${SLANGC_ARG[@]}" "$@"
cmake --build build/linux
echo
echo "Binaries in build/linux/:"
echo "  matrix_core, vk_canvas_core, vk_canvas_wayland (matrix_player GUI"
echo "  executable is Windows-only until the host-abstraction phase lands)"
