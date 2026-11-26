# AURORA

An ambient machine for the M-VAVE FM-1. It plays by itself: slow pads that drift through a scale over a drone, bells that fall into a long echo, and wind, all inside a very large reverb. The screen is the sky it plays under.

![AURORA running in the browser](docs/web.png)

The same firmware runs in a browser, compiled to WebAssembly on an emulated FM-1 panel. Open `web/dist/index.html` to play it; the page is self-contained.

## Controls

Every button opens a page: four knobs, and its own light in the sky over the aurora. The same button again (or HOME) goes back to the sky alone. A full guide is in [docs/help.html](docs/help.html).

| Button | Page | KNOB 1 – 4 | In the sky |
| --- | --- | --- | --- |
| HOME | Sky | DENSITY, BRIGHT, SPACE, DRIFT | the aurora alone |
| FX | Effects | ECHO, BELLS, WIND, DRONE | ripples on the water, snow on the wind, the horizon glowing |
| ENV | Pad shape | ATTACK, RELEASE, LENGTH, PADS | the envelope as a ridge of light, each voice riding it |
| EDIT | Pad tone | WAVE, DETUNE, RESO, SHIMMER | the waveform on the horizon, its twin beating against it |
| LFO | Motion | RATE, SWEEP, WIDTH, TREMOLO | ribbons of the voices' movement; the curtains pulse |
| GLO | Drone | TONE, FIFTH, SUB, BREATH | rings rising from under the horizon |
| SEQ | Machine | CHORDS, RANGE, VOICES, ANSWER | the scale as an arch of stars, the chord lit, the notes threaded |

| Control | What it does |
| --- | --- |
| ALGORITHM | The scene (its sound, sky, scale and settings) |
| SELECT | The scale |
| PRESETS | The root note (or hold SCL and press a key) |
| OCT-/+ | The register (both together: the middle) |
| SCL (tap) | The keys' mode: PADS, HOLD (a key latches its note: a pillar of light), BELLS |
| Keys | Play pads (snapped to the scale), latch them, or strike bells |
| PLAY | The machine plays, or waits (the aurora sinks low) |
| REC | Freeze: the reverb holds what is in it, and the sky turns to ice |
| ARP | A fall of bells: a shower of shooting stars |
| SAVE | Keep all this as the power-on state |
| MIDI in | Notes play the pads |

In the browser: `Z`–`/`, `S`–`'` and `Q`–`0` are the keys, `1`–`7` the pages, `Tab` is SCL, `←` `→` change the scene, `↑` `↓` the scale, `Space` is play, `Shift` is freeze and `Enter` is bells.

## Layout

- `firmware/src/aurora.c`: the firmware
- `shim/`: the small libc the browser build needs
- `web/`: the page and audio worklet; `web/dist/` is the built version
- `test/`: headless checks of the browser build
- `build_web.sh`: builds `web/dist/`
- `tools/build_fwsc.sh`: builds the FM-1 package (`aurora.fwsc`) on GR!MM's build system; GitHub Actions runs it, and releases on a `vX.Y` tag
- `docs/help.html`: the guide to every page and control

## Building the web version

You need clang, wasm-ld, Binaryen (`npm install binaryen`) and a checkout of [GR!MM](https://github.com/GRiMMBIENT/grimm).

```sh
GRIMM=/path/to/grimm BIN=/path/to/node_modules/.bin ./build_web.sh
```

## Credits and licence

AURORA is free software under GPL-3.0. It is built on GR!MM's platform, DSP and reverb, which stand on Felucca, Dinghy, SLOOP and X0X. The screen font is Terminus (SIL OFL 1.1). This is unofficial firmware, not made or endorsed by M-VAVE.
