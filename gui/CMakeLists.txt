# ---------------------------------------------------------------------------
# matrix_player executable. Shared portable sources (player_view.cc,
# art_view.cc, gui_main.cc, ...) plus exactly one platform Host backend
# (os/windows_host.cc or os/linux_host.cc — see src/host.hh).
# ---------------------------------------------------------------------------

# Where the app writes its database, log and font-atlas cache.
#
# Empty (the default) keeps every one of them beside the executable, which is
# what a build tree and the self-contained tarballs in dist/linux/ have always
# been — move the folder anywhere and it still works. Set it to a directory
# NAME (not a path) and those three move to $HOME/<name>/ instead, which is
# what a system package needs: /opt/matrix_player is installed root-owned, and
# the ~45 MB atlas cache is per-user regardless, since bakeFallbackGlyphs()
# bakes whatever scripts the listener's own library happens to contain.
#
#   cmake -B build -DMATRIX_STATE_HOME=.matrix_player
#
# Deliberately not an XDG lookup — see the comment in src/app_paths.hh.
# Read-only data (fonts/, assets/shaders/, eq_profiles.json) is NOT affected
# and stays beside the executable in every configuration.
set(MATRIX_STATE_HOME "" CACHE STRING
    "Directory name under $HOME for the DB/log/atlas cache (e.g. .matrix_player). Empty = write beside the executable.")

set(MATRIX_PLAYER_PORTABLE_SOURCES
    src/gui_main.cc
    src/app_paths.cc
    src/player_view.cc
    src/art_view.cc
    src/ui_metrics.cc
    src/ui_orientation.cc
    src/rail_layout.cc
    src/ui_icons.cc
    src/ui_icons_draw.cc
    src/panels/settings_panels.cc)

if(WIN32)
    add_executable(matrix_player WIN32
        ${MATRIX_PLAYER_PORTABLE_SOURCES}
        src/wasapi_output.cc
        src/os/windows_host.cc
        src/app.rc
        ${CMAKE_SOURCE_DIR}/framework/vk_canvas/platform/windows/win32_platform.cc)

    target_include_directories(matrix_player PRIVATE
        src
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_SOURCE_DIR}/third_party/soxr/src
        ${CMAKE_BINARY_DIR}/generated
        ${CMAKE_SOURCE_DIR}/framework/vk_canvas/platform/windows)

    # No hardcoded /O2 here: CMAKE_CXX_FLAGS_RELEASE/_DEBUG (set by
    # CMAKE_BUILD_TYPE) already apply the right optimization level per config —
    # forcing /O2 unconditionally would win over Debug's /Od on the command line
    # (MSVC takes the last /O flag) and silently turn a debug build into an
    # optimized build anyway.
    if(MSVC)
        target_compile_options(matrix_player PRIVATE /W3)
    else()
        target_compile_options(matrix_player PRIVATE -Wall)
    endif()

    target_compile_definitions(matrix_player PRIVATE
        NOMINMAX
        UNICODE
        _UNICODE)

    add_dependencies(matrix_player compile_shared_shaders compile_vk_canvas_shaders generate_ui_min_text_size)

    target_link_libraries(matrix_player PRIVATE
        matrix_core
        ae_usb
        winusb
        setupapi
        sqlite3
        soxr
        turbojpeg-static
        vk_canvas_core
        ole32
        mmdevapi
        avrt
        dbghelp
        winmm)

    # GNU ld (MSYS2/MinGW's linker) doesn't honor #pragma comment(lib,...)
    # .drectve directives the way MSVC's link.exe/lld-link does — winmm above
    # is the one library in this tree that had no explicit link fallback
    # before this (windows_host.cc's #pragma comment(lib,"winmm.lib") is a
    # silent no-op here). ole32/mmdevapi/avrt/dbghelp are also pragma'd but
    # were already listed explicitly, so they were never actually at risk.
    #
    # Static-link flags (-static/-stdlib=libc++) are set globally in the root
    # CMakeLists.txt, not per-target here — every Windows executable this
    # project builds (matrix_player, the Debug test binaries, matrix_ab_test)
    # needs to be self-contained, not just the shipped one; a test binary
    # that only runs with MSYS2's ucrt64/bin on PATH defeats the point.
