# ── app_shell_generate_min_text_size(<font files...>) ────────────────────────
#
# Emits ${CMAKE_BINARY_DIR}/generated/ui_min_text_size.gen.h, which
# ui_metrics.hh includes, and defines the target `generate_ui_min_text_size`
# that any consumer of app_shell's UiMetrics must add_dependencies() on.
#
# The value it computes is the smallest device-pixel size at which the given
# faces can still be rendered legibly — derived from their own outline geometry
# by vulkan_font_engine's min_text_size tool. That makes it a per-APPLICATION
# number, not a library constant: it depends on the fonts the app ships, and the
# emitted floor is the WORST case across them. List exactly the faces the app
# actually loads; one left out here could render below its own floor.
#
# Built through ExternalProject_Add rather than add_subdirectory because the
# tool independently builds FreeType and msdfgen, which collide by target name
# with the copies vk_canvas_core already builds for its text engine.
#
# NOT usable when cross-compiling: the tool has to RUN on the build machine.
# Android copies the header out of a desktop build tree instead — see
# android/CMakeLists.txt in any consumer that has an Android target.

include(ExternalProject)

function(app_shell_generate_min_text_size)
    set(_fonts ${ARGN})
    if(NOT _fonts)
        message(FATAL_ERROR "app_shell_generate_min_text_size() needs at least one font file")
    endif()

    set(_install_dir ${CMAKE_BINARY_DIR}/min-text-size-install)
    if(WIN32)
        set(_bin min_text_size.exe)
    else()
        set(_bin min_text_size)
    endif()

    ExternalProject_Add(min_text_size_ext
        SOURCE_DIR ${APP_SHELL_DIR}/../vk_canvas/first_party/vulkan_font_engine/tools/min_text_size
        CMAKE_ARGS
            -G${CMAKE_GENERATOR}
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
            -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
            -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
        INSTALL_COMMAND ${CMAKE_COMMAND} -E copy
            <BINARY_DIR>/${_bin} ${_install_dir}/${_bin}
        BUILD_BYPRODUCTS ${_install_dir}/${_bin})

    set(_header ${CMAKE_BINARY_DIR}/generated/ui_min_text_size.gen.h)
    add_custom_command(
        OUTPUT ${_header}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
        COMMAND ${_install_dir}/${_bin} --emit-header ${_header} 0.5 512 ${_fonts}
        DEPENDS min_text_size_ext ${_fonts}
        COMMENT "Computing minimum readable UI text size from font geometry")
    add_custom_target(generate_ui_min_text_size DEPENDS ${_header})
endfunction()
