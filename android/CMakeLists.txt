cmake_minimum_required(VERSION 3.22.1)
project(matrix_player_android)

# Reached via app/build.gradle's externalNativeBuild.cmake.path. REPO_ROOT is
# two levels up from this file (android/ is a sibling of core/, gui/,
# framework/ at the repo root).
set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

set(NATIVE_APP_GLUE ${ANDROID_NDK}/sources/android/native_app_glue)

# audio_engine: ae_core/ae_usb/ae_flac/ae_aaudio/ae_mediacodec linked directly
# as C++ targets (no C ABI — this app's own native code is C++, no JNI
# language boundary to cross). AUDIO_ENGINE_BUILD_TOOLS is desktop-only
# (gated behind the UNIX-and-not-Android block in its own CMakeLists.txt), so
# nothing extra builds here.
add_subdirectory(${REPO_ROOT}/framework/audio_engine ${CMAKE_BINARY_DIR}/audio_engine)

# sqlite3 (vendored amalgamation, third_party/) — matrix_core links this by
# name (core/CMakeLists.txt:26), same target root CMakeLists.txt defines for
# the desktop build (CMakeLists.txt:250). Desktop reaches it by add_subdirectory
# chaining down from the repo root; Android's own top-level project doesn't
# go through that file (its add_subdirectory(gui) isn't Android-portable), so
# it's defined here instead — same three lines, not a fork.
add_library(sqlite3 STATIC ${REPO_ROOT}/third_party/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${REPO_ROOT}/third_party)
target_compile_options(sqlite3 PRIVATE -w)

# matrix_core: library scan/DB/decoder logic, unmodified. Under the Android
# NDK toolchain PLATFORM_ID is "Android" (not "Windows"), so its own
# $<IF:$<PLATFORM_ID:Windows>,...> FolderWatcher selection already falls
# through to src/os/linux_folder_watch.cpp — confirmed safe (inotify/eventfd/
# poll, no Linux-desktop-specific assumptions) in the design doc's review.
add_subdirectory(${REPO_ROOT}/core ${CMAKE_BINARY_DIR}/matrix_core)

# vk_canvas_core: Vulkan canvas/text engine, unmodified. Its own CMakeLists.txt
# already has an if(ANDROID) branch linking vulkan/android/log directly (NDK
# provides libvulkan), so PUBLIC linking the target alone is enough.
add_subdirectory(${REPO_ROOT}/framework/vk_canvas/core ${CMAKE_BINARY_DIR}/vk_canvas_core)

# Shaders: compiled via the DESKTOP root CMakeLists.txt's list (includes
# msdf_vert/msdf_frag/glyph_area/glyph_resolve), not vk_canvas's own Android
# demo list (framework/vk_canvas/platform/android/CMakeLists.txt), which is
# missing MSDF because its test scene never calls Renderer::initMsdf — this
# app draws text, so it needs the full set. Output goes to
# app/src/main/assets/shaders/ so AGP packages it into the APK, same
# convention vk_canvas's own Android demo uses for its own (smaller) list.
set(MATRIX_ANDROID_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/app/src/main/assets/shaders)
include(${REPO_ROOT}/framework/vk_canvas/cmake/VceShaders.cmake)
vce_compile_slang(compile_shared_shaders
    ${MATRIX_ANDROID_SHADER_DIR}
    ${REPO_ROOT}/framework/vk_canvas/first_party/vulkan_font_engine/shaders_src
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
    ${MATRIX_ANDROID_SHADER_DIR}
    ${REPO_ROOT}/framework/vk_canvas/shaders_src
    overlay_vert
    overlay_frag
    image_vert
    image_frag
    shape_vert
    shape_frag
)

# ui_min_text_size.gen.h: ui_metrics.hh needs this build-generated header, but
# it cannot be regenerated under the NDK cross-compiler — the desktop root
# CMakeLists.txt's min_text_size_ext ExternalProject_Add forwards
# CMAKE_C_COMPILER/CMAKE_CXX_COMPILER into its sub-build, which under an
# Android-toolchain configure would try to compile a HOST tool with the ARM64
# cross-compiler. It's pure font-geometry math (platform-independent output),
# so copy the already-generated copy from an existing desktop build instead
# of re-deriving it here.
#
# ANY desktop build tree will do, and it must: this used to name
# build/windows/generated/ alone, so an Android build on a machine that had
# only ever built for Linux failed with "not found" and told the developer to
# do something they had already done. The output is identical everywhere — it
# is derived from the shipped font outlines, not from the toolchain.
set(UI_MIN_TEXT_SIZE_CANDIDATES
    ${REPO_ROOT}/build/linux/generated/ui_min_text_size.gen.h
    ${REPO_ROOT}/build/linux_debug/generated/ui_min_text_size.gen.h
    ${REPO_ROOT}/build/windows/generated/ui_min_text_size.gen.h
    ${REPO_ROOT}/build/windows_debug/generated/ui_min_text_size.gen.h)
