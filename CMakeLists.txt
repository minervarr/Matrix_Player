cmake_minimum_required(VERSION 3.22.1)
project("matrix_player")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Whole-program optimization (MSVC /GL+/LTCG, GCC/Clang LTO) for Release
# builds: lets the linker inline/dead-strip across translation units instead
# of stopping at each object's boundary. Must be set before
# add_subdirectory()/add_executable() calls below — CMake reads it at
# target-creation time, not build time. Costs link time (noticeably longer
# than a normal Release link); harmless for Debug (this only fires when
# CMAKE_BUILD_TYPE=Release).
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Optional x86-64 psABI microarchitecture level (v2/v3/v4, or "native").
# Empty (default) leaves the compiler's generic x86-64 baseline untouched —
# a binary compiled for v4 (AVX-512 etc.) SIGILLs on any CPU that lacks it,
# and GCC/Clang only recognize -march=x86-64-v2/v3/v4 from GCC 11 / Clang 12
# onward, so hardcoding this would break compiling *and* running on another
# machine or an older toolchain. Opt in explicitly per build:
#   cmake -B build/linux -DMATRIX_ARCH_LEVEL=v4 ...
# Appended to CMAKE_CXX_FLAGS/CMAKE_C_FLAGS (not add_compile_options) so the
# ExternalProject_Add sub-builds below (libjpeg-turbo, min_text_size), which
# forward those same variables into their own separate `cmake` invocations,
# pick it up too — an add_compile_options() directory property would not
# cross that boundary.
set(MATRIX_ARCH_LEVEL "" CACHE STRING "x86-64 target to compile for: v2/v3/v4 (psABI level), native, or a specific -march name (e.g. znver4). Empty = compiler default (portable).")
if(MATRIX_ARCH_LEVEL AND NOT MSVC)
    if(MATRIX_ARCH_LEVEL MATCHES "^v[234]$" OR MATRIX_ARCH_LEVEL STREQUAL "v1")
        set(MATRIX_ARCH_FLAG "-march=x86-64-${MATRIX_ARCH_LEVEL}")
    else()
        # "native", or any other -march value GCC/Clang recognizes directly
        # (znver1..znver4 for AMD Zen-tuned builds, etc.)
        set(MATRIX_ARCH_FLAG "-march=${MATRIX_ARCH_LEVEL}")
    endif()
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("${MATRIX_ARCH_FLAG}" MATRIX_ARCH_FLAG_SUPPORTED)
    if(NOT MATRIX_ARCH_FLAG_SUPPORTED)
        message(FATAL_ERROR "Compiler does not support ${MATRIX_ARCH_FLAG} "
                             "(x86-64-v2/v3/v4 need GCC 11+ or Clang 12+). "
                             "Leave MATRIX_ARCH_LEVEL unset to build portably.")
    endif()
    string(APPEND CMAKE_CXX_FLAGS " ${MATRIX_ARCH_FLAG}")
    string(APPEND CMAKE_C_FLAGS " ${MATRIX_ARCH_FLAG}")
elseif(MATRIX_ARCH_LEVEL AND MSVC)
    message(WARNING "MATRIX_ARCH_LEVEL is a GCC/Clang psABI flag and has no "
                     "MSVC equivalent for v4 (MSVC tops out at /arch:AVX2) — ignored.")
endif()

# -fno-math-errno: let the compiler emit math instructions instead of libm
# calls. GCC 15 at -O3 compiles `lrint(x)` to `jmp lrint@PLT` purely because
# lrint is allowed to set errno; with this flag it becomes a single cvtsd2siq.
# The audio path calls into that family once per sample, so it is worth having.
#
# This does NOT change any result: errno is the only thing dropped, and nothing
# in this tree reads errno after a math call. It is emphatically NOT
# -ffast-math, which would license reassociation and flush-to-zero and destroy
# the bit-exactness the DSP is built around (see framework/audio_engine/
# tests/dsp_null_test.cpp). Do not "upgrade" it.
#
# Appended to the global flags for the same reason as MATRIX_ARCH_LEVEL above:
# the ExternalProject sub-builds forward these variables.
if(NOT MSVC)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-fno-math-errno" MATRIX_HAVE_NO_MATH_ERRNO)
    if(MATRIX_HAVE_NO_MATH_ERRNO)
        string(APPEND CMAKE_CXX_FLAGS " -fno-math-errno")
        string(APPEND CMAKE_C_FLAGS " -fno-math-errno")
    endif()
