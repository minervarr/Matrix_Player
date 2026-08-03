#!/usr/bin/env bash
# Desktop Linux build -> <repo>/build/linux (Release) or build/linux_debug (Debug)
# Prereqs: cmake >= 3.22, ninja, a C++17 compiler, ALSA headers, jack2 headers
# (NOT pipewire-jack), wayland-client/wayland-cursor/xkbcommon dev headers,
# a Vulkan loader + headers, and the Slang shader compiler (slangc).
#
# Usage: scripts/linux/build.sh [--debug|--release|--share|--packages] [--clean] [cmake args...]
# Passing a mode flag explicitly (scripts, CI) always skips straight to the
# build — same for non-interactive stdin (defaults to Release, Universal).
#
# Run with no mode flag on an interactive terminal and two prompts run in
# sequence — microarchitecture target, then build type — since they're
# orthogonal (e.g. Native+Debug is a legitimate combination, not just
# Universal+Release):
#   Scene 1 — microarch target:
#     1) Universal (default) -- portable generic x86-64 baseline
#     2) Native               -- tuned to this exact CPU (-march=native)
#     3) Custom                -- enter any -march value (v2/v3/v4/znver4/...)
#     4) All                   -- build universal/v3/v4/zen4 in one pass
#     5) Packages              -- the same four as Arch packages, for upload
#   Scene 2 — build type (skipped for Packages, which is Release by definition):
#     1) Release (default)
#     2) Debug
#
# Release: only matrix_player — audio_engine's smoke-test tool executables
#   (list_usb_devices, capture_alsa_to_wav, etc.) and the Windows-only
#   matrix_ab_test are switched off, so a Release build produces exactly the
#   one binary that ships.
# Debug: everything — smoke-test tools included, LTO off, debug info on, no
#   optimization — for actually stepping through any of it.
# All (formerly --share): builds four binaries — Universal plus the v3/v4
#   x86-64 microarch levels plus a Zen4-tuned build (see MATRIX_ARCH_LEVEL in
#   the root CMakeLists.txt) — so you can hand out whichever one matches the
#   recipient's CPU instead of one lowest-common-denominator binary. Each
#   variant gets its own build dir under build/linux_share/ (same
#   one-config-per-directory reasoning as Debug/Release above). Release
#   variants are packaged as tarballs under dist/linux/; Debug variants
#   (an "All" + Debug combo picked interactively) are left unpackaged in
#   build/linux_share_debug/ — Debug output, with symbols and smoke-test
#   tools baked in, isn't the kind of thing you hand someone.
set -euo pipefail
cd "$(dirname "$0")/../.."

BUILD_TYPE=Release
SHARE=0
PACKAGES=0
CLEAN=0
MODE_SET=0
ARCH_LEVEL=""
ARCH_SUFFIX=""
CMAKE_ARGS=()

for arg in "$@"; do
    case "$arg" in
        --debug)    BUILD_TYPE=Debug; MODE_SET=1 ;;
        --release)  BUILD_TYPE=Release; MODE_SET=1 ;;
        --share)    SHARE=1; MODE_SET=1 ;;
        --packages) PACKAGES=1; MODE_SET=1 ;;
        --clean)    CLEAN=1 ;;
        *)          CMAKE_ARGS+=("$arg") ;;
    esac
done

# No mode flag given: ask, if there's actually someone at the keyboard to
# answer (stdin a tty) — a non-interactive caller (CI, a pipe) falls through
# to the Release/Universal default above instead of hanging on `read`.
if [[ "$MODE_SET" -eq 0 && -t 0 ]]; then
    echo "Select microarchitecture target:"
    echo "  1) Universal (default) -- portable generic x86-64 baseline"
    echo "  2) Native -- tuned to this exact CPU (-march=native)"
    echo "  3) Custom -- enter a specific -march value (v2/v3/v4/znver4/...)"
    echo "  4) All -- build universal/v3/v4/zen4 in one pass"
    echo "  5) Packages -- the same four as Arch packages, for upload"
    read -r -p "Enter choice [1-5, default 1]: " arch_choice
    case "$arch_choice" in
        ""|1) ;;
        2) ARCH_LEVEL="native"; ARCH_SUFFIX="_native" ;;
        3)
            read -r -p "Enter -march value (e.g. v3, v4, znver4): " custom_level
            if [[ -z "$custom_level" ]]; then
                echo "error: no value entered" >&2
                exit 2
            fi
            ARCH_LEVEL="$custom_level"
            ARCH_SUFFIX="_custom-${custom_level}"
            ;;
        4) SHARE=1 ;;
        5) PACKAGES=1 ;;
        *) echo "error: invalid choice '$arch_choice'" >&2; exit 2 ;;
    esac

    # Packages are Release by definition — the PKGBUILD hardcodes it, and a
    # Debug package (symbols, no LTO, smoke-test tools) is not something you
    # hand anyone. Skip the question rather than ask one whose answer is
    # ignored.
    if [[ "$PACKAGES" -eq 0 ]]; then
        echo "Select build type:"
        echo "  1) Release (default)"
        echo "  2) Debug"
        read -r -p "Enter choice [1-2, default 1]: " type_choice
        case "$type_choice" in
            ""|1) BUILD_TYPE=Release ;;
            2)     BUILD_TYPE=Debug ;;
            *)     echo "error: invalid choice '$type_choice'" >&2; exit 2 ;;
        esac
    fi
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

