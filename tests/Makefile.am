# Unit tests: one C binary per module under test, linked against stubs for
# anything that would do file or network I/O (see unit/mk-test.h). They need no
# Kodi and always run. unit/run-unit-tests.sh builds the same set by hand,
# without configure; keep its source lists in step with these.
UNIT_TESTS = unit/test-stdio unit/test-config unit/test-config-io \
             unit/test-history unit/test-tools unit/test-tools-handlers

check_PROGRAMS = $(UNIT_TESTS)

# A GLib/json-glib CRITICAL means code ran into a precondition it should have
# checked itself; make it abort the test instead of scrolling past in the log.
AM_TESTS_ENVIRONMENT = G_DEBUG=fatal-criticals; export G_DEBUG;

AM_CFLAGS = $(MCP_KODI_CFLAGS) -Wall -Wextra \
            -I$(top_srcdir)/src -I$(srcdir)/unit
LDADD     = $(MCP_KODI_LIBS)

unit_test_stdio_SOURCES  = unit/test-stdio.c  ../src/mk-stdio.c
unit_test_config_SOURCES = unit/test-config.c ../src/mk-config.c
unit_test_config_io_SOURCES = unit/test-config-io.c ../src/mk-config.c
unit_test_history_SOURCES = unit/test-history.c ../src/mk-history.c
unit_test_tools_SOURCES = unit/test-tools.c ../src/mk-tools.c \
                          ../src/mk-config.c unit/stub-kodi.c \
                          unit/stub-history.c
unit_test_tools_handlers_SOURCES = unit/test-tools-handlers.c \
                                   ../src/mk-tools.c ../src/mk-config.c \
                                   unit/stub-kodi-prog.c unit/stub-history.c

# Functional tests. Driven by automake's test harness: each TESTS entry is run,
# exit 0 = pass, 77 = skip (no Kodi / missing tool), anything else = fail.
# Most talk to a live Kodi, so they self-skip when one is not reachable;
# history.test reads only the local log and needs no Kodi.
#
# Every .test sources lib.sh for the shared harness (Kodi/MCP plumbing, the jq
# helpers, and the ok/no/eq assertion framework). lib.sh is not itself a test —
# it ships via EXTRA_DIST so out-of-tree `make distcheck` runs find it.

FUNC_TESTS = searchmedia.test contributors.test playfile.test queue.test \
        getplaylist.test dropplaylists.test notify.test rpc.test \
        history.test monitor.test audio_and_subtitle_info.test \
        set_audio_and_subtitle.test playfile_with_audio_and_subtitle.test \
        mute.test volume.test instances.test nowplaying.test stop.test \
        play_pause.test skip.test stringified_args.test arg_validation.test \
        parse_error.test protocol.test kodi_errors.test

TESTS = $(UNIT_TESTS) $(FUNC_TESTS)

EXTRA_DIST = $(FUNC_TESTS) lib.sh unit/mk-test.h \
             unit/run-unit-tests.sh
