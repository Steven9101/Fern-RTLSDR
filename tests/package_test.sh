#!/bin/sh
# Fern-RTLSDR, an RTL-SDR input module for FernSDR.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds a .fernmod from an executable of this module, reads it back with
# the checker, and makes sure the checker notices damage.
# Usage: package_test.sh path/to/fern-rtlsdr

set -u
bin=${1:?usage: package_test.sh path/to/fern-rtlsdr}
here=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
failures=0

pass() { echo "ok   $*" >&2; }
fail() { echo "FAIL $*" >&2; failures=$((failures + 1)); }

version=$("$bin" --describe | python3 -c 'import json, sys; print(json.load(sys.stdin)["version"])')
package="$tmp/rtlsdr-$version-linux-x86_64.fernmod"

if python3 "$here/tools/mkfernmod.py" --executable "$bin" --describe-with "$bin" --platform linux-x86_64 \
    --version "$version" --output "$package" >/dev/null; then
    pass "mkfernmod builds $(basename "$package")"
else
    fail "mkfernmod"
fi

if python3 "$here/tools/check_fernmod.py" --extract "$tmp/extracted" --manifest "$tmp/manifest.json" \
    "$package" >/dev/null; then
    pass "check_fernmod accepts it"
else
    fail "check_fernmod rejects the package"
fi
if cmp -s "$bin" "$tmp/extracted"; then pass "the executable comes back unchanged"; else fail "extracted executable differs"; fi

if python3 - "$package" "$bin" "$tmp/manifest.json" <<'EOF'
import hashlib, json, subprocess, sys
package, executable, manifest_path = sys.argv[1:]
data = open(package, "rb").read()
first, second, rest = data.split(b"\n", 2)
assert first == b"FERNMOD1"
length = int(second)
manifest = json.loads(rest[:length])
exe = open(executable, "rb").read()
assert rest[length:] == exe
assert manifest["size"] == len(exe) == len(rest) - length
assert manifest["sha256"] == hashlib.sha256(exe).hexdigest()
assert list(manifest)[:12] == ["schema", "id", "name", "version", "kind", "api", "platform", "size",
                               "sha256", "license", "source", "description"], list(manifest)
assert manifest["license"] == "GPL-2.0-or-later"
describe = json.loads(subprocess.run([executable, "--describe"], stdout=subprocess.PIPE, check=True).stdout)
assert manifest["settings"] == describe["settings"]
assert manifest == json.load(open(manifest_path))
EOF
then
    pass "size, sha256 and settings match the executable and --describe"
else
    fail "manifest contents"
fi

# Damage the package in several ways; each must be refused.
corrupt() { # corrupt <name> <python statement on bytearray d>
    python3 -c "
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
$2
open(sys.argv[2], 'wb').write(d)
" "$package" "$tmp/$1/$(basename "$package")"
}
for case in flipped truncated extended badmagic badlength; do
    mkdir -p "$tmp/$case"
done
corrupt flipped 'd[-100] ^= 0x01'
corrupt truncated 'del d[-1]'
corrupt extended 'd += b"x"'
corrupt badmagic 'd[0:8] = b"FERNMOD2"'
corrupt badlength 'd[9:10] = b"9"'
for case in flipped truncated extended badmagic badlength; do
    if python3 "$here/tools/check_fernmod.py" "$tmp/$case/$(basename "$package")" >/dev/null 2>&1; then
        fail "check_fernmod accepts the $case copy"
    else
        pass "check_fernmod refuses the $case copy"
    fi
done
cp "$package" "$tmp/rtlsdr-$version-linux-aarch64.fernmod"
if python3 "$here/tools/check_fernmod.py" "$tmp/rtlsdr-$version-linux-aarch64.fernmod" >/dev/null 2>&1; then
    fail "check_fernmod accepts a package under another platform's name"
else
    pass "check_fernmod refuses a package under another platform's name"
fi
if python3 "$here/tools/mkfernmod.py" --executable "$bin" --describe-with "$bin" --platform linux-x86_64 \
    --version 9.9.9 --output "$tmp/rtlsdr-9.9.9-linux-x86_64.fernmod" >/dev/null 2>&1; then
    fail "mkfernmod accepts a version --describe does not report"
else
    pass "mkfernmod refuses a version --describe does not report"
fi
if python3 "$here/tools/mkfernmod.py" --executable "$bin" --describe-with "$bin" --platform linux-x86_64 \
    --version 00.1.0 --output "$tmp/rtlsdr-00.1.0-linux-x86_64.fernmod" >/dev/null 2>&1; then
    fail "mkfernmod accepts a version with a leading zero"
else
    pass "mkfernmod refuses a version with a leading zero"
fi

# Manifests FernSDR would refuse, each in an otherwise intact package.
# remanifest <case> <python statement on the manifest dict m>
remanifest() {
    mkdir -p "$tmp/$1"
    python3 - "$package" "$tmp/$1" "$2" <<'EOF2'
import json, os, sys
source, directory, change = sys.argv[1:]
data = open(source, "rb").read()
first, second, rest = data.split(b"\n", 2)
length = int(second)
m = json.loads(rest[:length])
exe = rest[length:]
exec(change)
text = json.dumps(m, separators=(",", ":")).encode()
name = "%s-%s-%s.fernmod" % (m["id"], m["version"], m["platform"])
with open(os.path.join(directory, name), "wb") as f:
    f.write(b"FERNMOD1\n%d\n" % len(text) + text + exe)
EOF2
}
remanifest uppercase_key 'm["settings"][1]["key"] = "Gain"'
remanifest reserved_key 'm["settings"][0]["key"] = "center"'
remanifest long_help 'm["settings"][0]["help"] = "x" * 401'
remanifest leading_zero 'm["version"] = "0.01.0"'
remanifest many_settings 'm["settings"] = [dict(m["settings"][3], key="k%d" % i) for i in range(65)]'
remanifest bad_default 'm["settings"][2]["default"] = 1000'
remanifest min_above_max 'm["settings"][2]["min"] = 600'
for case in uppercase_key reserved_key long_help leading_zero many_settings bad_default min_above_max; do
    if python3 "$here/tools/check_fernmod.py" "$tmp/$case"/*.fernmod >/dev/null 2>&1; then
        fail "check_fernmod accepts a manifest with $case"
    else
        pass "check_fernmod refuses a manifest with $case"
    fi
done
# Refused with a reason, not a traceback: Python's json reads a lone surrogate
# and NaN where FernSDR's parser does not.
remanifest lone_surrogate 'm["settings"][0]["help"] = "\ud800"'
remanifest nan_number 'm["settings"][2]["min"] = float("nan")'
for case in lone_surrogate nan_number; do
    out=$(python3 "$here/tools/check_fernmod.py" "$tmp/$case"/*.fernmod 2>&1)
    status=$?
    if [ "$status" -eq 1 ] && ! printf '%s' "$out" | grep -q Traceback; then
        pass "check_fernmod refuses a manifest with $case cleanly"
    else
        fail "check_fernmod does not refuse a manifest with $case cleanly (exit $status): $out"
    fi
done
remanifest unchanged 'pass'
if python3 "$here/tools/check_fernmod.py" "$tmp/unchanged"/*.fernmod >/dev/null 2>&1; then
    pass "check_fernmod accepts the rebuilt, unchanged manifest"
else
    fail "check_fernmod refuses the rebuilt, unchanged manifest"
fi

if [ "$failures" -ne 0 ]; then
    echo "package_test: $failures failed" >&2
    exit 1
fi
echo "package_test: all passed" >&2
