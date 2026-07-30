# ---------------------------------------------------------------------------
# matrix_player executable. Shared portable sources (player_view.cc,
# art_view.cc, gui_main.cc, ...) plus exactly one platform Host backend
# (os/windows_host.cc or os/linux_host.cc — see src/host.hh).
# ---------------------------------------------------------------------------

set(MATRIX_PLAYER_PORTABLE_SOURCES
    src/gui_main.cc
    src/player_view.cc
    src/art_view.cc
    src/ui_metrics.cc
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
    target_compile_options(matrix_player PRIVATE /W3)

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
        dbghelp)
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

# Copy EQ profiles JSON next to the executable
add_custom_command(TARGET matrix_player POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/eq_profiles.json
        $<TARGET_FILE_DIR:matrix_player>/eq_profiles.json)

# Compiled shaders land at ${CMAKE_BINARY_DIR}/assets/shaders (root
# CMakeLists.txt's vce_compile_slang calls) — on Windows that's already the
# exe's own directory (single-target build), but here the exe lives one
# level down in gui/, so FileAssetReader (which reads <exe dir>/assets/...)
# wouldn't find them without this copy.
add_custom_command(TARGET matrix_player POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_BINARY_DIR}/assets
        $<TARGET_FILE_DIR:matrix_player>/assets)

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

    # icon_preview — standalone window showing ONLY the icons, at a ladder of
    # sizes plus the sizes the app really draws them at. An MTSDF bake is only
    # sharp within a range around its baked density, and judging that needs the
    # icons side by side at real sizes — not buried in the running player.
    # Linux-only: it drives WaylandDisplay/WaylandWindow directly rather than
    # going through Host (which is bound to PlayerWindow).
    if(NOT WIN32)
        add_executable(icon_preview
            ${CMAKE_SOURCE_DIR}/tools/icon_preview/main.cc
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
endif()
