# ---------------------------------------------------------------------------
# matrix_core — portable static lib: library scan/DB/EQ/decode. Zero OS
# headers except the guarded #ifdef _WIN32 MMCSS block in decoder.cpp and the
# per-platform FolderWatcher backend under src/os/ (windows_folder_watch.cpp
# on Windows, linux_folder_watch.cpp elsewhere — see core/include/core/library.h).
# ---------------------------------------------------------------------------

add_library(matrix_core STATIC
    src/library.cpp
    src/db.cpp
    src/eq_manager.cpp
    src/eq_profiles.cpp
    src/decoder.cpp
    $<IF:$<PLATFORM_ID:Windows>,src/os/windows_folder_watch.cpp,src/os/linux_folder_watch.cpp>
)

target_include_directories(matrix_core PUBLIC
    include
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/soxr/src)

target_link_libraries(matrix_core PUBLIC ae_core sqlite3)

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
