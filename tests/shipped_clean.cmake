# ctest shipped_clean: the program people get (phobicccp, desktop and pad)
# carries nothing that exists only for testing. Test-only code belongs in
# phobicccp-dev or a test target.
#   cmake -DBIN=<phobicccp> -DSOURCES=<a|b|c> -P shipped_clean.cmake

string(REPLACE "|" ";" _sources "${SOURCES}")
foreach(_s IN LISTS _sources)
    if(_s MATCHES "(^|/)tests/")
        message(FATAL_ERROR "test source linked into the shipped program: ${_s}")
    endif()
endforeach()

# Developer modes and screenshot hooks, by the exact strings the code matches
# (the built-in changelog mentions them in passing, which is fine).
file(STRINGS "${BIN}" _hits
     REGEX "^(--selftest|--shot|--grbl-[a-z0-9-]+|PHOBICCCP_SHOT_[A-Z]+|PHOBIC_SHOT_SIZE|PHOBIC_TOOL|PHOBIC_PAD_STYLE)$")
if(_hits)
    list(JOIN _hits "\n  " _list)
    message(FATAL_ERROR "test-only code in the shipped program ${BIN}:\n  ${_list}")
endif()
message(STATUS "shipped program is clean: ${BIN}")