endif()

# -ffp-contract=off: forbid the compiler from fusing `a*b + c` into a single
# FMA instruction. This is NOT a micro-optimization knob — it is what keeps
# MATRIX_ARCH_LEVEL from silently changing the audio.
#
# GCC defaults to -ffp-contract=fast, which is licensed to contract even in
# strict ISO mode. At the generic x86-64 baseline that is harmless because
# there is no FMA instruction to contract INTO. The moment MATRIX_ARCH_LEVEL
# reaches v3 (or native/znver4 on any modern part) FMA appears, the EQ's
# `acc = b0*x + b1*x1 + ...` starts fusing, and each biquad keeps a wider
# intermediate than the frozen scalar oracle it is checked against. The result
# is not "wrong" in a numerical sense — it is arguably more accurate — but it
# is DIFFERENT, and this project's whole DSP contract is that speed comes from
# removing overhead and never from touching the signal (rule 9 in
# framework/audio_engine/CLAUDE.md).
#
# Measured, not assumed: with -march=native and contraction left at the
# default, framework/audio_engine/tests/dsp_null_test.cpp fails test [2]
# (EqProcessor vs frozen scalar oracle) at line 481. With this flag it passes
# all 15681922 checks. That means every -DMATRIX_ARCH_LEVEL build — including
# the v3/v4/znver4 tarballs `scripts/linux/build.sh --share` produces — was
# shipping an EQ that did not match the reference.
#
# Global, not per-target, and deliberately so: the DSP primitives are HEADERS
# (core/dsp/eq_processor.h et al), compiled into whichever translation unit
# includes them — gui/src/player_view.cc among them. A flag scoped to the
# ae_* targets would miss exactly the copies that matter.
#
# Appended to the global flags for the same reason as MATRIX_ARCH_LEVEL above:
# the ExternalProject sub-builds forward these variables.
if(NOT MSVC)
    check_cxx_compiler_flag("-ffp-contract=off" MATRIX_HAVE_FP_CONTRACT_OFF)
    if(MATRIX_HAVE_FP_CONTRACT_OFF)
        string(APPEND CMAKE_CXX_FLAGS " -ffp-contract=off")
        string(APPEND CMAKE_C_FLAGS " -ffp-contract=off")
    endif()
endif()

