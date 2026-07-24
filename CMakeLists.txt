cmake_minimum_required(VERSION 3.22.1)
project("matrix_player_windows")

set(CMAKE_CXX_STANDARD 17)

# Whole-program optimization (MSVC /GL + /LTCG) for Release builds: lets the
# linker inline/dead-strip across translation units instead of stopping at
# each .obj's boundary. Must be set before add_subdirectory()/add_executable()
# calls below — CMake reads it at target-creation time, not build time.
# Costs link time (noticeably longer than a normal Release link); harmless
# for Debug (this only fires when CMAKE_BUILD_TYPE=Release, which is all
# build.bat ever configures).
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Standalone dev tool, not part of the shipped player — off by default so a
# normal build (or `build.bat`) never compiles or links it. Turn on with
# `cmake -B build -DMATRIX_BUILD_AB_TEST=ON` (or re-run build.bat after
# editing this default) when you actually want to run the A/B comparison.
option(MATRIX_BUILD_AB_TEST "Build the standalone audio A/B comparison tool (tools/ab_test.cpp)" OFF)

# ---------- audio_engine static lib (firstparty submodule) ----------
add_subdirectory(libs/firstparty/audio_engine)

# ---------- sqlite3 (vendored amalgamation) ----------
add_library(sqlite3 STATIC libs/thirdparty/sqlite3.c)
target_include_directories(sqlite3 PUBLIC libs/thirdparty)
target_compile_options(sqlite3 PRIVATE /W0)

# ---------- soxr resampler ----------
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/libs/thirdparty/soxr/cmake/Modules")
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WITH_OPENMP OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WITH_LSR_BINDINGS OFF CACHE BOOL "" FORCE)
add_subdirectory(libs/thirdparty/soxr)

# ---------- libjpeg-turbo (higher-fidelity JPEG decode for album art) ----------
# libjpeg-turbo's own CMakeLists.txt refuses add_subdirectory() (it FATAL_ERRORs
# unless it's the top-level project) — build it out-of-tree via ExternalProject
# and expose it as an imported target instead.
include(ExternalProject)
set(TURBOJPEG_INSTALL_DIR ${CMAKE_BINARY_DIR}/libjpeg-turbo-install)
# CMAKE_CXX_FLAGS/CMAKE_C_FLAGS forwarded explicitly: ExternalProject_Add runs
# an entirely separate `cmake configure`, so build.bat's v3/v4 arch flag
# (passed to THIS configure as -DCMAKE_CXX_FLAGS=/arch:AVX2) would otherwise
# never reach this nested build — libjpeg-turbo would silently compile at the
# generic baseline even when you asked for AVX2/AVX-512.
ExternalProject_Add(libjpeg_turbo_ext
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/libs/thirdparty/libjpeg-turbo
    CMAKE_ARGS
        -G${CMAKE_GENERATOR}
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
        -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
        -DENABLE_SHARED=OFF
        -DENABLE_STATIC=ON
        -DWITH_TURBOJPEG=ON
        -DWITH_TOOLS=OFF
        -DWITH_TESTS=OFF
        -DWITH_JAVA=OFF
        -DCMAKE_INSTALL_PREFIX=${TURBOJPEG_INSTALL_DIR}
    INSTALL_DIR ${TURBOJPEG_INSTALL_DIR}
    BUILD_BYPRODUCTS ${TURBOJPEG_INSTALL_DIR}/lib/turbojpeg-static.lib)

add_library(turbojpeg-static STATIC IMPORTED GLOBAL)
file(MAKE_DIRECTORY ${TURBOJPEG_INSTALL_DIR}/include)  # must exist at configure time
set_target_properties(turbojpeg-static PROPERTIES
    IMPORTED_LOCATION ${TURBOJPEG_INSTALL_DIR}/lib/turbojpeg-static.lib
    INTERFACE_INCLUDE_DIRECTORIES ${TURBOJPEG_INSTALL_DIR}/include)
add_dependencies(turbojpeg-static libjpeg_turbo_ext)

# ---------- min_text_size (build-time UI text-size floor generator) ----------
# Computes the minimum device-pixel size each baked font can render at before
# its stroke geometry drops below what antialiased rendering can represent
# (see libs/firstparty/vk_canvas/.../tools/min_text_size/min_text_size.cc),
# and emits a header the app statically asserts its UI text sizes against.
# Built via ExternalProject_Add (own isolated CMake tree) for the same reason
# as libjpeg-turbo above: its CMakeLists independently builds FreeType +
# msdfgen, which would collide by target name if add_subdirectory()'d
# directly (vk_canvas_core, linked below, already builds its own copies of
# both for the MSDF text engine).
set(MIN_TEXT_SIZE_INSTALL_DIR ${CMAKE_BINARY_DIR}/min-text-size-install)
ExternalProject_Add(min_text_size_ext
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/libs/firstparty/vk_canvas/first_party/vulkan_font_engine/tools/min_text_size
    CMAKE_ARGS
        -G${CMAKE_GENERATOR}
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
        -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
    INSTALL_COMMAND ${CMAKE_COMMAND} -E copy
        <BINARY_DIR>/min_text_size.exe
        ${MIN_TEXT_SIZE_INSTALL_DIR}/min_text_size.exe
    BUILD_BYPRODUCTS ${MIN_TEXT_SIZE_INSTALL_DIR}/min_text_size.exe)

