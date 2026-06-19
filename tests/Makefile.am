# Functional tests. Driven by automake's test harness: each TESTS entry is run,
# exit 0 = pass, 77 = skip (no Kodi / missing tool), anything else = fail.
# Most talk to a live Kodi, so they self-skip when one is not reachable;
# history.test reads only the local log and needs no Kodi.
#
# Every .test sources lib.sh for the shared harness (Kodi/MCP plumbing, the jq
# helpers, and the ok/no/eq assertion framework). lib.sh is not itself a test —
# it ships via EXTRA_DIST so out-of-tree `make distcheck` runs find it.

TESTS = searchmedia.test contributors.test playfile.test queue.test \
        getplaylist.test dropplaylists.test notify.test rpc.test \
        history.test monitor.test audio_and_subtitle_info.test \
        set_audio_and_subtitle.test playfile_with_audio_and_subtitle.test \
        mute.test volume.test instances.test nowplaying.test stop.test \
        play_pause.test skip.test stringified_args.test

EXTRA_DIST = $(TESTS) lib.sh
