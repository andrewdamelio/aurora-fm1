# AURORA

Ambient firmware for the M-VAVE FM-1. It plays by itself and never quite repeats: pads move through chord progressions and keep returning to a motif, bells play phrases of it, and the piece drifts through sections and keys. Underneath are a drone, wind, a huge reverb, and soft drums when you want a pulse. The screen is an aurora over a lake, and each page of controls adds its own light to it.

![AURORA running in the browser](docs/web.png)

**[Play it in your browser](https://andrewdamelio.github.io/aurora-fm1/web/dist/)** (the real firmware, no FM-1 needed) · **[The guide](https://andrewdamelio.github.io/aurora-fm1/docs/help.html)**: every page, knob and button.

## Controls

| Button | Page | KNOB 1 – 4 |
| --- | --- | --- |
| HOME | Sky | DENSITY, BRIGHT, SPACE, DRIFT |
| FX | Effects | ECHO, BELLS, WIND, DRONE |
| ENV | Pad shape | ATTACK, RELEASE, LENGTH, PADS |
| EDIT | Pad tone | WAVE, DETUNE, RESO, SHIMMER |
| LFO | Motion | RATE, SWEEP, WIDTH, TREMOLO |
| GLO | Drone | TONE, FIFTH, SUB, BREATH |
| SEQ | Machine | CHORDS, RANGE, VOICES, EVOLVE |
| ARP | Drums | TEMPO, LEVEL, BUSY, SWING |

ALGORITHM picks the scene, SELECT the scale, PRESETS the root. Tap SCL to switch the keys between PADS, HOLD and BELLS. On the drums page, turn LEVEL up from 0 to start them and BUSY for a new pattern. PLAY starts or stops the machine, REC freezes the reverb and the sky, and SAVE keeps the current state for power-on.

## Install

Download `aurora-X.Y.fwsc` from [Releases](../../releases). Then, from a checkout of [GR!MM](https://github.com/GRiMMBIENT/grimm), with `pip install mido python-rtmidi`:

```sh
python3 tools/fm1_install.py aurora-X.Y.fwsc
```

Any FM-1 firmware can go back on over it. If the FM-1 ever gets stuck, the [Firmware Hub](https://thefirmwarehub.com/)'s Rescue page brings it back. This is unofficial firmware: install at your own risk.

## Build

- **FM-1 package:** GitHub Actions builds it on every branch push, and a `vX.Y` tag publishes a release. To build it yourself, run `tools/build_fwsc.sh`.
- **Browser version:** `GRIMM=/path/to/grimm BIN=/path/to/node_modules/.bin ./build_web.sh`. GitHub Pages serves `main`: the emulator at `web/dist/`, the guide at `docs/help.html`.

GPL-3.0. Built on GR!MM's platform, DSP and reverb, which in turn build on Felucca, Dinghy, SLOOP and X0X. Not made or endorsed by M-VAVE.
