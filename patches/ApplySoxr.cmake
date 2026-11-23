# chirlu/soxr stays pinned and pristine. The CMake 3.10 / policy, C99,
# FENV_ACCESS and calloc fixes are ours, in patches/soxr-modern-cmake.patch.
#
# The patch is applied to a private copy under the build tree, never to the
# submodule: the pinned checkout is a third-party artifact, and configuring
# must not leave a "modified content" entry behind in `git status`. The copy
# is refreshed only when the pinned commit or the patch itself changes.
function(matrix_prepare_soxr_source repo_root out_var)
    find_package(Git QUIET)
    if(NOT GIT_EXECUTABLE)
        message(FATAL_ERROR "git is required to apply patches/soxr-modern-cmake.patch")
    endif()

    set(src "${repo_root}/third_party/soxr")
    set(patch "${repo_root}/patches/soxr-modern-cmake.patch")
    set(dst "${CMAKE_BINARY_DIR}/soxr-src")

    # The pin is part of the stamp. A source tarball without .git still works;
    # it just cannot notice a source change that the patch hash does not.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${src}" rev-parse HEAD
        RESULT_VARIABLE head_rc
        OUTPUT_VARIABLE head
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT head_rc EQUAL 0)
        set(head "no-git")
    endif()
    file(SHA256 "${patch}" patch_hash)
    set(want "${head}:${patch_hash}")

    set(have "")
    if(EXISTS "${dst}/.matrix-soxr-stamp")
        file(READ "${dst}/.matrix-soxr-stamp" have)
    endif()

    if(NOT have STREQUAL want)
        file(REMOVE_RECURSE "${dst}")
        file(COPY "${src}/" DESTINATION "${dst}"
             PATTERN ".git" EXCLUDE
             PATTERN ".github" EXCLUDE)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply "${patch}"
            WORKING_DIRECTORY "${dst}"
            RESULT_VARIABLE applied
            ERROR_VARIABLE err)
        if(NOT applied EQUAL 0)
            file(REMOVE_RECURSE "${dst}")
            message(FATAL_ERROR "soxr patch failed:\n${err}")
        endif()
        file(WRITE "${dst}/.matrix-soxr-stamp" "${want}")
    endif()

    set(${out_var} "${dst}" PARENT_SCOPE)
endfunction()
