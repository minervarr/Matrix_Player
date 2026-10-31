# Resolve Slang's slangc for any build that compiles .slang shaders.
#
# Included by BOTH top-level projects — the desktop root CMakeLists.txt and
# android/CMakeLists.txt, which does not go through it. Call
# matrix_resolve_slangc() BEFORE include(VceShaders.cmake):
#
#     include(${REPO_ROOT}/cmake/MatrixSlangc.cmake)
#     matrix_resolve_slangc()
#
# Why this file exists: vk_canvas's VceShaders.cmake (a submodule, never patched
# in place) resolves slangc from $VULKAN_SDK/bin/slangc and, when VULKAN_SDK is
# unset, falls back to a HARDCODED Windows path:
#
#     C:/VulkanSDK/1.4.341.1/Bin/slangc.exe
#
# On Linux with the distro's vulkan-devel that fallback is simply wrong (there
# is no Vulkan SDK hierarchy at all), and on Windows it pins one SDK version.
# Arch has two different Slang packages — shader-slang-bin installs to
# /opt/shader-slang-bin, while shader-slang / shader-slang-git (built from
# source) installs to /opt/shader-slang — and a self-compiled tree can live
# anywhere. So guessing one fixed path is not a resolution strategy.
#
# VCE_SLANGC stays the single override, because that is the variable
# VceShaders.cmake reads. Precedence:
#
#   1. an explicit -DVCE_SLANGC=/path/to/slangc (or one already in the cache)
#   2. $SLANGC, for a shell-local choice without touching the cache
#   3. slangc on PATH
#   4. $VULKAN_SDK/bin|Bin, the layout the LunarG SDK installs
#   5. the known Arch prefixes, then any C:/VulkanSDK/<version> on Windows
#
# A VCE_SLANGC cache entry that no longer exists (typically a Windows path
# left behind in a reused build tree, or an uninstalled AUR package) is NOT
# treated as an override: it is ignored with a status line and the search runs.
# Otherwise a stale cache would defeat this entire file — which is exactly how
# the hardcoded path used to fail an otherwise healthy Linux Android build.

function(matrix_resolve_slangc)
    set(_matrix_slangc "")

    # (1) Explicit/cached value, but only if it is a real file. A cache entry
    # can arrive from a previous OS or a removed package.
    if(VCE_SLANGC)
        if(EXISTS "${VCE_SLANGC}")
            set(_matrix_slangc "${VCE_SLANGC}")
        else()
            message(STATUS
                "Ignoring VCE_SLANGC='${VCE_SLANGC}' (no such file); "
                "searching for slangc")
        endif()
    endif()

    # (2) $SLANGC as a full path. $VCE_SLANGC is accepted too: it is the same
    # knob, and someone who exports it should not need -D as well.
    if(NOT _matrix_slangc)
        foreach(_matrix_slangc_env SLANGC VCE_SLANGC)
            if(DEFINED ENV{${_matrix_slangc_env}}
               AND EXISTS "$ENV{${_matrix_slangc_env}}")
                set(_matrix_slangc "$ENV{${_matrix_slangc_env}}")
                break()
            endif()
        endforeach()
    endif()

    # (3)-(5) find_program: PATH first, then the Vulkan SDK layout, then the
    # distro/AUR prefixes. NO_CACHE keeps the probe out of CMakeCache — the
    # resolved result is stored in VCE_SLANGC below, which is the one value the
    # user is meant to see and override. (NO_CACHE needs CMake 3.21; the
    # project floor is 3.22.)
    if(NOT _matrix_slangc)
        set(_matrix_slangc_hints "")
        if(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
            list(APPEND _matrix_slangc_hints
                "$ENV{VULKAN_SDK}/bin"
                "$ENV{VULKAN_SDK}/Bin")
        endif()
        find_program(_matrix_slangc_found
            NAMES slangc slangc.exe
            HINTS ${_matrix_slangc_hints}
            PATHS
                /opt/shader-slang/bin
                /opt/shader-slang-bin/bin
                /opt/slang/bin
                /usr/local/bin
            NO_CACHE)
        set(_matrix_slangc "${_matrix_slangc_found}")
    endif()

    # Windows ignores VULKAN_SDK surprisingly often (the installer sets it for
    # new shells only), so enumerate installed SDKs instead of naming a version.
    # Natural sort puts 1.4.341.1 above 1.4.99.1 and picks the newest.
    if(NOT _matrix_slangc AND WIN32)
        set(_matrix_vulkan_sdk_roots "C:/VulkanSDK")
        if(DEFINED ENV{PROGRAMFILES})
            list(APPEND _matrix_vulkan_sdk_roots
                "$ENV{PROGRAMFILES}/VulkanSDK")
        endif()
        set(_matrix_vulkan_sdk_slangc "")
        foreach(_matrix_vulkan_sdk_root ${_matrix_vulkan_sdk_roots})
            file(GLOB _matrix_vulkan_sdk_matches
                "${_matrix_vulkan_sdk_root}/*/Bin/slangc.exe")
            list(APPEND _matrix_vulkan_sdk_slangc ${_matrix_vulkan_sdk_matches})
        endforeach()
        if(_matrix_vulkan_sdk_slangc)
            list(SORT _matrix_vulkan_sdk_slangc COMPARE NATURAL ORDER DESCENDING)
            list(GET _matrix_vulkan_sdk_slangc 0 _matrix_slangc)
        endif()
    endif()

    if(NOT _matrix_slangc)
        message(FATAL_ERROR
            "slangc not found — it compiles every .slang shader, and there is "
            "no committed .spv to fall back on.\n"
            "Install it (Arch: vulkan-devel for the toolchain, plus "
            "shader-slang-bin or shader-slang for slangc), put it on PATH, or "
            "point at it explicitly:\n"
            "    -DVCE_SLANGC=/path/to/slangc\n"
            "Searched PATH, $VULKAN_SDK/{bin,Bin}, /opt/shader-slang/bin, "
            "/opt/shader-slang-bin/bin, /opt/slang/bin and /usr/local/bin.")
    endif()

    # CACHE ... FORCE, not a normal variable: VceShaders.cmake declares the
    # same cache entry, and it must see ours rather than re-derive its
    # hardcoded default. FORCE also heals the stale-cache case above.
    set(VCE_SLANGC "${_matrix_slangc}" CACHE FILEPATH
        "Path to the Slang compiler (slangc) from the Vulkan SDK" FORCE)
    message(STATUS "slangc: ${VCE_SLANGC}")
endfunction()
