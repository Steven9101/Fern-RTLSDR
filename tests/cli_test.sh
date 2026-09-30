#!/bin/sh
# Fern-RTLSDR, an RTL-SDR input module for FernSDR.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runs the real program. Needs no RTL-SDR and expects none to be plugged in.
# Usage: cli_test.sh path/to/fern-rtlsdr

set -u
bin=${1:?usage: cli_test.sh path/to/fern-rtlsdr}
case $bin in /*) ;; *) bin=$(pwd)/$bin ;; esac
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
failures=0

pass() { echo "ok   $*" >&2; }
fail() { echo "FAIL $*" >&2; failures=$((failures + 1)); }
check() { # check <description> <command...>
    what=$1
    shift
    if "$@"; then pass "$what"; else fail "$what"; fi
}

# Runs the module the way FernSDR does: fixed environment, commands from
# stdin, samples, log and events into files.
module() {
    env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 FERNSDR_MODULE_API=1 \
        "$bin" --fernsdr-module 1 >"$tmp/samples" 2>"$tmp/log" 3>"$tmp/events"
}

# Checks the event lines: argument 1 is a Python expression over the list
# `events` of parsed lines. The expressions are the literals in this script;
# nothing the module writes is evaluated.
events_are() {
    python3 - "$tmp/events" "$1" <<'EOF'
import json, sys
lines = open(sys.argv[1], encoding="utf-8").read().split("\n")
assert lines[-1] == "", "the last event line has no newline"
events = [json.loads(line) for line in lines[:-1]]
hello = {"type": "hello", "api": 1, "id": "rtlsdr", "kind": "input"}
assert events and all(events[0].get(k) == v for k, v in hello.items()), "no hello first: %r" % events[:1]
if not eval(sys.argv[2]):
    sys.exit("unexpected events: %r" % events)
EOF
}

"$bin" --describe </dev/null >"$tmp/describe.json" 2>"$tmp/stderr"
check "--describe exits 0" test $? -eq 0
check "--describe prints valid JSON" python3 -m json.tool "$tmp/describe.json" >/dev/null
check "--describe prints one line" test "$(wc -l <"$tmp/describe.json")" -eq 1
check "--describe prints nothing on stderr" test ! -s "$tmp/stderr"
check "--describe names the module" python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
assert (d["api"], d["id"], d["kind"]) == (1, "rtlsdr", "input"), d
assert [s["key"] for s in d["settings"]] == ["device", "gain", "ppm", "rtl_agc", "bias_tee",
    "direct_sampling", "offset_tuning", "bandwidth", "buffers"]
' "$tmp/describe.json"

# The package holds only the executable, which must carry the licences of
# the libraries built into it.
"$bin" --notices </dev/null >"$tmp/notices" 2>/dev/null
check "--notices exits 0" test $? -eq 0
check "--notices carries the GPL, the LGPL and both author lists" sh -c '
    grep -q "GNU GENERAL PUBLIC LICENSE" "$1" && grep -q "GNU LESSER GENERAL PUBLIC LICENSE" "$1" &&
    grep -q "Version 2.1, February 1999" "$1" && grep -q "Steve Markgraf" "$1" &&
    grep -q "Johannes Erdfelt" "$1"' - "$tmp/notices"
"$bin" --help >"$tmp/help"
check "--help names --notices" grep -q -- "--notices" "$tmp/help"

out=$("$bin" --list-devices </dev/null)
status=$?
check "--list-devices exits 0" test "$status" -eq 0
check "--list-devices prints {\"devices\":[]} (got $out)" test "$out" = '{"devices":[]}'

for args in "" "--bogus" "describe" "--describe --list-devices" "--describe extra" "--fernsdr-module" \
    "--fernsdr-module 2" "--fernsdr-module one" "--fernsdr-module 1 extra"; do
    # shellcheck disable=SC2086
    "$bin" $args </dev/null >"$tmp/stdout" 2>/dev/null
    status=$?
    check "'$args' exits 2 (got $status)" test "$status" -eq 2
    check "'$args' prints nothing on stdout" test ! -s "$tmp/stdout"
done

"$bin" --fernsdr-module 1 </dev/null >/dev/null 2>"$tmp/stderr" 3>&-
status=$?
check "--fernsdr-module 1 without fd 3 exits 2 (got $status)" test "$status" -eq 2
check "and says why" grep -q "fd 3 is not open" "$tmp/stderr"

# No RTL-SDR here: hello, then a fatal no-device error, exit 3, no samples.
echo '{"type":"open","sample_rate":2400000,"center":100000000,"signal":"iq","settings":{"gain":"auto"}}' | module
status=$?
check "open without a device exits 3 (got $status)" test "$status" -eq 3
check "open without a device writes no samples" test ! -s "$tmp/samples"
check "open without a device reports no-device" events_are \
    'len(events) == 2 and events[1]["type"] == "error" and events[1]["code"] == "no-device" and events[1]["fatal"] is True'
check "log lines are at most 1 KiB" awk 'length($0) > 1023 { exit 1 }' "$tmp/log"

# Invalid settings are refused before USB is touched.
echo '{"type":"open","sample_rate":2400000,"center":100000000,"signal":"iq","settings":{"squelch":1}}' | module
status=$?
check "an unknown setting exits 6 (got $status)" test "$status" -eq 6
check "an unknown setting is reported as invalid" events_are \
    'events[1]["code"] == "invalid" and "module.squelch" in events[1]["message"]'

# End of file on fd 0 before open: only hello, exit 0.
module </dev/null
status=$?
check "EOF on fd 0 exits 0 (got $status)" test "$status" -eq 0
check "EOF on fd 0: only hello" events_are 'len(events) == 1'

# stop before open, then EOF.
printf '%s\n' '{"type":"future","x":1}' 'garbage' '{"type":"stop"}' | module
status=$?
check "stop exits 0 (got $status)" test "$status" -eq 0

# SIGTERM while waiting for open. env execs the program, so $! is its pid.
mkfifo "$tmp/commands"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 FERNSDR_MODULE_API=1 \
    "$bin" --fernsdr-module 1 <"$tmp/commands" >"$tmp/samples" 2>"$tmp/log" 3>"$tmp/events" &
pid=$!
exec 4>"$tmp/commands"
sleep 0.3
kill -TERM "$pid"
wait "$pid"
status=$?
exec 4>&-
check "SIGTERM exits 0 (got $status)" test "$status" -eq 0
check "SIGTERM: hello was sent" events_are 'len(events) == 1'

if [ "$failures" -ne 0 ]; then
    echo "cli_test: $failures failed" >&2
    exit 1
fi
echo "cli_test: all passed" >&2