else()
    # AlsaOutput/JackOutput are secondary Linux outputs (see gui/src/os/) —
    # only built when audio_engine actually found ALSA/jack2 dev headers and
    # defined the corresponding target (same optionality as the ae_alsa/
    # ae_jack tools in framework/audio_engine/CMakeLists.txt).
    set(MATRIX_LINUX_AUDIO_SOURCES)
    set(MATRIX_LINUX_AUDIO_LIBS)
    if(TARGET ae_alsa)
        list(APPEND MATRIX_LINUX_AUDIO_SOURCES src/os/alsa_output.cc)
        list(APPEND MATRIX_LINUX_AUDIO_LIBS ae_alsa)
    endif()
    if(TARGET ae_jack)
        list(APPEND MATRIX_LINUX_AUDIO_SOURCES src/os/jack_output.cc)
        list(APPEND MATRIX_LINUX_AUDIO_LIBS ae_jack)
    endif()

    add_executable(matrix_player
        ${MATRIX_PLAYER_PORTABLE_SOURCES}
        ${MATRIX_LINUX_AUDIO_SOURCES}
        src/os/linux_host.cc
        ${CMAKE_SOURCE_DIR}/framework/vk_canvas/platform/linux/wayland_platform.cc)

    if(TARGET ae_alsa)
        target_compile_definitions(matrix_player PRIVATE MATRIX_HAVE_ALSA)
    endif()
    if(TARGET ae_jack)
        target_compile_definitions(matrix_player PRIVATE MATRIX_HAVE_JACK)
    endif()

    target_include_directories(matrix_player PRIVATE
        src
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_SOURCE_DIR}/third_party/soxr/src
        ${CMAKE_BINARY_DIR}/generated
        ${CMAKE_SOURCE_DIR}/framework/vk_canvas/platform/linux)

    target_compile_options(matrix_player PRIVATE -Wall)
    # LTO is on for Release (see root CMakeLists.txt): GCC's whole-program
    # analysis for these two warning classes runs at the final LINK step, not
    # per-TU compile — CMake does not forward target_compile_options to the
    # link line, so these have to be target_link_options, not
    # target_compile_options (confirmed empirically: adding them as compile
    # options here, and on the sqlite3/freetype targets themselves, left the
    # link-time warnings unaffected). They fire on vendored C we don't own
    # (sqlite3.c's amalgamation, vk_canvas's vendored FreeType) and are
    # well-known GCC LTO false positives on well-tested code (GCC's own
    # diagnostic hedges with "likely" / "note: source object is likely at
    # address zero"), not bugs to fix by editing that source.
    target_link_options(matrix_player PRIVATE -Wno-stringop-overflow -Wno-stringop-overread)

    add_dependencies(matrix_player compile_shared_shaders compile_vk_canvas_shaders generate_ui_min_text_size)

    find_package(Threads REQUIRED)
    target_link_libraries(matrix_player PRIVATE
        matrix_core
        ae_usb
        ${MATRIX_LINUX_AUDIO_LIBS}
        sqlite3
        soxr
        turbojpeg-static
        vk_canvas_core
        vk_canvas_wayland
        Threads::Threads)
endif()

# Applied to matrix_player ONLY. icon_preview and matrix_ui_capture are
# Debug-only dev tools that must keep writing beside their own binary —
# matrix_ui_capture's determinism depends on reading the matrix_player.db
# sitting next to it, and both deliberately share the player's atlas cache
# in a build tree.
if(MATRIX_STATE_HOME)
    target_compile_definitions(matrix_player PRIVATE
        MATRIX_STATE_HOME="${MATRIX_STATE_HOME}")
