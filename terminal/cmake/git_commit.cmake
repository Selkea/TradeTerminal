# Runs at build time (driven by the tt_build_info custom target): stamp the
# current git commit + dirty flag into build_info.h. configure_file leaves the
# output untouched when nothing changed, so this doesn't trigger a rebuild
# unless the commit actually moved. Inputs: IN, OUT, GIT_SRC.
execute_process(
    COMMAND git -C "${GIT_SRC}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE TT_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT TT_GIT_COMMIT)
    set(TT_GIT_COMMIT "unknown")
endif()

# --untracked-files=no: an untracked scratch file shouldn't flag the build dirty;
# any tracked modification does.
execute_process(
    COMMAND git -C "${GIT_SRC}" status --porcelain --untracked-files=no
    OUTPUT_VARIABLE _dirty
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(_dirty)
    set(TT_GIT_DIRTY "true")
else()
    set(TT_GIT_DIRTY "false")
endif()

string(TIMESTAMP TT_BUILD_DATE "%Y-%m-%dT%H:%M:%SZ" UTC)

# owner/repo of the origin remote, for the in-app GitHub update check. Handles
# both https (github.com/owner/repo(.git)) and ssh (git@github.com:owner/repo).
execute_process(
    COMMAND git -C "${GIT_SRC}" remote get-url origin
    OUTPUT_VARIABLE _url
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(_url MATCHES "github\\.com[:/]([^/]+/[^/.]+)")
    set(TT_REPO_SLUG "${CMAKE_MATCH_1}")
else()
    set(TT_REPO_SLUG "Selkea/TradeTerminal")   # fallback: the known repo
endif()

configure_file("${IN}" "${OUT}" @ONLY)