set(UI_MIN_TEXT_SIZE_SRC "")
foreach(candidate ${UI_MIN_TEXT_SIZE_CANDIDATES})
    if(EXISTS ${candidate})
        set(UI_MIN_TEXT_SIZE_SRC ${candidate})
        break()
    endif()
endforeach()
if(NOT UI_MIN_TEXT_SIZE_SRC)
    message(FATAL_ERROR
        "ui_min_text_size.gen.h not found in any desktop build tree "
        "(looked in build/{linux,linux_debug,windows,windows_debug}/generated/). "
        "Run a desktop build first (scripts/linux/build.sh --debug or "
        "scripts/windows/build.ps1 -Release) to generate it — see "
        "docs/superpowers/specs/2026-08-08-android-native-port-design.md's "
        "'Three findings' section.")
endif()
message(STATUS "ui_min_text_size.gen.h taken from ${UI_MIN_TEXT_SIZE_SRC}")
set(UI_MIN_TEXT_SIZE_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${UI_MIN_TEXT_SIZE_DIR})
configure_file(${UI_MIN_TEXT_SIZE_SRC} ${UI_MIN_TEXT_SIZE_DIR}/ui_min_text_size.gen.h COPYONLY)

# vk_canvas's platform/android/ substrate — jni_util.hh (env_for/check_exc),
# fullscreen.hh (immersive mode + nav-bar height, header-only), orientation.{hh,cc}
# (accelerometer), android_platform.{hh,cc} (AndroidAssetReader/
# AndroidSurfaceProvider — the platform.hh seam impls AndroidHost's Renderer
# needs). input_handler.hh/.cc is reference-only (its TextBuffer/UndoRedo-
# coupled class isn't reusable) — AndroidHost hand-writes touch dispatch
# using the same AMotionEvent_* primitives instead.
set(VK_CANVAS_ANDROID_PLATFORM ${REPO_ROOT}/framework/vk_canvas/platform/android)

# gui/src: reused verbatim, unmodified — theme.hh/color.hh/layout_rect.hh/
# ui_metrics.hh+.cc/ui_icons.hh+.cc+ui_icons_draw.cc. Confirmed portable
# (no Host/PlayerWindow/Win32/Wayland coupling) in the design review; only
# ui_metrics.hh/ui_icons.hh are actually drawn from by android_player_view.cc
# today, but all three .cc files are compiled here so the whole set stays a
# verified-working dependency for the touch-UI screens this slice defers.
set(MATRIX_GUI_SRC ${REPO_ROOT}/gui/src)

add_library(matrix_player_android SHARED
    src/main.cc
    src/android_host.cc
    src/android_player_view.cc
    src/launch_intent.cc
    src/safe_area.cc
    src/storage_permission.cc
    src/app_paths_android.cc
    ${VK_CANVAS_ANDROID_PLATFORM}/orientation.cc
    ${VK_CANVAS_ANDROID_PLATFORM}/android_platform.cc
    ${MATRIX_GUI_SRC}/ui_metrics.cc
    ${MATRIX_GUI_SRC}/ui_icons.cc
    ${MATRIX_GUI_SRC}/ui_icons_draw.cc
    ${NATIVE_APP_GLUE}/android_native_app_glue.c)

target_include_directories(matrix_player_android PRIVATE
    ${NATIVE_APP_GLUE}
    ${VK_CANVAS_ANDROID_PLATFORM}
    ${MATRIX_GUI_SRC}
    ${UI_MIN_TEXT_SIZE_DIR})

target_link_libraries(matrix_player_android
    android log
    ae_core ae_usb usb1 ae_flac ae_aaudio ae_mediacodec
    matrix_core
    vk_canvas_core)

add_dependencies(matrix_player_android compile_shared_shaders compile_vk_canvas_shaders)

# Keep the glue's NativeActivity entry point alive against linker GC (same
# rule framework/vk_canvas/platform/android/CMakeLists.txt already documents).
target_link_options(matrix_player_android PRIVATE -u ANativeActivity_onCreate)

# Bundled fonts (New Computer Modern + icon font) next to the shaders — same
# POST_BUILD-on-the-native-target shape gui/CMakeLists.txt uses for fonts on
# desktop (fonts are static input, unlike the shader compile step, so a plain
# POST_BUILD is fine here — no separate ALL target needed). The Gradle-side
# fix in app/build.gradle (mergeDebugAssets/mergeReleaseAssets depending on
# the buildCMake* tasks) already covers this ordering too, same as it does
# for the compiled shaders above.
add_custom_command(TARGET matrix_player_android POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${REPO_ROOT}/assets/fonts
        ${MATRIX_ANDROID_SHADER_DIR}/../fonts)
