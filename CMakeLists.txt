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

# Standalone dev tool, not part of the shipped player — off by default so a
# normal build never compiles or links it. Turn on with
# `cmake -B build -DMATRIX_BUILD_AB_TEST=ON`. Windows-only (uses <windows.h>
# directly) regardless of this flag.
option(MATRIX_BUILD_AB_TEST "Build the standalone audio A/B comparison tool (tools/ab_test.cpp)" OFF)

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

# ---------- libjpeg-turbo (higher-fidelity JPEG decode for album art) ----------
# libjpeg-turbo's own CMakeLists.txt refuses add_subdirectory() (it FATAL_ERRORs
# unless it's the top-level project) — build it out-of-tree via ExternalProject
# and expose it as an imported target instead.
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
set(UI_FONT_FILES
    ${CMAKE_SOURCE_DIR}/assets/fonts/lm/lmroman10-regular.otf
    ${CMAKE_SOURCE_DIR}/assets/fonts/lm/lmroman10-bold.otf
    ${CMAKE_SOURCE_DIR}/assets/fonts/lm/lmroman10-italic.otf
    ${CMAKE_SOURCE_DIR}/assets/fonts/lm/lmmono10-regular.otf)
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
