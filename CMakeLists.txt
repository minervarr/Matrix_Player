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

# ---------- min_text_size (build-time UI text-size floor generator) ----------
# Computes the minimum device-pixel size each baked font can render at before
# its stroke geometry drops below what antialiased rendering can represent
# (see framework/vk_canvas/.../tools/min_text_size/min_text_size.cc), and
# emits a header the app statically asserts its UI text sizes against.
# Built via ExternalProject_Add (own isolated CMake tree) for the same reason
# as libjpeg-turbo above: its CMakeLists independently builds FreeType +
# msdfgen, which would collide by target name if add_subdirectory()'d
# directly (vk_canvas_core, linked below, already builds its own copies of
# both for the MSDF text engine).
set(MIN_TEXT_SIZE_INSTALL_DIR ${CMAKE_BINARY_DIR}/min-text-size-install)
if(WIN32)
    set(MIN_TEXT_SIZE_BIN_NAME min_text_size.exe)
else()
    set(MIN_TEXT_SIZE_BIN_NAME min_text_size)
endif()
ExternalProject_Add(min_text_size_ext
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/framework/vk_canvas/first_party/vulkan_font_engine/tools/min_text_size
    CMAKE_ARGS
        -G${CMAKE_GENERATOR}
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
        -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
    INSTALL_COMMAND ${CMAKE_COMMAND} -E copy
        <BINARY_DIR>/${MIN_TEXT_SIZE_BIN_NAME}
        ${MIN_TEXT_SIZE_INSTALL_DIR}/${MIN_TEXT_SIZE_BIN_NAME}
    BUILD_BYPRODUCTS ${MIN_TEXT_SIZE_INSTALL_DIR}/${MIN_TEXT_SIZE_BIN_NAME})

set(UI_MIN_TEXT_SIZE_HEADER ${CMAKE_BINARY_DIR}/generated/ui_min_text_size.gen.h)
# Must list exactly the faces gui/src/ui_fonts.hh loads: the emitted floor is
# the WORST case across them, and a face left out here could render below it.
set(UI_FONT_FILES
    ${CMAKE_SOURCE_DIR}/assets/fonts/newcomputermodern/NewCM10-Regular.otf
    ${CMAKE_SOURCE_DIR}/assets/fonts/newcomputermodern/NewCM10-Bold.otf
    ${CMAKE_SOURCE_DIR}/assets/fonts/newcomputermodern/NewCM10-Italic.otf
    ${CMAKE_SOURCE_DIR}/assets/fonts/newcomputermodern/NewCMMono10-Regular.otf)
add_custom_command(
    OUTPUT ${UI_MIN_TEXT_SIZE_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND ${MIN_TEXT_SIZE_INSTALL_DIR}/${MIN_TEXT_SIZE_BIN_NAME}
        --emit-header ${UI_MIN_TEXT_SIZE_HEADER} 0.5 512 ${UI_FONT_FILES}
    DEPENDS min_text_size_ext ${UI_FONT_FILES}
    COMMENT "Computing minimum readable UI text size from font geometry")
add_custom_target(generate_ui_min_text_size DEPENDS ${UI_MIN_TEXT_SIZE_HEADER})

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
    target_compile_options(matrix_ab_test PRIVATE /W3)
    target_compile_definitions(matrix_ab_test PRIVATE NOMINMAX)
    target_link_libraries(matrix_ab_test PRIVATE ae_usb winusb setupapi avrt)
elseif(MATRIX_BUILD_AB_TEST)
    message(WARNING "MATRIX_BUILD_AB_TEST uses <windows.h> directly and is "
                     "Windows-only — ignored on this platform.")
endif()