endif()

# Copy EQ profiles JSON next to the executable
add_custom_command(TARGET matrix_player POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/eq_profiles.json
        $<TARGET_FILE_DIR:matrix_player>/eq_profiles.json)

# libc++.dll next to the executable (Clang/MinGW builds only). The rest of
# the MinGW/UCRT runtime (libgcc, libstdc++, winpthread) links into
# matrix_player.exe's OWN object code fully static — see the
# -static/-static-libgcc/-static-libstdc++ block in the root CMakeLists.txt —
# but libc++ itself doesn't: a vendored libFLAC CMake rule emits
# `-Wl,-Bstatic ... -Wl,-Bdynamic -lm` and never resets the linker's mode
# back, so everything resolved afterward (including Clang's own auto-appended
# C++ runtime) falls back to dynamic. Forcing that back to static means
# precisely controlling where in the flattened link line a static libc++.a
# lands relative to every transitive dependency CMake inserts on its own —
# fragile enough, for a single DLL, that shipping the DLL itself is the
# better trade. libFLAC is vendored and stays pristine (see
# framework/audio_engine/CLAUDE.md); this works around it here instead.
#
# libc++.dll is itself a precompiled MSYS2 binary with its OWN dynamic
# dependencies, regardless of how matrix_player.exe's own code links: it
# needs libgcc_s_seh-1.dll, which in turn needs libwinpthread-1.dll (verified
# via objdump -p on each, chain terminates there — both only need KERNEL32
# and the OS-builtin api-ms-win-crt-* apiset after that). Confirmed by an
# actual STATUS_DLL_NOT_FOUND launch failure with only libc++.dll copied —
# don't drop these two on the assumption libc++.dll alone is enough.
if(WIN32 AND NOT MSVC)
    get_filename_component(_matrix_mingw_bin_dir "${_matrix_libcxx_dll}" DIRECTORY)
    add_custom_command(TARGET matrix_player POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_matrix_libcxx_dll}"
            $<TARGET_FILE_DIR:matrix_player>/libc++.dll
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_matrix_mingw_bin_dir}/libgcc_s_seh-1.dll"
            $<TARGET_FILE_DIR:matrix_player>/libgcc_s_seh-1.dll
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_matrix_mingw_bin_dir}/libwinpthread-1.dll"
            $<TARGET_FILE_DIR:matrix_player>/libwinpthread-1.dll)
endif()

# Compiled shaders land at ${CMAKE_BINARY_DIR}/assets/shaders (root
# CMakeLists.txt's vce_compile_slang calls) — on Windows that's already the
# exe's own directory (single-target build), but here the exe lives one
# level down in gui/, so FileAssetReader (which reads <exe dir>/assets/...)
# wouldn't find them without this copy.
#
# Its own target, not a POST_BUILD on matrix_player. A POST_BUILD only runs
# when the executable RELINKS, so editing a .slang and rebuilding recompiled
# the SPIR-V and then left the stale copy sitting next to the exe — the shader
# change simply did not reach the running program, and nothing said so. That
# cost a verification pass that reported "no pixels changed" about a build
# whose shaders had not been updated at all. Every consumer of the copy now
# depends on the copy, and the copy depends on the compile.
add_custom_target(copy_shader_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_BINARY_DIR}/assets
        $<TARGET_FILE_DIR:matrix_player>/assets
    DEPENDS compile_shared_shaders compile_vk_canvas_shaders
    COMMENT "Copying compiled shaders next to the executables")
add_dependencies(copy_shader_assets matrix_player)

# Copy bundled fonts (Latin Modern + multi-script fallback faces) next to the executable
add_custom_command(TARGET matrix_player POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets/fonts
        $<TARGET_FILE_DIR:matrix_player>/fonts)

