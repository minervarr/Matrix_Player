# chirlu/soxr stays pinned. The CMake 3.22 and Clang fixes are ours, in
# patches/soxr-modern-cmake.patch. Apply them once per working tree.
function(matrix_apply_soxr_patch repo_root)
    find_package(Git QUIET)
    if(NOT GIT_EXECUTABLE)
        message(FATAL_ERROR "git is required to apply patches/soxr-modern-cmake.patch")
    endif()
    set(patch "${repo_root}/patches/soxr-modern-cmake.patch")
    set(soxr "${repo_root}/third_party/soxr")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${soxr}" apply --reverse --check "${patch}"
        RESULT_VARIABLE already
        OUTPUT_QUIET ERROR_QUIET)
    if(already EQUAL 0)
        return()
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${soxr}" apply "${patch}"
        RESULT_VARIABLE applied
        ERROR_VARIABLE err)
    if(NOT applied EQUAL 0)
        message(FATAL_ERROR "soxr patch failed:\n${err}")
    endif()
endfunction()