# -stdlib=libc++ on Windows/Clang only: MSYS2 UCRT64's Clang 22 ships against
# GCC 16's libstdc++ headers by default, and a plain std::map<std::string, T>
# ::operator[] (core/src/db.cpp's FileCache cache) fails to compile against
# them — a real incompatibility in libstdc++'s _Rb_tree internals under Clang's
# frontend, not a bug in this project's code (confirmed: the identical
# translation unit compiles clean with -stdlib=libc++, LLVM's own standard
# library, same version as Clang itself via the same MSYS2 packaging). Scoped
# to WIN32 so the working Linux GCC/Clang + system libstdc++ build is
# untouched. Must apply to every C++ TU in this configuration, including the
# ExternalProject sub-builds (hence CMAKE_CXX_FLAGS, not a per-target flag —
# mixing libstdc++ and libc++ objects in one binary is unsafe), and to the
# link step, since the runtime library actually linked has to match too.
if(WIN32 AND NOT MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    string(APPEND CMAKE_CXX_FLAGS " -stdlib=libc++")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -stdlib=libc++")
    # libc++.dll ends up a runtime dependency despite the static-link flags
    # below (see gui/CMakeLists.txt's POST_BUILD copy step for why) — derived
    # from the compiler's own location, not hardcoded, since it's a sibling
    # of clang++.exe in every MSYS2 UCRT64 install regardless of where MSYS2
    # itself is installed.
    get_filename_component(_matrix_clang_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_matrix_libcxx_dll "${_matrix_clang_bin_dir}/libc++.dll")
endif()

# Statically link the MinGW/UCRT runtime (libc++, libgcc, and — confirmed by
# an isolated std::thread/mutex/condition_variable smoke test — winpthread
# too) so every Windows executable this project builds is self-contained: no
# MSYS2 install needed on PATH to run it, on the dev machine or an end
# user's. Only vulkan-1.dll (GPU-driver-provided) and the OS's own
# api-ms-win-crt-*.dll/KERNEL32.dll stay dynamic. Global (CMAKE_EXE_LINKER_
# FLAGS), not per-target: a Debug test binary that only runs with MSYS2's
# ucrt64/bin on PATH defeats the point as much as the shipped player would.
if(WIN32 AND NOT MSVC)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -static -static-libgcc -static-libstdc++")
endif()

# -fuse-ld=lld on Windows/Clang: Release enables CMAKE_INTERPROCEDURAL_
# OPTIMIZATION (line ~15 above), which under Clang means -flto=thin — and
# MSYS2 UCRT64's default linker (GNU ld) needs its LLVMgold.dll plugin to
# understand the resulting LLVM-bitcode objects, which failed to load here
# ("error loading plugin: " — measured, not assumed) even with
# mingw-w64-ucrt-x86_64-llvm installed for llvm-ar/llvm-ranlib (a SEPARATE
# requirement — those two satisfy CMAKE_CXX_COMPILER_AR/RANLIB for archiving
# the static libs, but say nothing about the final link step's plugin).
# lld is LLVM's own linker and reads LLVM bitcode natively, no plugin
# involved — installed via mingw-w64-ucrt-x86_64-lld. Applied globally
# (Debug included) rather than gated on Release-with-LTO specifically: one
# linker for both configs is simpler than two, and lld works identically
# well for Debug's non-LTO link.
if(WIN32 AND NOT MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -fuse-ld=lld")
endif()

# Standalone dev tool, not part of the shipped player — off by default so a
# normal build never compiles or links it. Turn on with
# `cmake -B build -DMATRIX_BUILD_AB_TEST=ON`. Windows-only (uses <windows.h>
# directly) regardless of this flag.
option(MATRIX_BUILD_AB_TEST "Build the standalone audio A/B comparison tool (tools/ab_test.cpp)" OFF)

# ---------- libjpeg-turbo (higher-fidelity JPEG decode for album art) ----------
# libjpeg-turbo's own CMakeLists.txt refuses add_subdirectory() (it FATAL_ERRORs
# unless it's the top-level project) — build it out-of-tree via ExternalProject
# and expose it as an imported target instead.
#
# Defined here, before every add_subdirectory() below, because
# framework/vk_canvas/first_party/img_decode_kit's own CMakeLists.txt gates its
# turbojpeg JPEG path on `if(TARGET turbojpeg-static)` at the moment IT is
# add_subdirectory()'d (transitively, via vk_canvas/core) — CMake evaluates
# that check immediately, not after the whole tree is configured. Defining
# turbojpeg-static any later left the check seeing no such target yet, so
# img_decode_kit silently fell back to its stb_image decode path (which
# downsamples width/height independently to the request box instead of
# preserving the source aspect ratio) even though turbojpeg-static was linked
# into the final binary — every JPEG, including perfectly square cover art,
# came out stretched to whatever non-square box it was decoded for (e.g. the
# fullscreen album art window's monitor resolution).
include(ExternalProject)
set(TURBOJPEG_INSTALL_DIR ${CMAKE_BINARY_DIR}/libjpeg-turbo-install)
# libjpeg-turbo's own CMakeLists.txt renames the turbojpeg-static target's
# output to plain "turbojpeg" (see its CMakeLists.txt) everywhere except MSVC,
# where MSVC-style .lib naming keeps the target name as the file name.
if(MSVC)
    set(TURBOJPEG_BYPRODUCT ${TURBOJPEG_INSTALL_DIR}/lib/turbojpeg-static.lib)
else()
    set(TURBOJPEG_BYPRODUCT ${TURBOJPEG_INSTALL_DIR}/lib/libturbojpeg.a)
endif()
# CMAKE_CXX_FLAGS/CMAKE_C_FLAGS forwarded explicitly: ExternalProject_Add runs
# an entirely separate `cmake configure`, so an arch flag passed to THIS
# configure (e.g. -DCMAKE_CXX_FLAGS=/arch:AVX2) would otherwise never reach
# this nested build — libjpeg-turbo would silently compile at the generic
# baseline even when a wider ISA was requested.
ExternalProject_Add(libjpeg_turbo_ext
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/libjpeg-turbo
    CMAKE_ARGS
        -G${CMAKE_GENERATOR}
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
        -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
        -DCMAKE_INSTALL_LIBDIR=lib
        -DENABLE_SHARED=OFF
        -DENABLE_STATIC=ON
        -DWITH_TURBOJPEG=ON
        -DWITH_TOOLS=OFF
        -DWITH_TESTS=OFF
        -DWITH_JAVA=OFF
        -DCMAKE_INSTALL_PREFIX=${TURBOJPEG_INSTALL_DIR}
    INSTALL_DIR ${TURBOJPEG_INSTALL_DIR}
    BUILD_BYPRODUCTS ${TURBOJPEG_BYPRODUCT})

add_library(turbojpeg-static STATIC IMPORTED GLOBAL)
file(MAKE_DIRECTORY ${TURBOJPEG_INSTALL_DIR}/include)  # must exist at configure time
set_target_properties(turbojpeg-static PROPERTIES
    IMPORTED_LOCATION ${TURBOJPEG_BYPRODUCT}
    INTERFACE_INCLUDE_DIRECTORIES ${TURBOJPEG_INSTALL_DIR}/include)
add_dependencies(turbojpeg-static libjpeg_turbo_ext)

# ---------- audio_engine (framework/, firstparty submodule) ----------
# Defines ae_core (INTERFACE), ae_usb (STATIC, cross-platform), and on Linux
# ae_alsa/ae_jack automatically when ALSA headers / jack2 pkg-config are found.
add_subdirectory(framework/audio_engine)

# ---------- vk_canvas (Vulkan 2D canvas engine, framework/, firstparty submodule) ----------
add_subdirectory(framework/vk_canvas/core vk_canvas_build)


# Linux platform backend: builds vk_canvas_wayland (raw Wayland + xkbcommon).
# The Windows equivalent (win32_platform.cc) has no such subdirectory —
# framework/vk_canvas/platform/windows is vk_canvas's own standalone demo
# project; this app instead compiles win32_platform.cc directly as one of its
# own sources (see gui/CMakeLists.txt), matching that library's own convention
# for consumers.
if(NOT WIN32)
    add_subdirectory(framework/vk_canvas/platform/linux vk_canvas_linux_build)
endif()

# ---------- sqlite3 (vendored amalgamation, third_party/) ----------
add_library(sqlite3 STATIC third_party/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party)
if(MSVC)
    target_compile_options(sqlite3 PRIVATE /W0)
else()
    target_compile_options(sqlite3 PRIVATE -w)
endif()

# ---------- soxr resampler ----------
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/soxr/cmake/Modules")
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WITH_OPENMP OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WITH_LSR_BINDINGS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/soxr)

# ---------- app_shell (the application shell: Host + where files live) ------
# The Host/AppView seam and its three implementations, plus app_paths,
# ui_metrics and ui_orientation. Lifted out of gui/ so a second application can
# LINK it instead of copying it — see framework/app_shell/CLAUDE.md.
#
# Must come before core/ and gui/: both include its headers.
#
# The three names below are what this application tells its shell, and all
# three are chosen to preserve exactly what Matrix Player already had — the
# same matrix_player.log, the same "MatrixPlayerMain" window class, the same
# $HOME/<name>/ split — so lifting the code changed nothing on disk.
# MATRIX_STATE_HOME is declared here rather than in gui/CMakeLists.txt because
# app_paths.cc now belongs to app_shell, which CMake reads first.
set(MATRIX_STATE_HOME "" CACHE STRING
    "Directory name under $HOME for the DB/log/atlas cache (e.g. .matrix_player). Empty = write beside the executable.")
set(APP_SHELL_APP_NAME  "matrix_player"    CACHE STRING "" FORCE)
set(APP_SHELL_WIN_CLASS "MatrixPlayerMain" CACHE STRING "" FORCE)
# The two the USER sees. Both desktop hosts used to have these written into
# them as literals; naming them here is what let a second consumer stop
# announcing itself as this one.
set(APP_SHELL_WINDOW_TITLE "Matrix Player" CACHE STRING "" FORCE)
set(APP_SHELL_APP_ID       "matrix-player" CACHE STRING "" FORCE)
if(MATRIX_STATE_HOME)
    set(APP_SHELL_STATE_HOME "${MATRIX_STATE_HOME}" CACHE STRING "" FORCE)
endif()
add_subdirectory(framework/app_shell app_shell_build)

# ---------- min_text_size (build-time UI text-size floor generator) ----------
# The recipe now belongs to app_shell (its ui_metrics.hh is what includes the
# generated header); this call is the part that is genuinely OURS — the list of
# faces, which must be exactly the ones gui/src/ui_fonts.hh loads, because the
# emitted floor is the WORST case across them and a face left out here could
# render below it.
include(AppShellMinTextSize)
app_shell_generate_min_text_size(
    ${CMAKE_SOURCE_DIR}/assets/fonts/newcomputermodern/NewCM10-Regular.otf
    ${CMAKE_SOURCE_DIR}/assets/fonts/newcomputermodern/NewCM10-Bold.otf
    ${CMAKE_SOURCE_DIR}/assets/fonts/newcomputermodern/NewCM10-Italic.otf
    ${CMAKE_SOURCE_DIR}/assets/fonts/newcomputermodern/NewCMMono10-Regular.otf)

# app_shell's ui_metrics.cc INCLUDES the header that call just declared, so it
# has to wait for it. Stated here rather than inside app_shell's own
# CMakeLists.txt because the target does not exist yet when CMake reads that
# file — the consumer owns the font list, so the consumer owns the ordering.
add_dependencies(app_shell generate_ui_min_text_size)

# ---------- shaders (compile next to the exe) ----------
# vk_canvas's FileAssetReader reads <exe dir>/assets/shaders/, same convention
# as the library's own demo. composite/tiling/coverage/msdf are canonically
# owned by the font engine submodule; overlay/image/shape are vk_canvas's own.
include(framework/vk_canvas/cmake/VceShaders.cmake)
vce_compile_slang(compile_shared_shaders
    ${CMAKE_BINARY_DIR}/assets/shaders
    ${CMAKE_SOURCE_DIR}/framework/vk_canvas/first_party/vulkan_font_engine/shaders_src
    composite_vert
    composite_frag
    tiling
    coverage
    msdf_vert
    msdf_frag
    glyph_area
    glyph_resolve
)
vce_compile_slang(compile_vk_canvas_shaders
    ${CMAKE_BINARY_DIR}/assets/shaders
    ${CMAKE_SOURCE_DIR}/framework/vk_canvas/shaders_src
    overlay_vert
    overlay_frag
    image_vert
    image_frag
    shape_vert
    shape_frag
)

# ---------- matrix_core (portable library: scan/db/eq/decode) ----------
add_subdirectory(core)

# ---------- matrix_player (gui/, the executable) ----------
add_subdirectory(gui)

# ---------- matrix_ab_test (standalone A/B test tool, opt-in, Windows-only) ----------
if(MATRIX_BUILD_AB_TEST AND WIN32)
    add_executable(matrix_ab_test tools/ab_test.cpp)
    set_target_properties(matrix_ab_test PROPERTIES WIN32_EXECUTABLE FALSE)
    if(MSVC)
        target_compile_options(matrix_ab_test PRIVATE /W3)
    else()
        target_compile_options(matrix_ab_test PRIVATE -Wall)
    endif()
    target_compile_definitions(matrix_ab_test PRIVATE NOMINMAX)
    target_link_libraries(matrix_ab_test PRIVATE ae_usb winusb setupapi avrt)
elseif(MATRIX_BUILD_AB_TEST)
    message(WARNING "MATRIX_BUILD_AB_TEST uses <windows.h> directly and is "
                     "Windows-only — ignored on this platform.")
endif()
