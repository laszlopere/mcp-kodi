# Functional tests. Driven by automake's test harness: each TESTS entry is run,
# exit 0 = pass, 77 = skip (no Kodi / missing tool), anything else = fail.
# Most talk to a live Kodi, so they self-skip when one is not reachable;
# history.test reads only the local log and needs no Kodi.

TESTS = search.test playfile.test queue.test getplaylist.test \
        dropplaylists.test rpc.test history.test

EXTRA_DIST = $(TESTS)