set(UI_MIN_TEXT_SIZE_HEADER ${CMAKE_BINARY_DIR}/generated/ui_min_text_size.gen.h)
set(UI_FONT_FILES
    ${CMAKE_SOURCE_DIR}/fonts/lm/lmroman10-regular.otf
    ${CMAKE_SOURCE_DIR}/fonts/lm/lmroman10-bold.otf
    ${CMAKE_SOURCE_DIR}/fonts/lm/lmroman10-italic.otf
    ${CMAKE_SOURCE_DIR}/fonts/lm/lmmono10-regular.otf)
add_custom_command(
    OUTPUT ${UI_MIN_TEXT_SIZE_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND ${MIN_TEXT_SIZE_INSTALL_DIR}/min_text_size.exe
        --emit-header ${UI_MIN_TEXT_SIZE_HEADER} 0.5 512 ${UI_FONT_FILES}
    DEPENDS min_text_size_ext ${UI_FONT_FILES}
    COMMENT "Computing minimum readable UI text size from font geometry")
add_custom_target(generate_ui_min_text_size DEPENDS ${UI_MIN_TEXT_SIZE_HEADER})

# ---------- vk_canvas (Vulkan 2D canvas engine, firstparty submodule) ----------
# This app is the Win32 "platform" consumer of vk_canvas_core, the same way
# libs/firstparty/vk_canvas/platform/windows is inside the library's own repo.
add_subdirectory(libs/firstparty/vk_canvas/core vk_canvas_build)

# Shaders compile next to the exe: vk_canvas's FileAssetReader reads
# <exe dir>/assets/shaders/, same convention as the library's own demo.
# composite/tiling/coverage/msdf are canonically owned by the font engine
# submodule (vk_canvas used to hand-duplicate these — see its shaders_src
# comment); overlay/image are genuinely vk_canvas's own.
include(libs/firstparty/vk_canvas/cmake/VceShaders.cmake)
vce_compile_slang(compile_shared_shaders
    ${CMAKE_BINARY_DIR}/assets/shaders
    ${CMAKE_SOURCE_DIR}/libs/firstparty/vk_canvas/first_party/vulkan_font_engine/shaders_src
    composite_vert
    composite_frag
    tiling
    coverage
    msdf_vert
    msdf_frag
)
vce_compile_slang(compile_vk_canvas_shaders
    ${CMAKE_BINARY_DIR}/assets/shaders
    ${CMAKE_SOURCE_DIR}/libs/firstparty/vk_canvas/shaders_src
    overlay_vert
    overlay_frag
    image_vert
    image_frag
    shape_vert
    shape_frag
)

# ---------- matrix_player_windows executable ----------
add_executable(${CMAKE_PROJECT_NAME} WIN32
    src/main.cpp
    src/player_window.cpp
    src/wasapi_output.cpp
    src/library.cpp
    src/decoder.cpp
    src/db.cpp
    src/art_window.cpp
    src/eq_profiles.cpp
    src/eq_manager.cpp
    src/app.rc
    libs/firstparty/vk_canvas/platform/windows/win32_platform.cc)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    src
    libs/thirdparty
    libs/thirdparty/soxr/src
    ${CMAKE_BINARY_DIR}/generated
    libs/firstparty/vk_canvas/platform/windows)

# No hardcoded /O2 here: CMAKE_CXX_FLAGS_RELEASE/_DEBUG (set by
# CMAKE_BUILD_TYPE) already apply the right optimization level per config —
# forcing /O2 unconditionally would win over Debug's /Od on the command line
# (MSVC takes the last /O flag) and silently turn "build.bat debug" into an
# optimized build anyway.
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
    /W3)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    NOMINMAX
    UNICODE
    _UNICODE)

# Copy EQ profiles JSON next to the executable
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/eq_profiles.json
        $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/eq_profiles.json)

# Copy bundled fonts (Latin Modern + multi-script fallback faces) next to the executable
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/fonts
        $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/fonts)

add_dependencies(${CMAKE_PROJECT_NAME} compile_shared_shaders compile_vk_canvas_shaders generate_ui_min_text_size)

if(WIN32)
    set(MATRIX_WIN32_USB_LIBS winusb setupapi)
endif()

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    ae_usb
    ${MATRIX_WIN32_USB_LIBS}
    sqlite3
    soxr
    turbojpeg-static
    vk_canvas_core
    comctl32
    comdlg32
    shell32
    ole32
    mmdevapi
    avrt
    dbghelp)

# ---------- matrix_ab_test (standalone A/B test tool, opt-in) ----------
if(MATRIX_BUILD_AB_TEST)
    add_executable(matrix_ab_test tools/ab_test.cpp)
    set_target_properties(matrix_ab_test PROPERTIES WIN32_EXECUTABLE FALSE)
    target_compile_options(matrix_ab_test PRIVATE /W3)  # see matrix_player_windows's /O2 comment above
    target_compile_definitions(matrix_ab_test PRIVATE NOMINMAX)
    target_include_directories(matrix_ab_test PRIVATE
        libs/thirdparty
        libs/firstparty/audio_engine/src/main/cpp
        libs/firstparty/audio_engine/src/main/cpp/libusb/libusb)
    target_link_libraries(matrix_ab_test PRIVATE
        audio_engine_windows usb1 winusb setupapi avrt)
endif()