if [[ "$PACKAGES" -eq 1 ]]; then
    # The Arch-package sibling of --share: same four microarch variants, but
    # built by makepkg into installable .pkg.tar.zst files instead of tarballs.
    #
    # This script does NOT rebuild anything itself here. Everything — the four
    # configures, the dsp_null_test gate per variant, and the four self-
    # contained packages — lives in packaging/arch/PKGBUILD, because that is
    # where a person reading the package expects to find it, and because
    # makepkg has to own $srcdir for its checksums to mean anything.
    #
    # The PKGBUILD builds the last PUSHED commit, not this working tree.
    PKG_DIR=packaging/arch
    DIST_DIR=dist/linux

    # WHERE MAKEPKG PUTS ITS WORKING FILES — this is load-bearing, not tidiness.
    #
    # Left alone, all of these default to $startdir, i.e. packaging/arch/ itself
    # (makepkg: `${!var:-$startdir}`). For a git source that means makepkg drops
    # a BARE CLONE OF THIS ENTIRE REPOSITORY at packaging/arch/matrix_player/ —
    # a ~97 MB packfile sitting in the working tree, which is exactly how one
    # got committed and pushed. A .gitignore entry is a second line of defence;
    # this is the first, because build/ and dist/ are ignored wholesale at the
    # repo root rather than by a nested pattern that can be missed.
    #
    # Absolute paths: makepkg runs with its own $startdir, so relative ones
    # would resolve against packaging/arch/.
    export SRCDEST="$PWD/build/packaging/src"      # VCS clones + source tarballs
    export BUILDDIR="$PWD/build/packaging/build"   # src/ and pkg/ extraction
    export PKGDEST="$PWD/$DIST_DIR"                # finished packages, straight to dist/
    mkdir -p "$SRCDEST" "$BUILDDIR" "$PKGDEST"

    if ! command -v makepkg >/dev/null 2>&1; then
        echo "error: makepkg not found — this mode needs base-devel" >&2
        exit 2
    fi
    if [[ ! -f "$PKG_DIR/PKGBUILD" ]]; then
        echo "error: $PKG_DIR/PKGBUILD not found" >&2
        exit 2
    fi
    if [[ "$BUILD_TYPE" == "Debug" ]]; then
        echo "note: --packages is always Release; ignoring --debug." >&2
    fi
    if [[ ${#CMAKE_ARGS[@]} -gt 0 ]]; then
        echo "note: extra cmake args are not forwarded to makepkg;" >&2
        echo "      edit $PKG_DIR/PKGBUILD's build() instead: ${CMAKE_ARGS[*]}" >&2
    fi

    if [[ "$CLEAN" -eq 1 ]]; then
        echo "Cleaning build/packaging and previous packages..."
        rm -rf build/packaging
        rm -f "$DIST_DIR"/*.pkg.tar.zst
        mkdir -p "$SRCDEST" "$BUILDDIR"
    fi

    echo
    echo "==> makepkg: four variants, each a full build (this takes a while)"
    # -f so a re-run overwrites; no -i, since installing one of four on the
    # build machine is a separate decision from producing them.
    ( cd "$PKG_DIR" && makepkg -f )

    # PKGDEST already put them in $DIST_DIR, next to the --share tarballs, so
    # there is one place to upload from and nothing to move.
    shopt -s nullglob
    built=("$DIST_DIR"/*.pkg.tar.zst)
    shopt -u nullglob
    if [[ ${#built[@]} -eq 0 ]]; then
        echo "error: makepkg reported success but produced no packages" >&2
        exit 1
    fi

    echo
    echo "Packages in $DIST_DIR/:"
    for p in "${built[@]}"; do
        printf '  %6s  %s\n' "$(du -h "$p" | cut -f1)" "$p"
    done
    echo
    echo "Each is self-contained — a recipient downloads ONE and runs:"
    echo "  sudo pacman -U <file>"
    echo "They can check which variant their CPU supports with:"
    echo "  /lib/ld-linux-x86-64.so.2 --help | grep -A4 'Subdirectories of glibc-hwcaps'"
    echo "universal works everywhere; v3/v4/zen4 only where that line says 'supported'."
    exit 0
fi

if [[ "$SHARE" -eq 1 ]]; then
    # variant name -> MATRIX_ARCH_LEVEL value ("" = compiler default baseline)
    declare -A SHARE_VARIANTS=(
        [universal]=""
        [v3]="v3"
        [v4]="v4"
        [zen4]="znver4"
    )

    if [[ "$BUILD_TYPE" == "Debug" ]]; then
        SHARE_ROOT=build/linux_share_debug
        TOOLS_ARG=ON
        AB_TEST_ARG=ON
    else
        SHARE_ROOT=build/linux_share
        TOOLS_ARG=OFF
        AB_TEST_ARG=OFF
    fi
    DIST_DIR=dist/linux

    if [[ "$CLEAN" -eq 1 ]]; then
        echo "Cleaning $SHARE_ROOT and $DIST_DIR..."
        rm -rf "$SHARE_ROOT" "$DIST_DIR"
    fi
    [[ "$BUILD_TYPE" == "Release" ]] && mkdir -p "$DIST_DIR"

    for variant in "${!SHARE_VARIANTS[@]}"; do
        level="${SHARE_VARIANTS[$variant]}"
        variant_dir="$SHARE_ROOT/$variant"
        arch_arg=()
        [[ -n "$level" ]] && arch_arg=(-DMATRIX_ARCH_LEVEL="$level")

        echo
        echo "==> Configuring '$variant' ($BUILD_TYPE) -> $variant_dir..."
        cmake -S . -B "$variant_dir" -G Ninja \
            -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
            -DAUDIO_ENGINE_BUILD_TOOLS="$TOOLS_ARG" \
            -DMATRIX_BUILD_AB_TEST="$AB_TEST_ARG" \
            "${arch_arg[@]}" "${SLANGC_ARG[@]}" "${CMAKE_ARGS[@]}"
        cmake --build "$variant_dir"

        if [[ "$BUILD_TYPE" == "Release" ]]; then
            # Only the runtime files ship — $variant_dir/gui also holds
            # CMakeFiles/ and other build-tree clutter that cp -r'ing the
            # whole directory would otherwise drag into the tarball.
            pkg_name="matrix_player-linux-$variant"
            pkg_dir="$DIST_DIR/$pkg_name"
            rm -rf "$pkg_dir"
            mkdir -p "$pkg_dir"
            cp "$variant_dir/gui/matrix_player" "$pkg_dir/"
            cp "$variant_dir/gui/eq_profiles.json" "$pkg_dir/"
            cp -r "$variant_dir/gui/assets" "$pkg_dir/assets"
            cp -r "$variant_dir/gui/fonts" "$pkg_dir/fonts"
            tar -C "$DIST_DIR" -czf "$DIST_DIR/$pkg_name.tar.gz" "$pkg_name"
            rm -rf "$pkg_dir"
            echo "==> Packaged $DIST_DIR/$pkg_name.tar.gz"
        else
            echo "==> Built $variant_dir/gui/matrix_player (Debug, not packaged)"
        fi
    done

    echo
    if [[ "$BUILD_TYPE" == "Release" ]]; then
        echo "All-variant build done. Tarballs in $DIST_DIR/:"
        for variant in "${!SHARE_VARIANTS[@]}"; do
            echo "  $DIST_DIR/matrix_player-linux-$variant.tar.gz"
        done
    else
        echo "All-variant build done (Debug, unpackaged). Binaries:"
        for variant in "${!SHARE_VARIANTS[@]}"; do
            echo "  $SHARE_ROOT/$variant/gui/matrix_player"
        done
    fi
    exit 0
fi

if [[ "$BUILD_TYPE" == "Debug" ]]; then
    BUILD_DIR="build/linux${ARCH_SUFFIX}_debug"
    TOOLS_ARG=ON
    AB_TEST_ARG=ON
else
    BUILD_DIR="build/linux${ARCH_SUFFIX}"
    TOOLS_ARG=OFF
    AB_TEST_ARG=OFF
fi

if [[ "$CLEAN" -eq 1 && -d "$BUILD_DIR" ]]; then
    echo "Cleaning $BUILD_DIR..."
    rm -rf "$BUILD_DIR"
fi

ARCH_ARG=()
[[ -n "$ARCH_LEVEL" ]] && ARCH_ARG=(-DMATRIX_ARCH_LEVEL="$ARCH_LEVEL")

echo "Configuring CMake (Ninja, $BUILD_TYPE${ARCH_LEVEL:+, arch=$ARCH_LEVEL}) -> $BUILD_DIR..."
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DAUDIO_ENGINE_BUILD_TOOLS="$TOOLS_ARG" \
    -DMATRIX_BUILD_AB_TEST="$AB_TEST_ARG" \
    "${ARCH_ARG[@]}" "${SLANGC_ARG[@]}" "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR"
echo
echo "Binaries in $BUILD_DIR/:"
echo "  gui/matrix_player -- the full GUI, real Wayland window"
echo "  matrix_core, vk_canvas_core, vk_canvas_wayland -- static libs it links"
if [[ "$BUILD_TYPE" == "Debug" ]]; then
    echo "  audio_engine smoke-test tools (list_usb_devices, capture_alsa_to_wav, ...) -- Debug only"
fi