# ---------------------------------------------------------------------------
# ui_metrics_test — the app's only automated test. Pure arithmetic (no Vulkan,
# no window, no linking against the engine), so it runs anywhere. Debug-only,
# matching how this repo gates its other dev tooling (MATRIX_BUILD_AB_TEST, see
# the root CMakeLists.txt). Asserts survive via #undef NDEBUG in the test source
# itself. Convention matches framework/vk_canvas/core/tests/*.cc.
#
# layout.hh lives in framework/vk_canvas/core/ and is header-only for what this
# test touches (UiScale::factor is inline; canvas.hh/texture.hh pull in no
# Vulkan), so the include directory alone is enough — no vk_canvas_core link.
# ---------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_executable(ui_metrics_test src/ui_metrics_test.cc src/ui_metrics.cc)
    target_include_directories(ui_metrics_test PRIVATE
        src
        ${CMAKE_BINARY_DIR}/generated
        ${CMAKE_SOURCE_DIR}/framework/vk_canvas/core)
    add_dependencies(ui_metrics_test generate_ui_min_text_size)
    if(NOT WIN32)
        target_compile_options(ui_metrics_test PRIVATE -Wall)
    endif()

    # ui_orientation_test links the REAL src/ui_orientation.cc rather than
    # restating its arithmetic. ui_orientation.hh includes NOTHING at all — no
    # Canvas, no Host, no theme — which is what keeps this a pure-logic test and
    # what lets the Android build consume the same file untouched.
    add_executable(ui_orientation_test src/ui_orientation_test.cc src/ui_orientation.cc)
    target_include_directories(ui_orientation_test PRIVATE src)
    if(NOT WIN32)
        target_compile_options(ui_orientation_test PRIVATE -Wall)
    endif()

    # rail_layout_test links the REAL src/rail_layout.cc. Bar A's geometry is
    # the part of the orientation work most likely to break in silence -- a
    # wrong anchor in one of the eight states (2 orientations x bit-perfect x
    # search open) does not crash, it just looks wrong somewhere nobody looked
    # -- so it is arithmetic over a rectangle, tested without a Canvas.
    add_executable(rail_layout_test src/rail_layout_test.cc src/rail_layout.cc
                                    src/ui_orientation.cc)
    target_include_directories(rail_layout_test PRIVATE src)
    if(NOT WIN32)
        target_compile_options(rail_layout_test PRIVATE -Wall)
    endif()

    # ui_icons_test links the REAL src/ui_icons.cc rather than restating its
    # arithmetic, which is why the icon placement math is split away from the
    # Canvas-dependent half (src/ui_icons_draw.cc). Needs no include dirs
    # beyond src/: ui_icons.hh forward-declares Canvas/Color instead of
    # including canvas.hh, precisely so this stays a pure-logic test.
    add_executable(ui_icons_test src/ui_icons_test.cc src/ui_icons.cc)
    target_include_directories(ui_icons_test PRIVATE src)
    if(NOT WIN32)
        target_compile_options(ui_icons_test PRIVATE -Wall)
    endif()

    # ui_text_test — ordinal suffixes for ranked playlist rows. One include and
    # no link line, because src/ui_text.hh is header-only and pulls in nothing
    # but <string>. The rule worth guarding is the teens (11th, not 11st) and
    # its repeat every hundred (111th, not 111st): wrong, it reads as a typo
    # rather than as a bug, which is exactly how such things survive.
    add_executable(ui_text_test src/ui_text_test.cc)
    target_include_directories(ui_text_test PRIVATE src)
    if(NOT WIN32)
        target_compile_options(ui_text_test PRIVATE -Wall)
    endif()

    # icon_preview — standalone window showing ONLY the icons, at a ladder of
    # sizes plus the sizes the app really draws them at. An MTSDF bake is only
    # sharp within a range around its baked density, and judging that needs the
    # icons side by side at real sizes — not buried in the running player.
    # Linux-only: it drives WaylandDisplay/WaylandWindow directly rather than
    # going through Host (which is bound to PlayerWindow).
    if(NOT WIN32)
        add_executable(icon_preview
            ${CMAKE_SOURCE_DIR}/tools/icon_preview/main.cc
            src/app_paths.cc
            src/ui_icons.cc
            src/ui_icons_draw.cc
            src/ui_metrics.cc)
        target_include_directories(icon_preview PRIVATE
            src
            ${CMAKE_BINARY_DIR}/generated
            ${CMAKE_SOURCE_DIR}/framework/vk_canvas/platform/linux)
        target_compile_options(icon_preview PRIVATE -Wall)
        add_dependencies(icon_preview
            compile_shared_shaders compile_vk_canvas_shaders generate_ui_min_text_size)
        target_link_libraries(icon_preview PRIVATE
            vk_canvas_core
            vk_canvas_wayland)
    endif()

    # matrix_ui_capture — headless PNG screenshots of the REAL UI (no window,
    # no compositor, no DAC): VK_EXT_headless_surface via vk_canvas's
    # core/headless.hh, plus a Host implementation that answers "there is no
    # window" to everything else. See tools/ui_capture/main.cc.
    #
    # It compiles the app's own sources a second time with MATRIX_UI_CAPTURE
    # defined (which is what exposes PlayerWindow::captureGoTo/captureFrame) —
    # NOT a link against matrix_player, which is an executable owning main().
    # The two swaps versus that source list: gui_main.cc and os/linux_host.cc
    # are out (both define an entry point; the tool has its own), and the
    # tool's main.cc is in.
    #
    # Linux-only for now, same reason icon_preview is: it needs the Wayland
    # backend's FileAssetReader. A Windows equivalent would swap that for
    # win32_platform.cc's reader and drop nothing else.
    if(NOT WIN32)
        add_executable(matrix_ui_capture
            ${CMAKE_SOURCE_DIR}/tools/ui_capture/main.cc
            src/app_paths.cc
            src/player_view.cc
            src/art_view.cc
            src/ui_metrics.cc
            src/ui_orientation.cc
            src/rail_layout.cc
            src/ui_icons.cc
            src/ui_icons_draw.cc
            src/panels/settings_panels.cc
            ${MATRIX_LINUX_AUDIO_SOURCES}
            ${CMAKE_SOURCE_DIR}/framework/vk_canvas/platform/linux/wayland_platform.cc)

        target_compile_definitions(matrix_ui_capture PRIVATE MATRIX_UI_CAPTURE)
        if(TARGET ae_alsa)
            target_compile_definitions(matrix_ui_capture PRIVATE MATRIX_HAVE_ALSA)
        endif()
        if(TARGET ae_jack)
            target_compile_definitions(matrix_ui_capture PRIVATE MATRIX_HAVE_JACK)
        endif()

        target_include_directories(matrix_ui_capture PRIVATE
            src
            ${CMAKE_SOURCE_DIR}/third_party
            ${CMAKE_SOURCE_DIR}/third_party/soxr/src
            ${CMAKE_BINARY_DIR}/generated
            ${CMAKE_SOURCE_DIR}/framework/vk_canvas/platform/linux
            # stb_image_write.h (public domain, vendored by vk_canvas's own
            # capture runner) — header-only, so no link against that target.
            ${CMAKE_SOURCE_DIR}/framework/vk_canvas/core/capture)

        target_compile_options(matrix_ui_capture PRIVATE -Wall)
        add_dependencies(matrix_ui_capture
            compile_shared_shaders compile_vk_canvas_shaders generate_ui_min_text_size)

        find_package(Threads REQUIRED)
        target_link_libraries(matrix_ui_capture PRIVATE
            matrix_core
            ae_usb
            ${MATRIX_LINUX_AUDIO_LIBS}
            sqlite3
            soxr
            turbojpeg-static
            vk_canvas_core
            vk_canvas_wayland
            Threads::Threads)
    endif()
endif()
