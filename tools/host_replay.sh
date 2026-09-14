#!/usr/bin/env bash
# Build tools/host_replay.c against the portable protocol core and the UI model
# and run it on a capture. No ESP-IDF, no hardware - the same toolchain the host
# tests use.
#
#   tools/host_replay.sh                          # bundled sample capture
#   tools/host_replay.sh my_capture.txt --wifi-at 60000 --press 200000
set -eu
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
OUT="${TMPDIR:-/tmp}/hr_host_replay"
$CC -std=c11 -Wall -Wextra -Werror -g \
    -Icomponents/hr_protocol/include -Icomponents/hr_ui/include \
    components/hr_protocol/*.c components/hr_ui/*.c tools/host_replay.c \
    -o "$OUT"
case "${1:-}" in
    ""|--*)
        # No capture named: walk the bundled sample through Wi-Fi coming up,
        # a full batch, a link drop, two short presses (INFO, RAW) and a
        # long press pair (night mode on/off). Extra flags are appended.
        set -- test/fixtures/capture-v2-sample.txt --wifi-at 75630 \
            --press 200000 --press 203000 --long-press 250000 \
            --long-press 252000 "$@"
        ;;
esac
exec "$OUT" "$@"
