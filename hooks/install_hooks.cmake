execute_process(
        COMMAND clang-format --version
        RESULT_VARIABLE clang_format_exitcode
)
if (NOT clang_format_exitcode EQUAL 0)
    message(WARNING "clang-format is not in your PATH")
endif ()

set(GIT_HOOKS_DIR ${CMAKE_SOURCE_DIR}/.git/hooks)
execute_process(
        COMMAND /bin/sh -c "test -w \"${GIT_HOOKS_DIR}\""
        RESULT_VARIABLE git_hooks_writable
)

if (git_hooks_writable EQUAL 0)
    file(COPY ${CMAKE_SOURCE_DIR}/hooks/pre-commit DESTINATION ${GIT_HOOKS_DIR}/)
    message("git hooks installed!")
else ()
    message("skip installing git hooks because ${GIT_HOOKS_DIR} is not writable")
endif ()
