# Changelog

## 1.5 — The mix

**A mixer.** Press HOME again for **MIX 1**: PADS, BELLS, CHOIR, DRONE. Press it a third time for **MIX 2**: WIND, LAKE, LOONS, DRUMS. Once more goes back to the sky. Each level is a column of light rising from the lake, as tall as the knob and glowing as that part sounds. DRUMS above 0 starts the drums.

**The old level knobs have new jobs:**

| Page | Knobs |
|---|---|
| EFFECTS | ECHO, **CHIMES** (how often bells ring), **TIME** (echo length, 0.16–0.73 s, glides like tape), **TONE** (dark to bright repeats) |
| PAD SHAPE | ATTACK, RELEASE, LENGTH, **KEYS** (how fast the pads you play swell in) |
| DRUMS | TEMPO, **TONE** (muffled to open), BUSY, SWING |
| NIGHT | **WAVES** (calm to rough water), **CALLS** (how often loons call), **SWELLS** (how often the machine's choir swells in), SLEEP |

**Loons you can hear.** Calls come more often (the first about 12 s after switch-on), louder and less buried in the reverb, with their own level on MIX 2.

**Drums you can hear, all of them.**
- The rim and the shaker were nearly silent: the rim sat 32 dB under the heartbeat, the shaker 45 dB under, and the dark filter took the rest. Now the rim is level with the heartbeat, the mallets just under it, and the shaker a light layer 6 dB down, with its own gentler filter.
- The main beats always play. Calm sections only thin out the extras, instead of cutting down to the heartbeat alone.
- The drums page lights each step as it actually plays, so what you see is what you hear. The rim and heartbeat rows are now warm white, to stand out from the aurora.

**New drum patterns, easier.** On the drums page, press ARP again for a new pattern. It comes in on the next downbeat, at full level, without leaving the page (HOME leaves). Turning BUSY still makes one too.

**SEL's light** is steady in any key mode but PADS, instead of blinking. The screen says which mode you're in.

**Moments, easier.**
- They're on the first four **white** keys, F, G, A and B: hold SAVE and press one to keep a moment, hold HOME and press it to bring it back.
- HOME now changes page when you let go, so recalling a moment doesn't move you off the page.
- In the browser: Alt+Shift+1–4 keeps a moment, Alt+1–4 brings it back.

Settings from 1.4 start fresh.

## 1.4 — The night

The lake comes alive, the machine learns to sing, and AURORA can play you to sleep.

**The lake.** Press FX twice for the new NIGHT page.
- **LAKE**: water lapping at the shore on both sides, and crickets that come and go over minutes. Fireflies drift over the shore.
- **LOONS**: now and then a loon calls from far across the lake: two or three long tones on the scale, gliding, mostly reverb and echo. Rings spread across the water where it calls.

**Choir.**
- **CHOIR on the keys**: SCL now taps through PADS, HOLD, BELLS and CHOIR. Each key sings its note with three real singers, slightly apart, each with their own vibrato. Low keys get the men's voices, high keys the women's, and the vowel drifts through A E I O U. Each sung note sends a shaft of pale light down the sky.
- **The machine sings too**: the NIGHT page's **CHOIR** knob sets how often a soft choir swells in on a new chord, more in calm and night sections. It never takes a voice you are singing on the keys.

**Sleep timer.** The NIGHT page's **SLEEP** knob sets 15, 30, 45, 60, 90 or 120 minutes.
- For the second half the machine thins out. For the last third (five minutes at most) the sound fades and the aurora sinks to the stars, then it goes quiet.
- A crescent moon sets across the sky as the time runs out.
- **PLAY** wakes it.

**MIDI out.** Everything AURORA plays now goes out over USB, so it can drive your other synths with the same chords, motif and key changes:

| Channel | Part |
|---|---|
| 1 | Pads |
| 2 | Bells |
| 3 | The drone's root |
| 4 | The drums' mallets |
| 5 | Choir |
| 10 | Drums: 36 heartbeat, 37 rim, 70 shaker, 82 open shaker |

Notes that come in over MIDI aren't sent back out, and every note that starts also stops.

**Four saved moments.**
- **Hold SAVE** and press one of the first four keys (F3 to G#3) to keep the whole setup: scene, scale, root, octave, key mode and every knob.
- **Hold HOME** and press the same key to bring it back.
- **Tap SAVE** to keep the power-on state, as before.

**Also**
- Settings from earlier versions start fresh (the save format grew to hold the moments): set things up again and press SAVE.

The choir is made from VocalSet (Wilkins, Seetharaman, Wahl, Pardo, Northwestern University, 2018; CC BY 4.0), via GR!MM. See [LICENSING.md](LICENSING.md).

## 1.3

- **Newer FM-1s.** AURORA now accepts the UC25HQ80 flash chip of newer FM-1s (the ones with the logo under the screen): it installs on them, and SAVE keeps working after power-off. It also sends M-VAVE's full screen setup, so their screens aren't washed out. These are the same fixes Dinghy and OberVave carry.

## 1.2

- **Its own identity.** AURORA's package identity is FM-1_9441126.
- **Built from source.** Releases are built and signed on GitHub by the FM-1 Firmware Hub's shared workflow.
- **Generative.**
  - Chord progressions, and a motif that keeps coming back.
  - Sections (calm, bloom, night, sparkle, drift) that lean the knobs their way.
  - Key changes, and the **EVOLVE** knob to set how much of this happens.
- **Ambient drums.** ARP opens the drums' page; LEVEL starts and stops them. The kit is a heartbeat, a rim, a shaker and mallets on the chord.
- **Pages.** Every button opens a page of four knobs, each with its own light in the sky.
- **Key modes.** PADS, HOLD and BELLS.
- **Freeze** turns the sky to ice.
