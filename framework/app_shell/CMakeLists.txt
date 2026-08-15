# ── app_shell ────────────────────────────────────────────────────────────────
#
# The application shell: the Host/AppView seam, its three real implementations,
# and the small pure-logic modules every app built on it needs (where files
# live, one type scale, which way round the window is).
#
# Two kinds of target, and the split is deliberate:
#
#   app_shell            — portable, links NOTHING. Pure logic plus headers.
#   app_shell_win32      — one platform's Host each. These link that platform's
#   app_shell_wayland      vk_canvas backend and its system libraries.
#   app_shell_android
#
# A consumer links `app_shell` plus exactly one of the others. The tests below
# link neither: they compile the pure sources directly, which is the property
# that keeps them honest.

cmake_minimum_required(VERSION 3.22)

set(APP_SHELL_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "app_shell source root")
set(VK_CANVAS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vk_canvas CACHE PATH
    "Where vk_canvas lives; app_shell needs its core headers and one backend")

# Also pushed to the parent: the CONSUMER calls
# app_shell_generate_min_text_size(), because the font list is the consumer's.
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} PARENT_SCOPE)

# ── The portable half ────────────────────────────────────────────────────────
#
# ui_metrics.cc is the only file here that includes anything of vk_canvas's
# (layout.hh, for UiScale) and the only one that needs the generated
# ui_min_text_size.gen.h — which is why the include directories below carry both
# and why a consumer must add_dependencies(... generate_ui_min_text_size).
# app_paths.cc is the DESKTOP answer to exeDir()/stateDir(): probe the OS for
# where the binary is, and $HOME for where to write. Neither question means
# anything on Android, where both are handed to the app for free — so there the
# same two functions come from os/app_paths_android.cc instead (compiled into
# app_shell_android), and this file is left out entirely. Building both is a
# duplicate-symbol link error, which is exactly the right failure for two
# implementations of one seam.
set(APP_SHELL_SOURCES ui_metrics.cc ui_orientation.cc)
if(NOT ANDROID)
    list(APPEND APP_SHELL_SOURCES app_paths.cc)
endif()

add_library(app_shell STATIC ${APP_SHELL_SOURCES})

# vulkan_font_engine's core is here because vk_canvas's platform.hh includes
# asset_reader.hh, whose definition that submodule owns — the same transitive
# hop vk_canvas_core itself makes.
target_include_directories(app_shell PUBLIC
    ${APP_SHELL_DIR}
    ${VK_CANVAS_DIR}/core
    ${VK_CANVAS_DIR}/first_party/vulkan_font_engine/core
    ${CMAKE_BINARY_DIR}/generated)

target_compile_features(app_shell PUBLIC cxx_std_17)

# ── The three things an application tells its shell ─────────────────────────
#
# All three are cache variables with working defaults, so app_shell configures
# and builds on its own; a consumer sets them BEFORE add_subdirectory().
#
#   APP_SHELL_STATE_HOME  — directory name under $HOME for what the app WRITES.
#                           Empty (default) means beside the executable, which
#                           keeps a build tree and a portable tarball one
#                           self-contained folder; a name is what a system
#                           package needs, since /opt is root-owned.
#   APP_SHELL_APP_NAME    — names the log file and the Android logcat tag.
#   APP_SHELL_WIN_CLASS   — the Win32 window class. Only has to be unique per
#                           process; it is a cache variable purely so an app
#                           that already shipped one keeps it.
set(APP_SHELL_STATE_HOME "" CACHE STRING
    "Directory name under $HOME for files the app writes. Empty = beside the executable.")
set(APP_SHELL_APP_NAME "app" CACHE STRING
    "Base name for the log file and the Android logcat tag.")
set(APP_SHELL_WIN_CLASS "AppShellMain" CACHE STRING
    "Win32 window class name for the main window.")

set(APP_SHELL_NAME_DEFS
    APP_SHELL_LOG_NAME="${APP_SHELL_APP_NAME}"
    APP_SHELL_LOG_NAME_W=L"${APP_SHELL_APP_NAME}"
    APP_SHELL_WINDOW_CLASS=L"${APP_SHELL_WIN_CLASS}")

target_compile_definitions(app_shell PUBLIC ${APP_SHELL_NAME_DEFS})
if(APP_SHELL_STATE_HOME)
    target_compile_definitions(app_shell PRIVATE
        APP_SHELL_STATE_HOME="${APP_SHELL_STATE_HOME}")
endif()

# ── The three hosts ──────────────────────────────────────────────────────────
#
# Each is guarded on the platform AND on its vk_canvas backend actually being
# present, so a build tree that only configured one of them defines only that
# one — the same shape audio_engine uses for ae_alsa/ae_jack.
if(WIN32)
    add_library(app_shell_win32 STATIC os/win32_host.cc)
    target_link_libraries(app_shell_win32 PUBLIC app_shell)
    target_include_directories(app_shell_win32 PUBLIC ${VK_CANVAS_DIR}/platform/windows)
    target_link_libraries(app_shell_win32 PUBLIC dbghelp winmm)
elseif(ANDROID)
    # native_app_glue ships inside the NDK, so only the consumer knows where it
    # is. It must be set before this file is read.
    if(NOT APP_SHELL_NATIVE_APP_GLUE_DIR)
        message(FATAL_ERROR
            "app_shell on Android needs APP_SHELL_NATIVE_APP_GLUE_DIR "
            "(the NDK's sources/android/native_app_glue)")
    endif()
    add_library(app_shell_android STATIC
        os/android_host.cc
        os/app_paths_android.cc
        os/launch_intent.cc
        os/safe_area.cc
        os/storage_permission.cc)
    target_link_libraries(app_shell_android PUBLIC app_shell android log)
    target_include_directories(app_shell_android PUBLIC
        ${APP_SHELL_DIR}/os
        ${APP_SHELL_NATIVE_APP_GLUE_DIR}
        ${VK_CANVAS_DIR}/platform/android)
else()
    add_library(app_shell_wayland STATIC os/wayland_host.cc)
    target_link_libraries(app_shell_wayland PUBLIC app_shell)
    target_include_directories(app_shell_wayland PUBLIC ${VK_CANVAS_DIR}/platform/linux)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
#
# Debug-only, plain assert(), no framework — the convention this whole family of
# repositories uses. They COMPILE the sources rather than linking app_shell, so
# a test can never quietly start depending on Vulkan; ui_metrics_test is the one
# that needs an include path at all, for layout.hh and the generated floor.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT ANDROID)
    add_executable(ui_metrics_test tests/ui_metrics_test.cc ui_metrics.cc)
    target_include_directories(ui_metrics_test PRIVATE
        ${APP_SHELL_DIR} ${VK_CANVAS_DIR}/core ${CMAKE_BINARY_DIR}/generated)
    add_dependencies(ui_metrics_test generate_ui_min_text_size)

    add_executable(ui_orientation_test tests/ui_orientation_test.cc ui_orientation.cc)
    target_include_directories(ui_orientation_test PRIVATE ${APP_SHELL_DIR})
endif()
