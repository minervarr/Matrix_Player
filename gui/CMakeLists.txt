# ---------------------------------------------------------------------------
# matrix_player executable. Windows-only until Phase 3's host abstraction
# lands — player_view.hh/.cc, art_view.hh/.cc, gui_main.cc, wasapi_output.hh/.cc
# are still Win32-typed (HWND/HMONITOR/RECT/win32_platform.hh throughout).
# A Linux gui target is added once that abstraction lands; until then the
# Linux build only produces matrix_core + the engine/canvas libraries.
# ---------------------------------------------------------------------------

if(WIN32)
    add_executable(matrix_player WIN32
        src/gui_main.cc
        src/player_view.cc
        src/wasapi_output.cc
        src/art_view.cc
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

    # Copy EQ profiles JSON next to the executable
    add_custom_command(TARGET matrix_player POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/eq_profiles.json
            $<TARGET_FILE_DIR:matrix_player>/eq_profiles.json)

    # Copy bundled fonts (Latin Modern + multi-script fallback faces) next to the executable
    add_custom_command(TARGET matrix_player POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets/fonts
            $<TARGET_FILE_DIR:matrix_player>/fonts)

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
        comctl32
        comdlg32
        shell32
        ole32
        mmdevapi
        avrt
        dbghelp)
else()
    message(STATUS "matrix_player GUI executable not yet built on this platform "
                    "(Phase 3 host abstraction pending) — only matrix_core and the "
                    "engine/canvas libraries build here for now.")
endif()
