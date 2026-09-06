# The faces that actually SHIP — the one list, for every platform.
#
# assets/fonts is the shared `fonts` submodule (123 MiB) and the app opens
# ELEVEN files out of it. Both the
# desktop build and the Android build used to `copy_directory` the whole tree,
# so every per-ABI split APK carried 86 MiB of faces that no code path can
# reach — more, per APK, than everything else in it put together, the native
# .so included (which is ~5 MiB, already stripped).
#
# This is an allowlist, NOT a prune of the source tree, and that is deliberate:
# assets/fonts stays complete because it is the source of truth (and is now a
# submodule, so it is not ours to prune anyway), because
# app_shell_generate_min_text_size reads four of the faces at build time, and
# because vk_canvas's gpos_kern_test reads assets/fonts/newcomputermodern
# directly. Deleting from assets/ would break both and buy nothing extra — what
# costs is what gets COPIED, and this file is the only thing that copies.
#
# Included by the root CMakeLists.txt (desktop) and by android/CMakeLists.txt,
# which does not go through the root file — the same shape it already uses for
# framework/vk_canvas/cmake/VceShaders.cmake.
#
# Paths are relative to assets/fonts/ and MUST match what the code opens:
#   gui/src/ui_fonts.hh     — the UI family (NewCM regular/bold/italic/mono)
#                             and the icon face
#   gui/src/player_view.cc  — the CJK/Hangul fallbacks, via addFallback()
#   gui/src/art_view.cc     — the same six again, for the second window
#
# Adding a face at one of those call sites without adding it here ships an app
# that cannot draw the script it was added for. The failure is quiet: the atlas
# has no glyph, so the row renders BLANK rather than wrong, which is the hard
# kind to notice. If you add a face, add it here in the same change.
set(MATRIX_SHIPPED_FONTS
    newcomputermodern/NewCM10-Regular.otf
    newcomputermodern/NewCM10-Bold.otf
    newcomputermodern/NewCM10-Italic.otf
    newcomputermodern/NewCMMono10-Regular.otf
    fandol/FandolSong-Regular.otf
    fandol/FandolSong-Bold.otf
    haranoaji/HaranoAjiMincho-Regular.otf
    haranoaji/HaranoAjiMincho-Bold.otf
    unfonts-core/UnBatang.ttf
    unfonts-core/UnBatangBold.ttf)

# The icon face is NOT in the list above and NOT in assets/fonts. It looks like
# a typeface but it is artwork: per-app glyphs in the Private Use Area, built
# from tools/icon_font/icons/*.svg. Every repo's copy is named matrix-icons.otf
# and streamer's is a DIFFERENT file, so it cannot live in the shared fonts
# submodule without one app silently drawing another's icons. It sits in
# assets/icons/ instead, and is copied to the same runtime path as before —
# fonts/icons/matrix-icons.otf — so gui/src/ui_fonts.hh is unchanged.
set(MATRIX_ICON_FACE icons/matrix-icons.otf)

# Emit the copy commands for one target, into one directory. Both consumers go
# through this, so the desktop and the APK cannot end up carrying different
# sets. copy_if_different rather than copy: these are static inputs and the
# POST_BUILD runs on every relink.
function(matrix_copy_shipped_fonts target repo_root dest)
    foreach(f ${MATRIX_SHIPPED_FONTS})
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${repo_root}/assets/fonts/${f} ${dest}/${f})
    endforeach()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${repo_root}/assets/${MATRIX_ICON_FACE} ${dest}/${MATRIX_ICON_FACE})
endfunction()
