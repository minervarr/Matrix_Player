# ---------------------------------------------------------------------------
# matrix_core — portable static lib: library scan/DB/EQ/decode. Zero OS
# headers except the guarded #ifdef _WIN32 MMCSS block in decoder.cpp and the
# per-platform FolderWatcher backend under src/os/ (windows_folder_watch.cpp
# on Windows, linux_folder_watch.cpp elsewhere — see core/include/core/library.h).
# ---------------------------------------------------------------------------

add_library(matrix_core STATIC
    src/library.cpp
    src/variants.cpp
    src/facets.cpp
    src/db.cpp
    src/db_stats.cpp
    src/stats.cpp
    src/streamer_db.cpp
    src/eq_manager.cpp
    src/eq_profiles.cpp
    src/decoder.cpp
    $<IF:$<PLATFORM_ID:Windows>,src/os/windows_folder_watch.cpp,src/os/linux_folder_watch.cpp>
)

target_include_directories(matrix_core PUBLIC
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/soxr/src)

# arc_fs is PUBLIC because core/include/core/streamer_db.h's search-path helper
# is arc::fs::ancestorsUpTo() — the header names it, so a consumer needs it too.
target_link_libraries(matrix_core PUBLIC ae_core sqlite3 arc_fs)

# Decode goes through audio_engine's backends (see src/decoder.cpp). Each is
# optional at the engine's end — libFLAC is a submodule and libmpg123 is a
# download — so link and advertise only what this build actually produced,
# and let decoder.cpp compile the matching branches out.
foreach(_codec flac mp3 dsd)
    string(TOUPPER ${_codec} _CODEC)
    if(TARGET ae_${_codec})
        target_link_libraries(matrix_core PRIVATE ae_${_codec})
        target_compile_definitions(matrix_core PRIVATE MATRIX_HAVE_${_CODEC}=1)
    else()
        message(WARNING "ae_${_codec} not built — matrix_player will not decode ${_CODEC}.")
    endif()
endforeach()

if(MSVC)
    target_compile_options(matrix_core PRIVATE /W3)
else()
    target_compile_options(matrix_core PRIVATE -Wall)
endif()

