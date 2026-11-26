#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# AURORA for the FM-1: an installable package (aurora.fwsc, identity FM-1_944) built with GR!MM's build system.
#   tools/build_fwsc.sh [X.Y]          -> build/aurora.fwsc, or build/aurora-X.Y.fwsc with the version X.Y
#   GRIMM            a checkout of github.com/GRiMMBIENT/grimm (default: cloned into build/grimm at GRIMM_REF)
#   GRIMM_REF        the GR!MM commit to build on (the one AURORA was checked against)
#   JIELI_TOOLCHAIN  JieLi's Linux toolchain (default ~/.jieli/toolchain; GR!MM's tools/get_toolchain.sh fetches it)
#   AC79_SDK         JieLi AC79 SDK checkout (default ~/fw-AC79_AIoT_SDK, AC79NN_SDK_V1.2.1_2023-12-13)
# GR!MM's checks run as for GR!MM (no 64-bit division, no ROM calls, RAM and stack limits, registers only in hal/):
# if one fails, no package is written. The SDK's three files that go into the package must be the tested ones.
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
VER="$1"
GRIMM_REF="${GRIMM_REF:-b519acbee7dbdb3e0fb82b15b86a913898c6f8f5}"
export JIELI_TOOLCHAIN="${JIELI_TOOLCHAIN:-$HOME/.jieli/toolchain}"
export AC79_SDK="${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}"
mkdir -p build
python3 -c 'import PIL' 2>/dev/null || python3 -m pip install --quiet Pillow 2>/dev/null || python3 -m pip install --quiet --break-system-packages Pillow

if [ -z "$GRIMM" ]; then
    GRIMM="$ROOT/build/grimm"
    if [ ! -d "$GRIMM/.git" ]; then
        git init -q "$GRIMM"
        git -C "$GRIMM" remote add origin https://github.com/GRiMMBIENT/grimm
    fi
    git -C "$GRIMM" fetch -q --depth 1 origin "$GRIMM_REF"
    git -C "$GRIMM" checkout -q -f FETCH_HEAD
fi

python3 - "$AC79_SDK/cpu/wl82/tools" <<'PY'
import hashlib, os, sys
want = {"uboot.boot": "4e3b4c220dc96641cb5a723f41e68ce41d5261ae9434bb33fbd7f2c59976ded4",
        "cfg_tool.bin": "276579954f076886a6a7694f65dc71c034a63a2c204b76749065c0ac7b010d1b",
        "cfg/eq_cfg_hw.bin": "41167491bffed4651750719c973d2758adeb9021a5670d02d6a53c85ed80ea7d"}
d = sys.argv[1]
bad = [f for f, h in want.items()
       if not os.path.exists(os.path.join(d, f)) or hashlib.sha256(open(os.path.join(d, f), "rb").read()).hexdigest() != h]
if bad:
    sys.exit("build_fwsc: SDK files missing or not AC79NN_SDK_V1.2.1 (%s): no package" % ", ".join(bad))
print("build_fwsc: SDK files are the tested ones")
PY

# AURORA in place of GR!MM's main file (aurora.c includes GR!MM's platform.c, dsp.c and hall.c, unchanged)
cp firmware/src/aurora.c "$GRIMM/firmware/src/aurora.c"
python3 - "$GRIMM/tools/build.py" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
for a, b in (('FW / "src" / "grimm.c"', 'FW / "src" / "aurora.c"'), ('DEFAULT_VERSION = "2.0"', 'DEFAULT_VERSION = "1.0"'),
             ('PRODUCT = "FM-1_966"', 'PRODUCT = "FM-1_944"')):   # (AURORA's own package identity, not GR!MM's)
    if a not in s and b not in s:
        sys.exit("build_fwsc: GR!MM's tools/build.py has changed (%s): check GRIMM_REF" % a)
    s = s.replace(a, b)
open(p, "w").write(s)
PY

NAME="aurora${VER:+-$VER}.fwsc"
if [ -n "$VER" ]; then
    (cd "$GRIMM" && ./build.sh --release "$VER" --name "$NAME")
else
    (cd "$GRIMM" && ./build.sh --name "$NAME")
fi
cp "$GRIMM/build/$NAME" "build/$NAME"
echo "build_fwsc: build/$NAME"
