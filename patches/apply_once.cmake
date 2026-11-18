# Applies a patch with git apply unless it is already applied, so a
# FetchContent PATCH_COMMAND survives a re-configure of an already patched
# checkout. Run in the checkout: cmake -DPATCH=<file> -P apply_once.cmake
execute_process(COMMAND git apply --ignore-whitespace --reverse --check "${PATCH}"
    RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
if(already EQUAL 0)
    message(STATUS "already applied: ${PATCH}")
    return()
endif()
execute_process(COMMAND git apply --ignore-whitespace "${PATCH}" RESULT_VARIABLE failed)
if(NOT failed EQUAL 0)
    message(FATAL_ERROR "could not apply ${PATCH}")
endif()