if(WIN32)
    target_compile_definitions(matrix_core PRIVATE NOMINMAX UNICODE _UNICODE)
    target_link_libraries(matrix_core PRIVATE avrt)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(matrix_core PRIVATE Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# Assert-based pure-logic test, same convention as gui/'s ui_metrics_test and
# ui_icons_test (plain assert(), #undef NDEBUG in the source, no framework).
# Debug-only, run directly:  ./build/linux_debug/core/variants_test
#
# It links the REAL src/variants.cpp and nothing else — no matrix_core, no
# ae_core, no sqlite3. That is only possible because variants.cpp touches
# nothing outside <string>/<vector>, which is the property to protect: if this
# target ever needs another source to link, the grouping logic has picked up a
# dependency it should not have.
# ---------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_executable(variants_test tests/variants_test.cc src/variants.cpp)
    target_include_directories(variants_test PRIVATE include)
    if(MSVC)
        target_compile_options(variants_test PRIVATE /W3 /utf-8)
    else()
        target_compile_options(variants_test PRIVATE -Wall)
    endif()

    # ── stats_test ─────────────────────────────────────────────────────────
    # The listening log and its aggregate queries, over an in-memory database.
    # Links the REAL db.cpp / db_stats.cpp / variants.cpp plus sqlite3 — and
    # deliberately NOT matrix_core, ae_core or library.cpp. That restriction is
    # the property worth protecting: the data layer must not need a decoder, an
    # audio engine or a filesystem walk to answer a question about history. If
    # this target ever needs another source to link, something leaked into it.
    add_executable(stats_test
        tests/stats_test.cc
        src/db.cpp
        src/db_stats.cpp
        src/stats.cpp
        src/variants.cpp)
    target_include_directories(stats_test PRIVATE include src ${CMAKE_SOURCE_DIR}/third_party)
    target_link_libraries(stats_test PRIVATE sqlite3)
    if(MSVC)
        target_compile_options(stats_test PRIVATE /W3 /utf-8)
    else()
        target_compile_options(stats_test PRIVATE -Wall)
    endif()

    # ── facets_test ────────────────────────────────────────────────────────
    # The guided search engine (chips, suggestions, counts, the two kinds of
    # "zero results"). Links the REAL src/facets.cpp and NOTHING else — not
    # even variants.cpp: facets.cpp reads Album/Track as plain data and calls
    # no other translation unit, which is what keeps the search usable from
    # the Android build unchanged. If this target ever needs another source,
    # the search picked up a dependency it should not have.
    add_executable(facets_test tests/facets_test.cc src/facets.cpp)
    target_include_directories(facets_test PRIVATE include)
    if(MSVC)
        target_compile_options(facets_test PRIVATE /W3 /utf-8)
    else()
        target_compile_options(facets_test PRIVATE -Wall)
    endif()

    # ── streamer_db_test ───────────────────────────────────────────────────
    # WHERE the external downloader's library.db is looked for, relative to a
    # music root. Only the pure half of streamer_db.cpp is exercised —
    # streamerSearchPath() touches no filesystem — so the three real layouts
    # (root above the library, root == the library, root below it) can be
    # asserted with no fixture and no downloader installed. sqlite3 is linked
    # because the translation unit includes it for the reader alongside; the
    # test itself opens nothing.
    add_executable(streamer_db_test tests/streamer_db_test.cc src/streamer_db.cpp)
    target_include_directories(streamer_db_test PRIVATE include ${CMAKE_SOURCE_DIR}/third_party)
    # arc_fs supplies the ancestor walk streamerSearchPath() now delegates to.
    # It links nothing but the standard library, so this stays the narrow,
    # fixture-free target it was.
    target_link_libraries(streamer_db_test PRIVATE sqlite3 arc_fs)
    if(MSVC)
        target_compile_options(streamer_db_test PRIVATE /W3 /utf-8)
    else()
        target_compile_options(streamer_db_test PRIVATE -Wall)
    endif()

    # ── scan_source_test ───────────────────────────────────────────────────
    # The two candidate sources for a scan — the platform's media catalogue and
    # the directory walk — must produce the same library, because the walk is
    # the fallback the index drops to whenever it has nothing useful to say.
    #
    # This is the check that was going to need a phone. arc::fs::MediaIndex
    # takes its backend as an INSTALLED pointer rather than a compile-time
    # #ifdef, so a fake one runs here and the Android path is exercised on the
    # desktop. It links matrix_core because it drives the real scan over real
    # WAV files — the sample rate and bit depth it asserts are exactly what no
    # media index reports, so they can only come from the files themselves.
    add_executable(scan_source_test tests/scan_source_test.cc)
    target_link_libraries(scan_source_test PRIVATE matrix_core arc_fs)
    target_include_directories(scan_source_test PRIVATE include ${CMAKE_SOURCE_DIR}/third_party)
    if(MSVC)
        target_compile_options(scan_source_test PRIVATE /W3 /utf-8)
    else()
        target_compile_options(scan_source_test PRIVATE -Wall)
    endif()

    # ── matrix_fixture_gen ────────────────────────────────────────────────
    # Writes a small, fixed synthetic music library (real sine-tone WAVs, a
    # real folder layout) to disk so the GUI's Album/EP/Single/Compilation/
    # Live/Remix views and the variant-grouping rules can be exercised
    # without a real music collection — see
    # docs/superpowers/specs/2026-08-08-fixture-gen-design.md. Links
    # variants.cpp alone (same trick as variants_test above) to self-verify
    # every album it writes against the REAL classifyReleaseType(), so it
    # can never silently drift out of sync with the classifier it exists to
    # exercise. Both platforms — no OS-specific code at all, unlike
    # icon_preview/matrix_ui_capture's Wayland dependency.
    add_executable(matrix_fixture_gen ${CMAKE_SOURCE_DIR}/tools/fixture_gen.cpp src/variants.cpp)
    target_include_directories(matrix_fixture_gen PRIVATE include ${CMAKE_SOURCE_DIR}/third_party)
    if(MSVC)
        target_compile_options(matrix_fixture_gen PRIVATE /W3 /utf-8)
    else()
        target_compile_options(matrix_fixture_gen PRIVATE -Wall)
    endif()
endif()
