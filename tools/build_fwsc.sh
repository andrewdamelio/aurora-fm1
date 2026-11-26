#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# AURORA for the FM-1: an installable package (aurora.fwsc) built with GR!MM's build system.
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
AURORA_ID="FM-1_9441126"                        # the package identity: AURORA's own (the hub tells firmwares apart by it)
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
python3 - "$GRIMM/tools/build.py" "$AURORA_ID" <<'PY'
import sys
p, ident = sys.argv[1], sys.argv[2]; s = open(p).read()
for a, b in (('FW / "src" / "grimm.c"', 'FW / "src" / "aurora.c"'), ('DEFAULT_VERSION = "2.0"', 'DEFAULT_VERSION = "1.0"'),
             ('PRODUCT = "FM-1_966"', 'PRODUCT = "%s"' % ident)):   # (AURORA's own package identity, not GR!MM's)
    if a not in s and b not in s:
        sys.exit("build_fwsc: GR!MM's tools/build.py has changed (%s): check GRIMM_REF" % a)
    s = s.replace(a, b)
open(p, "w").write(s)
PY

# Newer FM-1s (the FM-1 / M-VAVE logo under the screen): their flash is a UC25HQ80 (JEDEC 0xB36014), the same generic SPI
# NOR commands as the first units' Puya P25Q80 (0x856014), and their screen wants M-VAVE's full ST7789V setup. GR!MM
# 2.0.2 knows only the first: its update loader refuses the newer flash (nothing installs), the app saves to RAM only,
# and the screen may come out washed out. As Dinghy 0.6.1 / OberVave 3.3.1 fixed it (the screen: Jake Handy, h1).
python3 - "$GRIMM/firmware" <<'PY'
import sys
fw = sys.argv[1]
def patch(rel, a, b):
    p = fw + "/" + rel; s = open(p).read()
    if b in s: return
    if s.count(a) != 1: sys.exit("build_fwsc: GR!MM's %s has changed: check GRIMM_REF" % rel)
    open(p, "w").write(s.replace(a, b))
OK = "(jid == 0x856014u || jid == 0xB36014u)"
patch("src/platform.c", "flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;",
      "{ uint32_t jid = FL_FAR(fl_jedec_ram)(); flash_ok = " + OK + "; }   /* (AURORA: the UC25HQ80 too) */")
patch("src/recovery.c", "flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;",
      "{ uint32_t jid = FL_FAR(fl_jedec_ram)(); flash_ok = " + OK + "; }   /* (AURORA: the UC25HQ80 too) */")
patch("loader/loader.c", "static int ldr_flash_known(void) { return fl_jedec_ram() == 0x856014u; }",
      "static int ldr_flash_known(void) { uint32_t jid = fl_jedec_ram(); return " + OK + "; }   /* (AURORA: the UC25HQ80 too) */")
p = fw + "/src/lcd.c"; s = open(p).read()
if "0xB7, 1, 0x56" not in s:
    i = s.find("static const uint8_t LCD_SEQ[] = {"); j = s.find("};", i)
    if i < 0 or j < 0 or s.count("static const uint8_t LCD_SEQ[] = {") != 1:
        sys.exit("build_fwsc: GR!MM's src/lcd.c has changed: check GRIMM_REF")
    seq = """static const uint8_t LCD_SEQ[] = {      /* (AURORA: M-VAVE's own ST7789V setup, in its order, as Dinghy 0.6.1) */
    0x01, 0,                     /* SWRESET */
    0x00, 1, 150,
    0x28, 0,                     /* DISPOFF */
    0x00, 1, 100,
    0x11, 0,                     /* SLPOUT */
    0x00, 1, 120,
    0xB2, 5, 0x0C, 0x0C, 0x0C, 0x00, 0x33,   /* PORCTRL */
    0xB7, 1, 0x56,               /* GCTRL */
    0xBB, 1, 0x18,               /* VCOMS */
    0xC0, 1, 0x2C,               /* LCMCTRL */
    0xC2, 1, 0x01,               /* VDVVRHEN */
    0xC3, 1, 0x1F,               /* VRHS */
    0xC4, 1, 0x20,               /* VDVS */
    0xC6, 1, 0x0F,               /* FRCTRL2: 60 Hz */
    0xD0, 2, 0xA6, 0xA1,         /* PWCTRL1 */
    0xE0, 14, 0xD0, 0x0D, 0x14, 0x0B, 0x0B, 0x07, 0x3A, 0x44, 0x50, 0x08, 0x13, 0x13, 0x2D, 0x32,   /* PVGAMCTRL */
    0xE1, 14, 0xD0, 0x0D, 0x14, 0x0B, 0x0B, 0x07, 0x3A, 0x44, 0x50, 0x08, 0x13, 0x13, 0x2D, 0x32,   /* NVGAMCTRL */
    0x36, 1, 0x00,               /* MADCTL: top-left origin, RGB order */
    0x3A, 1, 0x55,               /* COLMOD: RGB565 */
    0xE7, 1, 0x00,               /* SPI2EN: one data line */
    0x51, 1, 0xFF,               /* WRDISBV: full brightness */
    0x21, 0,                     /* INVON: IPS panel */
    0x13, 0,                     /* NORON */
"""
    s = s[:i] + seq + s[j:]
    open(p, "w").write(s)
print("build_fwsc: newer FM-1s: the UC25HQ80 flash and M-VAVE's screen setup")
PY

NAME="aurora${VER:+-$VER}.fwsc"
if [ -n "$VER" ]; then
    (cd "$GRIMM" && ./build.sh --release "$VER" --name "$NAME")
else
    (cd "$GRIMM" && ./build.sh --name "$NAME")
fi
cp "$GRIMM/build/$NAME" "build/$NAME"
echo "build_fwsc: build/$NAME"
