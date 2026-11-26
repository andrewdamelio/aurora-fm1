/* SPDX-License-Identifier: GPL-3.0-only */
/* AURORA: an ambient machine for the M-VAVE FM-1. It plays by itself: slow pads that drift through a scale over a
 * drone, bells that fall into a long echo, wind, all inside a very large reverb; the screen is the sky it plays under.
 *
 * Built on GR!MM's tree (github.com/GRiMMBIENT/grimm, GPL-3.0), used unchanged: the platform (platform.c: Dinghy's, by
 * zvenson, on Felucca's / SLOOP's FM-1 work; in a browser fm1_web.c), the canvas (gfx.c), Felucca's fixed-point DSP
 * (dsp.c: the oscillators, the filter, the tables) and GR!MM's SPACE reverb (hall.c, Felucca's HALL). Everything in this
 * file is AURORA's own. Integers only, no 64-bit division: the chip's rules (docs/RULES.md).
 *
 * Pages: each button opens its four knobs and its own light in the sky; the same button again (or HOME) goes back.
 *   HOME   SKY        DENSITY  BRIGHT   SPACE    DRIFT      the aurora alone
 *   FX     EFFECTS    ECHO     BELLS    WIND     DRONE      ripples on the water, snow, the horizon's glow
 *   ENV    PAD SHAPE  ATTACK   RELEASE  LENGTH   PADS       the pads' envelope, drawn across the sky
 *   EDIT   PAD TONE   WAVE     DETUNE   RESO     SHIMMER    their waveform, on the horizon
 *   LFO    MOTION     RATE     SWEEP    WIDTH    TREMOLO    ribbons of their movement
 *   GLO    DRONE      TONE     FIFTH    SUB      BREATH     the drone's rings rising from under the horizon
 *   SEQ    MACHINE    CHORDS   RANGE    VOICES   EVOLVE     the scale as a constellation, its chord lit
 *   ARP    DRUMS      TEMPO    LEVEL    BUSY     SWING      the pattern as rows of lights, a playhead (LEVEL 0: off)
 *
 *   ALGORITHM    the scene (its sound, its sky, its scale, its settings)      SELECT   the scale
 *   PRESETS      the root note                                                OCT-/+   the register (both: middle)
 *   keys         PADS: play pads, snapped to the scale; HOLD: a key latches its note; BELLS: strike bells; CHOIR: sing
 *   SEL          tap: the keys' mode (PADS, HOLD, BELLS, CHOIR); held + a key: that key is the root
 *   PLAY         the machine plays / waits (lit: playing)      REC    FREEZE: the reverb and the sky hold still (lit)
 *   SAVE         keep all this as the power-on state
 *   MIDI in      notes play the pads
 */
#define APP_NAME "AURORA"
#include "platform.c"
#define LV_COUNT(c, x, k) ((void)0)
#include "dsp.c"
#include "hall.c"
#include "grimm_choir.h"                     /* the choir's singers (VocalSet, CC BY 4.0: see LICENSING.md) */

/* ------------------------------------------------------------------ small things */
static inline int32_t sin15(uint32_t ph)        /* a full cycle over 2^32 -> -32767 .. 32767 */
{
    uint32_t i = ph >> 22;
    int32_t a = SINE[i];
    return a + (((SINE[i + 1u] - a) * (int32_t)((ph >> 7) & 32767u)) >> 15);
}
static uint32_t a_seed = 0x9E3779B9u;           /* the audio interrupt's random numbers */
static inline uint32_t arnd(void)
{
    a_seed ^= a_seed << 13, a_seed ^= a_seed >> 17, a_seed ^= a_seed << 5;
    return a_seed;
}
static inline uint32_t arnd_n(uint32_t n) { return (uint32_t)(((uint64_t)arnd() * n) >> 32); }
static uint32_t v_seed = 0xC0FFEE11u;           /* the main loop's */
static inline uint32_t vrnd(void)
{
    v_seed ^= v_seed << 13, v_seed ^= v_seed >> 17, v_seed ^= v_seed << 5;
    return v_seed;
}
static inline uint32_t vrnd_n(uint32_t n) { return (uint32_t)(((uint64_t)vrnd() * n) >> 32); }
static char *put_s(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static char *put_u(char *d, uint32_t v)
{
    char t[11];
    uint32_t n = 0;
    do t[n++] = (char)('0' + v % 10u), v /= 10u; while (v);
    while (n) *d++ = t[--n];
    *d = 0;
    return d;
}

/* ------------------------------------------------------------------ what is set (the main loop writes, the audio reads) */
enum { P_DENSITY, P_BRIGHT, P_SPACE, P_DRIFT,   /* HOME: the sky */
       P_ECHO, P_BELLS, P_ETIME, P_ETONE,       /* FX: the echo, how often bells ring (CHIMES), its time, its tone */
       P_ATTACK, P_RELEASE, P_LENGTH, P_KATK,   /* ENV: the pads' shape; KEYS: how fast the keys' notes swell in */
       P_WAVE, P_DETUNE, P_RESO, P_SHIMMER,     /* EDIT: their tone */
       P_RATE, P_SWEEP, P_WIDTH, P_TREMOLO,     /* LFO: their motion */
       P_DTONE, P_FIFTH, P_SUB, P_BREATH,       /* GLO: the drone */
       P_CHORDS, P_RANGE, P_VOICES, P_EVOLVE,   /* SEQ: the machine */
       P_TEMPO, P_DKTONE, P_BUSY, P_SWING,      /* ARP: the drums */
       P_WAVES, P_LOONS, P_MCHOIR, P_SLEEP,     /* FX twice: the night: the water, the loons' calls, the machine's choir, sleep */
       P_PADS, P_BVOL, P_CVOL, P_DRONE,         /* HOME twice: MIX 1 */
       P_WIND, P_LAKE, P_LVOL, P_DLEVEL,        /* HOME three times: MIX 2 (DRUMS 0: off) */
       NPARAM };
static const char *const P_NAME[NPARAM] = {
    "DENSITY", "BRIGHT", "SPACE", "DRIFT", "ECHO", "CHIMES", "TIME", "TONE",
    "ATTACK", "RELEASE", "LENGTH", "KEYS", "WAVE", "DETUNE", "RESO", "SHIMMER",
    "RATE", "SWEEP", "WIDTH", "TREMOLO", "TONE", "FIFTH", "SUB", "BREATH",
    "CHORDS", "RANGE", "VOICES", "EVOLVE", "TEMPO", "TONE", "BUSY", "SWING", "WAVES", "CALLS", "SWELLS", "SLEEP",
    "PADS", "BELLS", "CHOIR", "DRONE", "WIND", "LAKE", "LOONS", "DRUMS"};
/* where the knobs start that a scene does not set */
static const uint8_t P_DEF[NPARAM] = {
    64, 64, 90, 56, 60, 40, 76, 40,
    43, 53, 64, 12, 50, 40, 23, 0,
    56, 20, 90, 0, 50, 64, 0, 24,
    64, 64, 80, 64, 19, 50, 50, 20, 50, 60, 36, 0,
    85, 85, 85, 80, 30, 40, 70, 0};
static const uint8_t SCENE_P[8] = {P_DENSITY, P_BRIGHT, P_SPACE, P_DRIFT, P_ECHO, P_BELLS, P_WIND, P_DRONE};   /* (a scene's def[]) */
enum { PG_HOME, PG_FX, PG_ENV, PG_EDIT, PG_LFO, PG_GLO, PG_SEQ, PG_RHY, PG_NIGHT, PG_MIX1, PG_MIX2, NPAGE };
static const uint8_t PAGE_BTN[NPAGE] = {BTN_HOME, BTN_FX, BTN_ENV, BTN_EDIT, BTN_LFO, BTN_GLO, BTN_SEQ, BTN_ARP, BTN_FX, BTN_HOME, BTN_HOME};
static const char *const PAGE_NAME[NPAGE] = {"SKY", "EFFECTS", "PAD SHAPE", "PAD TONE", "MOTION", "DRONE", "MACHINE", "DRUMS", "NIGHT",
                                             "MIX 1", "MIX 2"};
static volatile int32_t vis_mix[8];             /* (the audio's) each part's level now, 0..255: pads, bells, choir, drone,
                                                 * wind, lake, loons, drums */
static volatile int32_t g_svol = 256, g_sthin = 256;   /* the sleep timer: the volume and how busy the machine is (0..256) */
enum { KM_PADS, KM_HOLD, KM_BELLS, KM_CHOIR, NKMODE };
static const char *const KM_NAME[NKMODE] = {"KEYS: PADS", "KEYS: HOLD", "KEYS: BELLS", "KEYS: CHOIR"};
static const char *const KM_SAY[NKMODE] = {"KEYS PLAY PADS", "KEYS HOLD NOTES", "KEYS RING BELLS", "KEYS SING CHOIR"};
static const char *const KM_HOW[NKMODE] = {"", "", "", ""};

static volatile int32_t par[NPARAM];            /* 0 .. 127 */
static int32_t md[NPARAM];                      /* (the audio's) how the section leans each knob (x 256) */
#define PAR(p) clamp(par[p] + (md[p] >> 8), 0, 127)   /* a knob as the sound uses it: where it is, and the lean */
static volatile int32_t g_shift;                /* (the audio's) the key the sections moved to, over the root */
#define EROOT ((g_root + g_shift) % 12)        /* the root as it sounds */
static volatile int32_t vis_mood;
static volatile int32_t g_scene, g_scale, g_root, g_oct, g_run = 1, g_freeze;
static volatile int32_t g_drums;                /* the drums play (their LEVEL above 0) */

typedef struct { const char *name; uint8_t n; uint8_t st[7]; } scale_t;
static const scale_t SCALES[] = {
    {"LYDIAN", 7, {0, 2, 4, 6, 7, 9, 11}},   {"MAJOR", 7, {0, 2, 4, 5, 7, 9, 11}},
    {"MAJ PENTA", 5, {0, 2, 4, 7, 9}},       {"SUSPENDED", 5, {0, 2, 5, 7, 10}},
    {"DORIAN", 7, {0, 2, 3, 5, 7, 9, 10}},   {"MINOR", 7, {0, 2, 3, 5, 7, 8, 10}},
    {"MIN PENTA", 5, {0, 3, 5, 7, 10}},      {"PHRYGIAN", 7, {0, 1, 3, 5, 7, 8, 10}},
    {"HIRAJOSHI", 5, {0, 2, 3, 7, 8}},       {"WHOLE TONE", 6, {0, 2, 4, 6, 8, 10}},
    {"FIFTHS", 2, {0, 7}},
};
#define NSCALE (sizeof SCALES / sizeof SCALES[0])
static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

/* a scene: its sound (the pads' wave: 0 sines .. 127 saws; their attack and release in tenths of a second; their
 * register; their shimmer), where its first eight knobs start, and its sky: the curtains' colours from their lower edge
 * up, the sky at the top and at the horizon */
typedef struct {
    const char *name;
    uint8_t scale, root, wave, atk, rel;
    int8_t reg;
    uint8_t shim;
    uint8_t def[8];
    uint8_t c[3][3], sky[2][3];
} scene_t;
static const scene_t SCENES[] = {
    {"BOREAL", 0, 2, 58, 28, 70, 0, 0, {52, 64, 92, 56, 60, 44, 30, 78},
     {{70, 255, 140}, {30, 205, 215}, {150, 70, 235}}, {{2, 4, 14}, {8, 20, 38}}},
    {"EMBER", 4, 9, 96, 40, 90, -1, 0, {40, 46, 84, 40, 48, 26, 46, 96},
     {{255, 190, 60}, {255, 86, 40}, {170, 30, 120}}, {{9, 3, 8}, {40, 14, 14}}},
    {"ABYSS", 5, 0, 26, 50, 110, -1, 0, {28, 38, 112, 70, 78, 34, 58, 104},
     {{60, 140, 255}, {74, 70, 235}, {150, 50, 210}}, {{1, 2, 10}, {4, 10, 34}}},
    {"HALO", 2, 5, 14, 22, 80, 1, 46, {66, 84, 100, 48, 84, 92, 16, 56},
     {{255, 226, 240}, {255, 120, 200}, {140, 110, 255}}, {{8, 4, 16}, {34, 16, 40}}},
    {"GLACIER", 3, 7, 0, 34, 100, 0, 30, {46, 72, 106, 30, 70, 62, 40, 70},
     {{170, 255, 250}, {60, 190, 255}, {60, 90, 235}}, {{2, 6, 16}, {10, 30, 48}}},
    {"NOCTURNE", 8, 4, 44, 36, 96, 0, 0, {44, 54, 98, 84, 66, 50, 34, 88},
     {{255, 70, 170}, {160, 56, 235}, {50, 70, 210}}, {{6, 2, 14}, {26, 8, 36}}},
};
#define NSCENE (sizeof SCENES / sizeof SCENES[0])

static int32_t scale_note(const scale_t *s, int32_t deg)   /* the scale's degree deg (0 up) in semitones above the root */
{
    int32_t o = deg / (int32_t)s->n;
    return 12 * o + s->st[deg - o * (int32_t)s->n];
}
static int32_t scale_snap(int32_t note)         /* the scale's note nearest to note (below first) */
{
    const scale_t *s = &SCALES[g_scale];
    static const int8_t TRY[12] = {0, -1, 1, -2, 2, -3, 3, -4, 4, -5, 5, 6};
    uint32_t t, k;
    for (t = 0; t < 12u; t++) {
        int32_t pc = ((note + TRY[t] - EROOT) % 12 + 12) % 12;
        for (k = 0; k < s->n; k++)
            if (s->st[k] == pc) return note + TRY[t];
    }
    return note;
}
/* the knobs as the sound uses them */
static inline int32_t atk10_of(void) { return 1 + par[P_ATTACK] * 79 / 127; }        /* 0.1 .. 8 s */
static inline int32_t rel10_of(void) { return 5 + par[P_RELEASE] * 155 / 127; }      /* 0.5 .. 16 s */
static inline int32_t len256_of(void) { return 64 + par[P_LENGTH] * 384 / 127; }     /* the machine's notes: 0.25 .. 1.75 x */
static inline int32_t range_top(const scale_t *s)                                    /* the machine's highest degree */
{
    int32_t t = (int32_t)s->n * (1 + par[P_RANGE] * 3 / 127) + 1;
    return t > 28 ? 28 : t;
}

/* what the screen shows of the sound (the audio writes, the main loop reads) */
#define NV 6u                                   /* the pads' voices */
static volatile int32_t vis_lvl[NV], vis_note[NV];   /* each voice: 0 .. 255, its note */
static volatile int32_t vis_env[NV], vis_stage[NV], vis_hf[NV], vis_kind[NV];   /* its envelope 0..255; 0 attack, 1 held,
                                                 * 2 released; how far through its hold 0..255; 0 machine, 1 key, 2 latched */
static volatile int32_t vis_out;                /* the output's peak over the last block (Q15) */
static volatile uint32_t vis_bell_seq;          /* bells struck so far */
static volatile int32_t vis_bell_note, vis_bell_kind;   /* the last: its note; 0 the machine, 1 a fall, 2 a key */
static volatile int32_t vis_chord, vis_drone;   /* the degree the harmony leans on; the drone's level (Q15) */
static volatile uint32_t vis_chord_seq;

/* notes from the keys and MIDI: id << 16 | note << 8 | kind << 1 | on (kind 1: a bell) */
static volatile uint32_t evq[32];
static volatile uint32_t ev_w, ev_r;
static void ev_put(uint32_t id, int32_t note, uint32_t on)
{
    if (ev_w - ev_r >= 32u) return;
    evq[ev_w % 32u] = id << 16 | (uint32_t)clamp(note, 0, 127) << 8 | on;
    ev_w = ev_w + 1u;
}

/* MIDI out (the audio writes, the main loop sends): what AURORA plays, over USB. Pads channel 1, bells 2, the drone 3,
 * the drums' mallets 4, the choir 5, the drums 10 (36 the heartbeat, 37 the rim, 70 the shaker, 82 the open one). Notes
 * that came in over MIDI are not sent back out */
static volatile uint32_t moq[64];
static volatile uint32_t moq_w, moq_r;
static void mo(uint32_t st, int32_t d1, int32_t d2)
{
    if (moq_w - moq_r >= 64u) return;
    moq[moq_w & 63u] = st << 16 | (uint32_t)clamp(d1, 0, 127) << 8 | (uint32_t)clamp(d2, 1, 127);
    moq_w = moq_w + 1u;
}

/* ------------------------------------------------------------------ the sound (the audio interrupt) */
#define BLK_PER_S 172                           /* blocks of 256 frames a second (172.3) */
#define BLK(ms) ((int32_t)(ms) * BLK_PER_S / 1000)
#define VB 256u
static int32_t bus_l[VB], bus_r[VB], bus_rev[VB], bus_dly[VB];

typedef struct {
    uint32_t ph1, ph2, ph3, lfo, lfo_inc;
    int32_t ic1, ic2;                           /* its filter */
    int32_t env, a_prev;                        /* the envelope (Q24, straight), the level the last block ended at */
    int32_t atk;                                /* Q24 a block */
    int32_t gate, hold, hold_all, note, id, pan, det;   /* hold: blocks until it lets go (0: until its key does) */
    uint32_t born;
    int32_t mn;                                 /* the note sent out over MIDI, + 1 (0: none) */
} pv_t;
static pv_t pv[NV];
static uint32_t a_blk;                          /* blocks so far */

/* the knobs, worked out once a block for all the voices */
static struct { int32_t rel, vol, wave, spread, reso, shk, rmul, sweep, width, trem; } kb;
static void kb_update(void)
{
    int32_t r = par[P_RATE];
    kb.rel = 975419 / rel10_of();               /* 2^24 over the release in tenths of a second, a block */
    kb.vol = par[P_PADS];                       /* x / 85: 0 .. 1.5 */
    kb.wave = par[P_WAVE];
    kb.spread = 1 + par[P_DETUNE] * 30 / 127;   /* cents either side */
    kb.reso = par[P_RESO] * 100 / 127;
    kb.shk = par[P_SHIMMER] * 14000 / 127;
    kb.rmul = 64 + r * r * 960 / 16129;         /* the voices' slow movement: 0.25 .. 4 x (x 256) */
    kb.sweep = 4 + PAR(P_DRIFT) / 10 + par[P_SWEEP] * 28 / 127;
    kb.width = par[P_WIDTH];
    kb.trem = par[P_TREMOLO] * 26000 / 127;
}

#define PAD_VOL 7400                            /* a voice at full level (Q15) */
static void pad_render(pv_t *v, uint32_t vi)
{
    int32_t bright = PAR(P_BRIGHT), drift = PAR(P_DRIFT), echo = PAR(P_ECHO);
    int32_t e, a0 = v->a_prev, a1, vib, spread = kb.spread, p16, gl, gr, pan, ds, shk = kb.shk;
    uint32_t inc1, inc2, inc3, ph1 = v->ph1, ph2 = v->ph2, ph3 = v->ph3, i;
    int32_t ic1 = v->ic1, ic2 = v->ic2, wave = kb.wave, ls;
    tsvf_t c;
    if (v->gate) {
        v->env += v->atk;
        if (v->env > 1 << 24) v->env = 1 << 24;
        if (v->hold > 0 && --v->hold == 0) v->gate = 0;
    }
    if (!v->gate && v->mn) mo(0x80, v->mn - 1, 64), v->mn = 0;
    if (!v->gate) {
        v->env -= kb.rel;
        if (v->env < 0) v->env = 0;
    }
    e = v->env >> 9;
    a1 = (((e * e) >> 15) * PAD_VOL) >> 15;
    vis_lvl[vi] = (a1 * 255) / PAD_VOL;
    vis_env[vi] = v->env >> 16;
    vis_stage[vi] = !v->gate ? 2 : v->env < 1 << 24 ? 0 : 1;
    vis_hf[vi] = v->hold_all > 0 ? (v->hold_all - v->hold) * 255 / v->hold_all : 128;
    if (!a1 && !a0) { v->a_prev = 0; return; }
    v->lfo += (uint32_t)(((uint64_t)v->lfo_inc * (uint32_t)kb.rmul) >> 8);
    ls = sin15(v->lfo);
    if (kb.trem) a1 = (a1 * (32768 - ((kb.trem * ((32767 + sin15(v->lfo * 5u)) >> 1)) >> 15))) >> 15;
    a1 = a1 * kb.vol / 85;
    vib = (ls * (2 + drift * 9 / 127)) >> 15;   /* cents: a slow lean, wider with DRIFT */
    p16 = v->note * 16;
    inc1 = cents_inc(p16, v->det + vib + spread, 0);
    inc2 = cents_inc(p16, v->det + vib - spread, 0);
    inc3 = cents_inc(p16, v->det + vib, 0);
    tsvf_coef(&c, ((36 + bright * 62 / 127 + (v->note - 60) / 3) << 8) + ((sin15(v->lfo * 3u + 0x40000000u) * kb.sweep) >> 7)
                  + ((e * 10) >> 7), kb.reso);
    pan = 128 + (((v->pan - 128) * kb.width) >> 7) + ((((sin15(v->lfo * 2u) * (10 + drift / 2)) >> 15) * kb.width) >> 7);
    pan = clamp(pan, 0, 256);
    gl = SINE[256 - pan], gr = SINE[pan];
    ds = 20 + echo * 60 / 127;                  /* its echo send, / 256 */
    for (i = 0; i < VB; i++) {
        int32_t s, a, y;
        ph1 += inc1, ph2 += inc2, ph3 += inc3;
        if (wave) {
            int32_t w = (osc_saw(ph1, inc1) + osc_saw(ph2, inc2)) >> 1;
            w = tsvf_lp(&c, w, &ic1, &ic2);
            s = (sin15(ph3) * (128 - wave) + w * wave) >> 7;
        } else {
            s = (sin15(ph1) + sin15(ph2) + (sin15(ph3 << 1) >> 3)) >> 1;
        }
        a = a0 + (((a1 - a0) * (int32_t)i) >> 8);
        y = (s * a) >> 15;
        bus_l[i] += (y * gl) >> 15;
        bus_r[i] += (y * gr) >> 15;
        bus_rev[i] += (y * 150) >> 8;
        bus_dly[i] += (y * ds) >> 8;
        if (shk) {                              /* SHIMMER: its octave and the one above, mostly into the reverb */
            int32_t z = ((((sin15(ph3 << 1) + (sin15(ph3 << 2) >> 1)) * shk) >> 15) * a) >> 15;
            bus_l[i] += z >> 2, bus_r[i] += z >> 2;
            bus_rev[i] += z;
        }
    }
    v->ph1 = ph1, v->ph2 = ph2, v->ph3 = ph3, v->ic1 = ic1, v->ic2 = ic2, v->a_prev = a1;
}

static void pad_start(pv_t *v, int32_t note, int32_t id, int32_t hold_blk, int32_t atk10)
{
    v->note = clamp(note, 24, 108);
    v->id = id;
    v->hold = v->hold_all = hold_blk;
    v->atk = 975419 / (atk10 < 1 ? 1 : atk10);  /* 2^24 over atk10 tenths of a second, a block */
    v->pan = 36 + (int32_t)arnd_n(185);
    v->det = (int32_t)arnd_n(9) - 4;
    v->lfo_inc = 1000000u + arnd_n(1900000u);   /* a cycle in 9 .. 25 s */
    if (!v->a_prev) v->ph2 = arnd(), v->lfo = arnd(), v->ic1 = v->ic2 = 0;
    v->born = a_blk;
    v->gate = 1;
    if (v->mn) mo(0x80, v->mn - 1, 64), v->mn = 0;
    if (id < 32 || id >= 64) mo(0x90, v->note, 88), v->mn = v->note + 1;   /* (not the notes that came in over MIDI) */
    vis_note[v - pv] = v->note;
    vis_kind[v - pv] = id < 0 ? 0 : id >= 64 && id < 96 ? 2 : 1;
}
static pv_t *pad_free(void)                     /* a silent voice, or 0 */
{
    uint32_t k;
    for (k = 0; k < NV; k++)
        if (!pv[k].gate && !pv[k].env && !pv[k].a_prev) return &pv[k];
    return 0;
}
static pv_t *pad_take(void)                     /* for a key: a silent voice, else the quietest fading one, else the
                                                 * machine's oldest, else the oldest */
{
    pv_t *v = pad_free(), *q = 0;
    uint32_t k;
    if (v) return v;
    for (k = 0; k < NV; k++)
        if (!pv[k].gate && (!q || pv[k].env < q->env)) q = &pv[k];
    if (q) return q;
    for (k = 0; k < NV; k++)
        if (pv[k].id < 0 && (!q || a_blk - pv[k].born > a_blk - q->born)) q = &pv[k];
    if (q) return q;
    for (k = 0; k < NV; k++)
        if (!q || a_blk - pv[k].born > a_blk - q->born) q = &pv[k];
    return q;
}

/* the bells: a sine bent by another (two-operator FM), struck and left to ring */
#define NB 4u
typedef struct { uint32_t pc, pm, ic, im; int32_t amp, a_prev, idx, kamp, pan, mn, moff; } bell_t;
static bell_t bell[NB];
static void bell_strike(int32_t note, int32_t vel, int32_t kind)   /* vel 0 .. 127 */
{
    static const uint8_t RATIO2[4] = {7, 4, 7, 11};   /* the bending sine's pitch, in halves of the bell's: 3.5, 2, 3.5, 5.5 */
    bell_t *b = &bell[0];
    uint32_t k;
    for (k = 1; k < NB; k++)
        if (bell[k].amp < b->amp) b = &bell[k];
    while (note > 93) note -= 12;               /* (never shrill: A6 at most) */
    note = clamp(note, 48, 93);
    b->ic = pitch_inc((uint32_t)note * 16u);
    b->im = (b->ic >> 1) * (note > 84 ? 4u : RATIO2[arnd_n(4)]);   /* (high up: the octave only, nothing near the top of the band) */
    b->amp = (((5000 + vel * 52) * 9) >> 4) * par[P_BVOL] / 85;   /* (MIX: BELLS, 85 as before) */
    b->idx = clamp(13000 + (int32_t)arnd_n(22000) - (note - 72) * 550, 4000, 40000);
    b->kamp = note > 86 ? 7 : 8;                /* ~0.75 s or ~1.5 s to 1/e, then the echo and the reverb */
    b->pan = 30 + (int32_t)arnd_n(196);
    vis_bell_note = note;
    vis_bell_kind = kind;
    vis_bell_seq = vis_bell_seq + 1u;
    if (b->mn) mo(0x81, b->mn - 1, 64);
    mo(0x91, note, 40 + vel / 2), b->mn = note + 1, b->moff = BLK(600);
}
static void bell_render(bell_t *b)
{
    int32_t a0 = b->a_prev, a1 = b->amp, idx = b->idx, gl = SINE[256 - b->pan], gr = SINE[b->pan];
    int32_t ds = 70 + PAR(P_ECHO) * 150 / 127;
    uint32_t pc = b->pc, pm = b->pm, i;
    if (b->mn && --b->moff <= 0) mo(0x81, b->mn - 1, 64), b->mn = 0;
    if (!a0 && !a1) return;
    for (i = 0; i < VB; i++) {
        int32_t a = a0 + (((a1 - a0) * (int32_t)i) >> 8), y;
        pc += b->ic, pm += b->im;
        y = (sin15(pc + (uint32_t)(sin15(pm) * idx)) * a) >> 15;
        bus_l[i] += (y * gl) >> 15;
        bus_r[i] += (y * gr) >> 15;
        bus_rev[i] += (y * 170) >> 8;
        bus_dly[i] += (y * ds) >> 8;
    }
    b->pc = pc, b->pm = pm, b->a_prev = a1;
    b->amp = a1 - (a1 >> b->kamp) - (a1 > 0);
    if (b->amp < 0) b->amp = 0;
    b->idx = idx - (idx >> 5);
}

/* the choir (the keys in CHOIR): real singers' single cycles (GR!MM's grimm_choir.h, from VocalSet: Wilkins,
 * Seetharaman, Wahl, Pardo, Northwestern University, 2018, CC BY 4.0), three to a note, a little apart in pitch, each
 * with its own vibrato; the men's voices low, the women's high; the vowel drifting slowly through A E I O U; deep in
 * the reverb. Four notes at once. A cycle's size (256 .. 32 points: 64 .. 8 harmonics) as its pitch allows, so nothing
 * folds over above ~20 kHz (as GR!MM's VOICE does) */
#define NCH 4u
#define CH_SING 3u
#define CH_VOL 9000                             /* a note at full level (Q15) */
static const int8_t CH_DET[CH_SING] = {-7, 0, 6};   /* each singer's pitch, cents */
static const uint32_t CH_VIB[CH_SING] = {129000000u, 139000000u, 121000000u};   /* their vibratos: ~5.2, 5.6, 4.9 Hz */
static const uint16_t CH_OFF[4] = {0, 256, 384, 448};   /* a cycle's sizes in CHOIR_T: 256, 128, 64, 32 */
typedef struct { int32_t note, id, gate, env, a_prev, vpos, vdir, pan, hold, lvl, atk, mn; uint32_t ph[CH_SING], vp[CH_SING], born; } ch_t;
static ch_t chv[NCH];
static volatile int32_t vis_ch_lvl[NCH], vis_ch_note[NCH];
static int choir_on(int32_t note, int32_t id, int32_t hold, int32_t lvl, int32_t atk10, int32_t steal)   /* 0: no voice */
{
    ch_t *c = 0;
    uint32_t k;
    for (k = 0; k < NCH && !c; k++)
        if (!chv[k].gate && !chv[k].env && !chv[k].a_prev) c = &chv[k];
    if (!c && !steal) return 0;
    for (k = 0; k < NCH && !c; k++)             /* else one letting go, else the oldest */
        if (!chv[k].gate) c = &chv[k];
    if (!c)
        for (k = 0; k < NCH; k++)
            if (!c || a_blk - chv[k].born > a_blk - c->born) c = &chv[k];
    if (!c->a_prev) {
        for (k = 0; k < CH_SING; k++) c->ph[k] = 0, c->vp[k] = arnd();
        c->vpos = (int32_t)arnd_n(1024), c->vdir = arnd_n(2) ? 1 : -1;
    }
    c->note = clamp(note, 36, 84), c->id = id, c->gate = 1, c->born = a_blk, c->hold = hold, c->lvl = lvl;
    c->atk = 975419 / (atk10 < 1 ? 1 : atk10);
    c->pan = 60 + (int32_t)arnd_n(136);
    if (c->mn) mo(0x84, c->mn - 1, 64);
    mo(0x94, c->note, 40 + lvl / 4), c->mn = c->note + 1;
    vis_ch_note[c - chv] = c->note;
    return 1;
}
static void choir_off(int32_t id)
{
    uint32_t k;
    for (k = 0; k < NCH; k++)
        if (chv[k].gate && chv[k].id == id) chv[k].gate = 0;
}
static void choir_render(ch_t *c, uint32_t ci)
{
    struct { const int8_t *t0, *t1; uint32_t ph, inc, sh, sf, mk; } sg[CH_SING];
    int32_t e, a0 = c->a_prev, a1, vi, mf, gl, gr;
    uint32_t i, k, w, z;
    if (c->gate) {
        c->env += c->atk;
        if (c->env > 1 << 24) c->env = 1 << 24;
        if (c->hold > 0 && --c->hold == 0) c->gate = 0;
    }
    if (!c->gate && c->mn) mo(0x84, c->mn - 1, 64), c->mn = 0;
    if (!c->gate) {
        c->env -= kb.rel;
        if (c->env < 0) c->env = 0;
    }
    e = c->env >> 9;
    a1 = ((((((e * e) >> 15) * CH_VOL) >> 15) * c->lvl) >> 8) * par[P_CVOL] / 85;   /* (MIX: CHOIR) */
    vis_ch_lvl[ci] = (a1 * 255) / CH_VOL;
    if (!a1 && !a0) { c->a_prev = 0; return; }
    c->vpos += c->vdir;                         /* the vowel: one every ~1.5 s, there and back */
    if (c->vpos <= 0) c->vpos = 0, c->vdir = 1;
    if (c->vpos >= 4 * 256) c->vpos = 4 * 256, c->vdir = -1;
    vi = c->vpos >> 8, mf = c->vpos & 255;
    if (vi >= 4) vi = 3, mf = 255;
    w = c->note >= 60, z = c->note >= (w ? 66 : 54);   /* the women above middle C; each recorded at two notes */
    for (k = 0; k < CH_SING; k++) {
        const int8_t *t = CHOIR_T[w][k][z][0];
        uint32_t b, lv;
        c->vp[k] += CH_VIB[k];
        b = cents_inc(c->note * 16, CH_DET[k] + ((sin15(c->vp[k]) * 9) >> 15), 0);
        for (lv = 0; lv < 3u && (b >> 20) * (64u >> lv) > 1858u; lv++)
            ;
        sg[k].t0 = t + vi * CHOIR_LEN + CH_OFF[lv], sg[k].t1 = t + (vi + 1 < 5 ? vi + 1 : vi) * CHOIR_LEN + CH_OFF[lv];
        sg[k].sh = 24u + lv, sg[k].sf = 16u + lv, sg[k].mk = (256u >> lv) - 1u, sg[k].ph = c->ph[k], sg[k].inc = b;
    }
    gl = SINE[256 - c->pan], gr = SINE[c->pan];
    for (i = 0; i < VB; i++) {
        int32_t acc = 0, a = a0 + (((a1 - a0) * (int32_t)i) >> 8), y;
        for (k = 0; k < CH_SING; k++) {
            uint32_t ph = sg[k].ph, j = ph >> sg[k].sh, j1 = (j + 1u) & sg[k].mk;
            int32_t fr = (int32_t)(ph >> sg[k].sf & 255u), s0 = sg[k].t0[j], v = (s0 << 8) + (sg[k].t0[j1] - s0) * fr;
            int32_t e0 = sg[k].t1[j], u = (e0 << 8) + (sg[k].t1[j1] - e0) * fr;
            acc += v + (((u - v) * mf) >> 8);
            sg[k].ph = ph + sg[k].inc;
        }
        y = ((((acc * 10923) >> 15)) * a) >> 15;   /* (a third each) */
        bus_l[i] += (y * gl) >> 15;
        bus_r[i] += (y * gr) >> 15;
        bus_rev[i] += (y * 210) >> 8;
        bus_dly[i] += (y * 24) >> 8;
    }
    for (k = 0; k < CH_SING; k++) c->ph[k] = sg[k].ph;
    c->a_prev = a1;
}

/* the drone: the root, low: a sine, two saws behind a dark filter (TONE), its fifth and octave coming and going (FIFTH),
 * a sine an octave under it (SUB), and its level breathing (BREATH) */
static struct { uint32_t p0, p1, p2, p5, p8, ps, l1, l2, l3, lb; int32_t p16, ic1, ic2, a_prev, mn; } dr;
static void drone_render(void)
{
    int32_t lvl = PAR(P_DRONE), root = EROOT, note = (root <= 5 ? 36 : 24) + root, tgt = note * 16;
    int32_t a0 = dr.a_prev, a1 = (lvl * lvl * 9200) >> 14, f5, f8, cut, d, fifth = par[P_FIFTH], sub = par[P_SUB] * 160;
    int32_t br = par[P_BREATH];
    uint32_t i0, i1, i2, i5, i8, is, i;
    tsvf_t c;
    dr.lb += 2900000u + (uint32_t)br * 9000u;   /* BREATH: its level swells and sinks, every ~9 .. ~5 s, deeper */
    if (br) a1 = (a1 * (32768 - ((br * 220 * ((32767 + sin15(dr.lb)) >> 1)) >> 15))) >> 15;
    a1 = a0 + (a1 - a0) / 48 + (a1 > a0) - (a1 < a0);   /* (its level moves over ~0.3 s: no step at switch-on or a new scene) */
    vis_drone = a1;
    {                                           /* MIDI: the drone's root, held while it sounds */
        int32_t want = lvl > 0 && g_svol > 0 ? note + 1 : 0;
        if (want != dr.mn) {
            if (dr.mn) mo(0x82, dr.mn - 1, 64);
            if (want) mo(0x92, want - 1, 72);
            dr.mn = want;
        }
    }
    if (!dr.p16) dr.p16 = tgt;
    d = tgt - dr.p16;                           /* a new root: it slides there */
    dr.p16 += d > 0 ? (d + 15) >> 4 : -((-d + 15) >> 4);
    if (!a0 && !a1) return;
    dr.l1 += 124000u, dr.l2 += 81000u, dr.l3 += 53000u;   /* cycles of 200, 308, 470 s / 2 pi.. slow enough */
    i0 = cents_inc(dr.p16, 0, 0);
    i1 = cents_inc(dr.p16 + 192, 7, 0), i2 = cents_inc(dr.p16 + 192, -7, 0);
    i5 = cents_inc(dr.p16 + 19 * 16, 2, 0), i8 = cents_inc(dr.p16 + 24 * 16, -3, 0);
    is = i0 >> 1;
    f5 = ((9000 + ((sin15(dr.l1 * 5u) * 8000) >> 15)) * fifth) >> 6;
    f8 = ((7000 + ((sin15(dr.l2 * 7u) * 6500) >> 15)) * fifth) >> 6;
    cut = ((30 + par[P_DTONE] * 40 / 127 + PAR(P_BRIGHT) * 10 / 127) << 8) + ((sin15(dr.l3 * 9u) * 6) >> 7);
    tsvf_coef(&c, cut, 30);
    for (i = 0; i < VB; i++) {
        int32_t s, a, y, w;
        dr.p0 += i0, dr.p1 += i1, dr.p2 += i2, dr.p5 += i5, dr.p8 += i8;
        w = tsvf_lp(&c, (osc_saw(dr.p1, i1) + osc_saw(dr.p2, i2)) >> 1, &dr.ic1, &dr.ic2);
        s = (sin15(dr.p0) * 18000 + w * 15000) >> 15;
        if (sub) dr.ps += is, s += (sin15(dr.ps) * sub) >> 15;
        a = a0 + (((a1 - a0) * (int32_t)i) >> 8);
        y = (s * a) >> 15;
        bus_l[i] += y + ((((sin15(dr.p5) * f5) >> 15) * a) >> 15);
        bus_r[i] += y + ((((sin15(dr.p8) * f8) >> 15) * a) >> 15);
        bus_rev[i] += (y * 90) >> 8;
    }
    dr.a_prev = a1;
}

/* the wind: noise through a band that wanders */
static struct { uint32_t l1, l2, l3; int32_t ic1, ic2, a_prev; } wn;
static void wind_render(void)
{
    int32_t lvl = PAR(P_WIND), a0 = wn.a_prev, a1, gl, gr, pan;
    uint32_t i;
    tsvf_t c;
    wn.l1 += 1083000u, wn.l2 += 1468000u, wn.l3 += 803000u;   /* cycles of 23, 17, 31 s */
    a1 = (((lvl * lvl) >> 2) * (11000 + ((sin15(wn.l2) + sin15(wn.l2 * 3u + wn.l1)) * 5000 >> 15))) >> 15;
    if (a1 < 0) a1 = 0;
    a1 = a0 + (a1 - a0) / 48 + (a1 > a0) - (a1 < a0);
    if (!a0 && !a1) return;
    tsvf_coef_k(&c, (72 << 8) + ((sin15(wn.l1) * 19) >> 7) + ((sin15(wn.l1 * 5u) * 5) >> 7) + PAR(P_BRIGHT) * 20, 2000);
    pan = 128 + ((sin15(wn.l3) * 90) >> 15);
    gl = SINE[256 - pan], gr = SINE[pan];
    for (i = 0; i < VB; i++) {
        int32_t x = (int32_t)(arnd() >> 16) - 32768, a = a0 + (((a1 - a0) * (int32_t)i) >> 8);
        int32_t y = (tsvf_any(&c, 2000, FT_BP, x, &wn.ic1, &wn.ic2) * a) >> 15;
        bus_l[i] += (y * gl) >> 15;
        bus_r[i] += (y * gr) >> 15;
        bus_rev[i] += (y * 110) >> 8;
    }
    wn.a_prev = a1;
}

/* the drums (ARP), made to sit in the haze: 16 steps whose pattern the main loop makes up each time they start; each
 * step plays only as often as its chance says (so no two bars are the same), a little late or early, quieter in calm
 * sections, and every four bars EVOLVE moves a step. A soft low heartbeat (a sine falling slowly, no click), a brushed
 * rim (a band of noise with a small knock), a shaker (noise, eased in), and mallets on the chord and the motif, into
 * the echo. All through a dark filter (BRIGHT) and deep into the reverb. TEMPO 60 .. 140 BPM, SWING the off-beats late */
enum { DK_KICK = 1, DK_SNARE = 2, DK_HAT = 4, DK_OPEN = 8, DK_PERC = 16 };
static volatile uint8_t dpat[16], dacc[16], dprob[16];   /* the main loop's new pattern: what, its accent, its chance (%) */
static volatile uint32_t g_dgen;                /* a new pattern is there (the main loop counts) */
static volatile uint8_t apat[16], aprob[16];    /* the pattern playing (the audio's copy: it evolves) */
static volatile int32_t vis_step = -1;
static volatile uint32_t vis_kick_seq, vis_snare_seq, vis_hat_seq;
static int32_t gn_motif_note(int32_t up);       /* (the machine's, below) */
static int32_t sec_drums = 256;                 /* the section's say in how busy the drums are (0..256) */
static struct {
    uint32_t pos, len, step, bar, gen, kph, sph, pc, pm, pic, pim, pend;
    int32_t on, fade, kamp, kat, kpe, samp, sat, sic1, sic2, hamp, hat_, hdec, hx, hlp, pamp, pidx, lp, lpr, ac, mn;
    uint32_t moff;                              /* MIDI: the drum notes to let go of next block */
    uint8_t pf;                                 /* the step waiting for its moment (humanised) */
} dm;
static void drums_hit(uint32_t f, int32_t a)
{
    int32_t lv = ((par[P_DLEVEL] * dm.fade >> 8) * g_sthin) >> 8;
    if (lv <= 0) return;
    if (f & DK_KICK) mo(0x99, 36, 60 + a / 3), dm.moff |= DK_KICK;
    if (f & DK_SNARE) mo(0x99, 37, 50 + a / 3), dm.moff |= DK_SNARE;
    if (f & DK_HAT) mo(0x99, 70, 30 + a / 3), dm.moff |= DK_HAT;
    if (f & DK_OPEN) mo(0x99, 82, 40 + a / 3), dm.moff |= DK_OPEN;
    if (f & DK_KICK) dm.kamp = (14000 + a * 40) * lv / 100, dm.kat = 0, dm.kpe = 32767, dm.kph = 0x40000000u, vis_kick_seq = vis_kick_seq + 1u;
    if (f & DK_SNARE) dm.samp = (5000 + a * 36) * lv / 100, dm.sat = 0, dm.sph = 0, vis_snare_seq = vis_snare_seq + 1u;
    if (f & (DK_HAT | DK_OPEN)) dm.hamp = (1400 + a * 22 + (int32_t)arnd_n(700)) * lv / 100, dm.hat_ = 0, dm.hdec = f & DK_OPEN ? 12 : 10,
                                vis_hat_seq = vis_hat_seq + 1u;
    if (f & DK_PERC) {
        int32_t note = gn_motif_note(1);
        while (note > 91) note -= 12;
        dm.pic = pitch_inc((uint32_t)clamp(note, 55, 91) * 16u), dm.pim = dm.pic;
        dm.pamp = (2600 + a * 22) * lv / 100, dm.pidx = 9000;
        if (dm.mn) mo(0x83, dm.mn - 1, 64);
        mo(0x93, note, 50 + a / 3), dm.mn = note + 1;
    }
}
static void drums_step(void)                    /* a new step: what plays of it, and when */
{
    uint32_t s = dm.step, f;
    if (s == 0u && g_dgen != dm.gen) {          /* a new pattern while they play: from this downbeat, no easing in */
        uint32_t k;
        dm.gen = g_dgen;
        for (k = 0; k < 16u; k++) apat[k] = dpat[k], aprob[k] = dprob[k];
    }
    f = apat[s];
    int32_t pr = aprob[s], keep = sec_drums;
    if (s == 0u && (dm.bar & 3u) == 0u && dm.on) {   /* every four bars EVOLVE moves a step (not the downbeat) */
        int32_t ev = par[P_EVOLVE];
        if ((int32_t)arnd_n(127) < ev) {
            uint32_t j = 1u + arnd_n(15), what = arnd_n(4);
            static const uint8_t KIND[4] = {DK_HAT, DK_PERC, DK_KICK, DK_SNARE};
            if (apat[j] & KIND[what]) apat[j] = (uint8_t)(apat[j] & ~KIND[what]);
            else apat[j] = (uint8_t)(apat[j] | KIND[what]), aprob[j] = (uint8_t)(30 + arnd_n(50));
        }
    }
    if (!f) return;
    if (!(s == 0u || pr >= 100)) {              /* chance, the section's mood scaling everything but the core */
        if ((int32_t)arnd_n(100) >= (pr * keep) >> 8) return;
    }
    if ((f & DK_KICK) && keep < 140 && s != 0u) f &= ~DK_KICK;   /* (calm: the heartbeat on the one only) */
    if ((f & DK_SNARE) && keep < 100) f &= ~DK_SNARE;
    dm.pf = (uint8_t)f;
    dm.ac = 40 + (int32_t)arnd_n(60) + (s % 4u == 0u ? 25 : 0);
    dm.pend = s == 0u || s == 8u ? 1u : 1u + arnd_n(150);   /* a few ms late, but the beats */
}
static void drums_render(void)
{
    uint32_t i, bpm = 60u + (uint32_t)par[P_TEMPO] * 80u / 127u, base = 661500u / bpm, sw = (uint32_t)par[P_SWING];
    int32_t klp = 4000 + par[P_DKTONE] * 200;   /* TONE: the dark filter over them all */
    tsvf_t c;
    if (dm.moff) {                              /* (MIDI: last block's drums let go) */
        if (dm.moff & DK_KICK) mo(0x89, 36, 64);
        if (dm.moff & DK_SNARE) mo(0x89, 37, 64);
        if (dm.moff & DK_HAT) mo(0x89, 70, 64);
        if (dm.moff & DK_OPEN) mo(0x89, 82, 64);
        dm.moff = 0;
    }
    if (dm.mn && !dm.pamp) mo(0x83, dm.mn - 1, 64), dm.mn = 0;
    if (g_dgen != dm.gen && !dm.on) {           /* a new pattern before they start: take it (they ease in) */
        uint32_t k;
        dm.gen = g_dgen;
        for (k = 0; k < 16u; k++) apat[k] = dpat[k], aprob[k] = dprob[k];
    }
    if (g_drums && !dm.on) dm.on = 1, dm.step = 15, dm.pos = 0, dm.len = 1, dm.bar = 0, dm.fade = 40;
    if (!g_drums) dm.on = 0, dm.pend = 0, vis_step = -1;
    if (!dm.on && !dm.kamp && !dm.samp && !dm.hamp && !dm.pamp) {   /* (silent: nothing to do; the filter's last crumbs go) */
        if (dm.lp > -8 && dm.lp < 8 && dm.lpr > -8 && dm.lpr < 8) { dm.lp = dm.lpr = 0; return; }
    }
    tsvf_coef_k(&c, 84 << 8, 2400);             /* the rim's band, ~1 kHz */
    for (i = 0; i < VB; i++) {
        int32_t y = 0, rv = 0, dly = 0, hr = 0, n;
        if (dm.on && ++dm.pos >= dm.len) {      /* the next step; SWING: the off-beats come late, the on-beats early */
            dm.pos = 0, dm.step = (dm.step + 1u) & 15u;
            if (dm.step == 0u) dm.bar++;
            if (dm.fade < 256) dm.fade += 14;
            if (dm.fade > 256) dm.fade = 256;
            dm.len = dm.step & 1u ? base - base * sw / 400u : base + base * sw / 400u;
            vis_step = (int32_t)dm.step;
            drums_step();
        }
        if (dm.pend && --dm.pend == 0u) drums_hit(dm.pf, dm.ac);
        if (dm.kamp) {                          /* the heartbeat: ~90 Hz falling to ~40, eased in over 3 ms */
            uint32_t f = 40u + ((50u * (uint32_t)dm.kpe) >> 15);
            int32_t a = dm.kat < 128 ? (dm.kamp * dm.kat) >> 7 : dm.kamp;
            dm.kat++;
            dm.kph += f * 97391u;
            n = (sin15(dm.kph) * a) >> 15;
            y += n, rv += n >> 2;
            dm.kpe -= (dm.kpe >> 9) + (dm.kpe > 0);
            dm.kamp -= (dm.kamp >> 13) + (dm.kamp > 0);
        }
        if (dm.samp) {                          /* the rim: a brushed band of noise, eased in, and a small knock */
            int32_t x = (int32_t)(arnd() >> 17) - 16384, a = dm.sat < 260 ? (dm.samp * dm.sat) / 260 : dm.samp;
            dm.sat++;
            dm.sph += 820u * 97391u;
            n = ((tsvf_any(&c, 2400, FT_BP, x, &dm.sic1, &dm.sic2) + (dm.sat < 600 ? sin15(dm.sph) >> 3 : 0)) * a) >> 15;
            y += n >> 1, rv += n + (n >> 1);
            dm.samp -= (dm.samp >> 11) + (dm.samp > 0);
        }
        if (dm.hamp) {                          /* the shaker: bright noise, eased in, to the right */
            int32_t x = (int32_t)(arnd() >> 17) - 16384, h = x - dm.hx, a = dm.hat_ < 130 ? (dm.hamp * dm.hat_) / 130 : dm.hamp;
            dm.hat_++;
            dm.hx = x;
            dm.hlp += (h - dm.hlp) >> 1;
            n = (dm.hlp * a) >> 15;
            hr += n, rv += n >> 1;
            dm.hamp -= (dm.hamp >> dm.hdec) + (dm.hamp > 0);
        }
        if (dm.pamp) {                          /* the mallet: a sine with a little bend, into the echo */
            dm.pc += dm.pic, dm.pm += dm.pim;
            n = (sin15(dm.pc + (uint32_t)(sin15(dm.pm) * dm.pidx)) * dm.pamp) >> 15;
            rv += n, dly += n + (n >> 1);
            bus_l[i] += (n * 3) >> 2, bus_r[i] += n >> 2;
            dm.pamp -= (dm.pamp >> 12) + (dm.pamp > 0);
            dm.pidx -= dm.pidx >> 9;
        }
        dm.lp += ((y - dm.lp) * klp) >> 15;     /* (one pole: the dark filter) */
        dm.lpr += ((hr - dm.lpr) * klp) >> 15;
        bus_l[i] += dm.lp + (dm.lpr >> 2), bus_r[i] += dm.lp + dm.lpr, bus_rev[i] += rv, bus_dly[i] += dly;
    }
}

/* the lake (the NIGHT page): water lapping at the shore (noise through a low band that opens as each lap rises, one
 * stream to each side), crickets (chirps of three or four pulses of a high sine, two of them, coming and going over
 * minutes) and, now and then, a loon: two or three long tones on the scale, gliding between them, a slow vibrato,
 * far off to one side and mostly reverb and echo. LAKE: the water and the crickets; LOONS: how often a loon calls */
static volatile uint32_t vis_loon_seq;
static volatile int32_t vis_loon_x;
static struct {
    int32_t le[2], pk[2], up[2], t[2], ic1[2], ic2[2];   /* the laps: level, peak, rising, blocks to the next */
    uint32_t cp[2], cinc[2];                    /* the crickets: carrier phase and step */
    int32_t cpos[2], cn[2], ct[2], ca[2];       /* sample in the chirp, pulses in it, blocks to the next, loudness */
    uint32_t cl;                                /* their slow coming and going */
    int32_t lt, lstage, lseg, lnseg, lblk, lamp, la_prev, lp16, lp0, lp1, lpan, started;   /* the loon */
    uint32_t lph, lvib;
    int32_t lnote[3];
} lk = {{0, 0}, {0, 0}, {0, 0}, {40, 90}};
static void loon_start(void)
{
    const scale_t *s = &SCALES[g_scale];
    int32_t base = 72 + EROOT + 12 * g_oct, d = (int32_t)arnd_n(3), k;
    while (base > 79) base -= 12;
    lk.lnseg = 2 + (int32_t)arnd_n(2);
    for (k = 0; k < lk.lnseg; k++) {            /* low, then up a few steps (the wail), maybe back down */
        int32_t deg = k == 0 ? d : k == 1 ? d + 2 + (int32_t)arnd_n(3) : d + (int32_t)arnd_n(2);
        lk.lnote[k] = (base + scale_note(s, deg)) * 16;
    }
    lk.lseg = 0, lk.lstage = 1, lk.lblk = 0, lk.lp16 = lk.lnote[0] - 32, lk.lp0 = lk.lp16, lk.lp1 = lk.lnote[0];
    lk.lpan = arnd_n(2) ? 30 + (int32_t)arnd_n(40) : 186 + (int32_t)arnd_n(40);
    vis_loon_x = 20 + lk.lpan * 200 / 256;
    vis_loon_seq = vis_loon_seq + 1u;
}
static void lake_render(void)
{
    int32_t lake = PAR(P_LAKE), loons = par[P_LOONS], s, active = 0;
    uint32_t i;
    tsvf_t c[2];
    if (!lk.started) lk.started = 1, lk.lt = BLK(12000);   /* (the first call a little after switch-on) */
    if (loons > 0 && par[P_LVOL] > 0 && --lk.lt <= 0) {   /* CALLS: a call every ~70 s .. ~13 s */
        int32_t mean = 70000 - loons * 450;
        lk.lt = BLK(mean / 2 + (int32_t)arnd_n((uint32_t)mean));
        if (!lk.lstage) loon_start();
    }
    if (lake > 0) {
        lk.cl += 3500u;
        for (s = 0; s < 2; s++) {               /* the laps: one every ~1.5 .. 4 s each side */
            if (--lk.t[s] <= 0) {               /* WAVES: calm (a lap every ~4 s, small) .. rough (~1 s, big) */
                int32_t wv = par[P_WAVES];
                lk.t[s] = BLK(3600 - wv * 20 + (int32_t)arnd_n((uint32_t)(3000 - wv * 15)));
                lk.pk[s] = (6000 + (int32_t)arnd_n(14000)) * (40 + wv) / 90, lk.up[s] = 1;
            }
            if (lk.up[s]) { lk.le[s] += (lk.pk[s] - lk.le[s]) / 18; if (lk.le[s] >= lk.pk[s] - 300) lk.up[s] = 0; }
            else lk.le[s] -= lk.le[s] / 80 + (lk.le[s] > 0);
            tsvf_coef_k(&c[s], ((54 + lk.le[s] / 1200) << 8), 5000);
            if (--lk.ct[s] <= 0) {              /* a chirp */
                lk.ct[s] = BLK(450 + (int32_t)arnd_n(700)), lk.cpos[s] = 0, lk.cn[s] = 3 + (int32_t)arnd_n(2);
                lk.ca[s] = (int32_t)((32767 + sin15(lk.cl * (s ? 3u : 2u) + (uint32_t)s * 0x60000000u)) >> 1) * lake / 127;
                lk.cinc[s] = s ? 459000000u : 419000000u;   /* ~4.7, ~4.3 kHz */
            }
        }
        active = 1;
    }
    if (lk.lstage) {                            /* the loon's call: glide to each tone, hold it, then let it fade */
        int32_t segb = BLK(900), glide = BLK(320), a;
        lk.lblk++;
        if (lk.lblk < glide) lk.lp16 = lk.lp0 + (lk.lp1 - lk.lp0) * lk.lblk / glide;
        else lk.lp16 = lk.lp1;
        if (lk.lblk >= segb + (lk.lseg == lk.lnseg - 1 ? BLK(500) : 0)) {
            if (++lk.lseg >= lk.lnseg) lk.lstage = 2;
            else lk.lblk = 0, lk.lp0 = lk.lp16, lk.lp1 = lk.lnote[lk.lseg];
        }
        a = lk.lstage == 2 ? 0 : (lk.lblk < glide && lk.lseg ? 5000 : 7000) * par[P_LVOL] / 70;   /* (MIX: LOONS) */
        lk.lamp += (a - lk.lamp) / (lk.lstage == 2 ? 60 : 30);
        if (lk.lstage == 2 && lk.lamp < 20 && lk.la_prev < 20) lk.lstage = 0, lk.lamp = 0;
        lk.lvib += 104000000u;                  /* ~4.2 Hz */
        active = 1;
    }
    if (!active && !lk.la_prev) return;
    {
        uint32_t linc = lk.lstage || lk.la_prev ? cents_inc(lk.lp16, (sin15(lk.lvib) * 14) >> 15, 0) : 0;
        int32_t a0 = lk.la_prev, a1 = lk.lstage ? lk.lamp : 0, lgl = SINE[256 - lk.lpan], lgr = SINE[lk.lpan];
        int32_t w0 = lake > 0 ? (lk.le[0] * lake) >> 7 : 0, w1 = lake > 0 ? (lk.le[1] * lake) >> 7 : 0;
        for (i = 0; i < VB; i++) {
            int32_t l = 0, r = 0, rv = 0, dly = 0;
            if (lake > 0) {
                int32_t x0 = (int32_t)(arnd() >> 17) - 16384, x1 = (int32_t)(arnd() >> 17) - 16384, y0, y1;
                y0 = (tsvf_lp(&c[0], x0, &lk.ic1[0], &lk.ic2[0]) * w0) >> 15;
                y1 = (tsvf_lp(&c[1], x1, &lk.ic1[1], &lk.ic2[1]) * w1) >> 15;
                l += y0 + (y1 >> 2), r += y1 + (y0 >> 2), rv += (y0 + y1) >> 2;
                for (s = 0; s < 2; s++)         /* a pulse: 12 ms on, 18 ms off, eased */
                    if (lk.cn[s] > 0) {
                        int32_t pp = lk.cpos[s]++, e;
                        if (pp < 530) {
                            e = pp < 265 ? pp : 530 - pp;
                            lk.cp[s] += lk.cinc[s];
                            e = (((sin15(lk.cp[s]) * e) >> 8) * lk.ca[s]) >> 15;
                            e = (e * 12) >> 8;
                            if (s) r += e, l += e >> 2; else l += e, r += e >> 2;
                            rv += e >> 2;
                        }
                        if (lk.cpos[s] >= 1323) lk.cpos[s] = 0, lk.cn[s]--;
                    }
            }
            if (a0 | a1) {
                int32_t a = a0 + (((a1 - a0) * (int32_t)i) >> 8), y;
                lk.lph += linc;
                y = (((sin15(lk.lph) + (sin15(lk.lph << 1) >> 3)) * a) >> 15);
                l += (y * lgl) >> 16, r += (y * lgr) >> 16, rv += y, dly += y >> 1;
            }
            bus_l[i] += l, bus_r[i] += r, bus_rev[i] += rv, bus_dly[i] += dly;
        }
        lk.la_prev = a1;
    }
}

/* the echo: two lines, each feeding the other (left, then right, then left), darker each time round */
#define DL_N 32768u
#define DL_L 22050u                             /* 0.5 s */
#define DL_R 16538u                             /* 0.375 s */
static int16_t dl_l[DL_N] __attribute__((section(".pool")));
static int16_t dl_r[DL_N] __attribute__((section(".pool")));
static uint32_t dl_w;
static int32_t dl_fl, dl_fr;
static int32_t dl_len = DL_L;                   /* TIME: the left line's length now (it glides, like tape) */
static void echo_render(void)
{
    int32_t echo = PAR(P_ECHO), fb = 9000 + echo * 12500 / 127, ret = 9000 + echo * 15000 / 127;
    int32_t want = 7000 + par[P_ETIME] * 25000 / 127, tc = 3000 + par[P_ETONE] * 26000 / 127;   /* ~0.16 .. 0.73 s; dark .. bright */
    uint32_t w = dl_w, i, ll, lr;
    int32_t fl = dl_fl, fr = dl_fr;
    dl_len += (want - dl_len) / 64 + (want > dl_len) - (want < dl_len);
    ll = (uint32_t)dl_len, lr = (uint32_t)(dl_len * 3 / 4);
    for (i = 0; i < VB; i++) {
        int32_t ol = dl_l[(w - ll) & (DL_N - 1u)], or_ = dl_r[(w - lr) & (DL_N - 1u)];
        fl += ((ol - fl) * tc) >> 15;           /* (TONE: darker each time round, or not) */
        fr += ((or_ - fr) * tc) >> 15;
        dl_l[w] = (int16_t)clamp(bus_dly[i] + ((fr * fb) >> 15), -32767, 32767);
        dl_r[w] = (int16_t)clamp((fl * fb) >> 15, -32767, 32767);
        w = (w + 1u) & (DL_N - 1u);
        bus_l[i] += (ol * ret) >> 15;
        bus_r[i] += (or_ * ret) >> 15;
        bus_rev[i] += ((ol + or_) * 60) >> 8;
    }
    dl_w = w, dl_fl = fl, dl_fr = fr;
}

/* the machine: what plays the pads and the bells when nobody does. The harmony walks from chord to chord the way
 * chords lead (CHORDS: how often); a motif of three to five notes keeps coming back, on each chord, turned about as
 * EVOLVE allows; a new chord sometimes blooms as two or three pads at once; and the piece goes through sections (calm,
 * bloom, night, sparkle, drift), each leaning the knobs a little its way, and now and then (EVOLVE high) to a new key */
static struct {
    int32_t t_pad, t_bell, t_chord, deg, chord;
    int32_t t_next, next_note, casc, casc_deg;  /* a bell's phrase under way; a fall of bells */
    int32_t intro;                              /* the first notes after switch-on come quickly */
    int32_t mot[5], mlen, mi, bloom;            /* the motif (degrees over the chord), where it is; pads to bloom */
    int32_t phrase, ph_i, ph_dir;               /* a bell phrase: notes left, where in the motif, which way */
    int32_t t_sec, mood, tgt[NPARAM];           /* the section: blocks left, which, where it leans the knobs */
    uint32_t wph;                               /* the slow wander under the sections */
} gn = {30, 700, 5000, 2, 0, 0, 0, 0, 0, 3, {0, 2, 1, 4, 0}, 4, 0, 0, 0, 0, 1, BLK(30000), 0, {0}, 0};

static int32_t pad_base(void) { return 48 + EROOT + 12 * (g_oct + SCENES[g_scene].reg); }
static int32_t gn_motif_note(int32_t up)        /* the motif's next note on the chord (for the drums' mallets: up an
                                                 * octave or two) */
{
    const scale_t *s = &SCALES[g_scale];
    int32_t d = gn.chord + gn.mot[gn.mi % gn.mlen], note;
    gn.mi = (gn.mi + 1) % gn.mlen;
    if (d < 0) d += (int32_t)s->n;
    note = pad_base() + 12 * up + scale_note(s, d);
    return note;
}
static void motif_turn(void)                    /* EVOLVE: the motif changes a little: a note moves, it turns over, or
                                                 * (rarely) a new one */
{
    int32_t r = (int32_t)arnd_n(100), k;
    if (r < 50) {
        k = (int32_t)arnd_n((uint32_t)gn.mlen);
        gn.mot[k] = clamp(gn.mot[k] + ((int32_t)arnd_n(5) - 2), -2, 6);
    } else if (r < 70) {
        for (k = 0; k < gn.mlen; k++) gn.mot[k] = clamp(4 - gn.mot[k], -2, 6);   /* upside down */
    } else if (r < 85) {
        int32_t t = gn.mot[0];
        for (k = 0; k + 1 < gn.mlen; k++) gn.mot[k] = gn.mot[k + 1];
        gn.mot[gn.mlen - 1] = t;
    } else {
        int32_t d = (int32_t)arnd_n(3);
        gn.mlen = 3 + (int32_t)arnd_n(3);
        for (k = 0; k < gn.mlen; k++) d = clamp(d + (int32_t)arnd_n(5) - 2, -1, 6), gn.mot[k] = d;
    }
}
/* the sections: how each leans DENSITY, BRIGHT, SPACE, DRIFT, ECHO, BELLS, WIND, DRONE, VOICES; and the drums */
enum { MOOD_HOME, MOOD_CALM, MOOD_BLOOM, MOOD_NIGHT, MOOD_SPARKLE, MOOD_DRIFT, NMOOD };
static const int8_t MOOD_LEAN[NMOOD][9] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0},
    {-28, -16, 10, -10, 0, -18, 0, 16, -40},
    {22, 18, 0, 6, 10, 14, -10, 0, 30},
    {-12, -26, 18, 0, 6, -24, 28, 22, -20},
    {-14, 12, 6, 0, 22, 36, -6, -10, -10},
    {0, -6, 12, 34, 8, 0, 14, 6, 0},
};
static const uint8_t MOOD_P[9] = {P_DENSITY, P_BRIGHT, P_SPACE, P_DRIFT, P_ECHO, P_BELLS, P_WIND, P_DRONE, P_VOICES};
static const int16_t MOOD_DRUMS[NMOOD] = {256, 120, 256, 70, 200, 170};
static void gen_section(void)
{
    int32_t ev = par[P_EVOLVE], k, mood;
    gn.t_sec = BLK((40000 + (int32_t)arnd_n(80000)) * (170 - ev) / 100);   /* ~1 .. ~3 min at 64; quicker turned up */
    for (k = 0; k < NPARAM; k++) gn.tgt[k] = 0;
    if (ev <= 0) { gn.mood = MOOD_HOME, sec_drums = 256; return; }
    do mood = (int32_t)arnd_n(NMOOD); while (mood == gn.mood);
    gn.mood = mood;
    for (k = 0; k < 9; k++) gn.tgt[MOOD_P[k]] = MOOD_LEAN[mood][k] * (32 + ev) / 159;
    sec_drums = MOOD_DRUMS[mood];
    if ((int32_t)arnd_n(100) < ev / 2) motif_turn();
    if (ev > 70 && (int32_t)arnd_n(100) < (ev - 70)) {   /* a new key: up a fifth or a fourth, or home again */
        if (g_shift && arnd_n(2)) g_shift = 0;
        else g_shift = (g_shift + (arnd_n(2) ? 7 : 5)) % 12;
    }
    vis_mood = mood;
}
static void gen_lean(void)                      /* once a block: the knobs ease towards the section's lean, with a slow
                                                 * wander under it (EVOLVE) */
{
    int32_t ev = par[P_EVOLVE], k;
    gn.wph += 3100u;                            /* (a cycle ~2.3 h; its harmonics below, minutes) */
    for (k = 0; k < 9; k++) {
        int32_t p = MOOD_P[k], w = (sin15(gn.wph * (uint32_t)(23 + k * 11) + (uint32_t)k * 0x1C000000u) * ev) >> 19;
        int32_t want = (gn.tgt[p] + w) << 8;
        md[p] += (want - md[p]) >> 10;          /* (~6 s to get most of the way) */
    }
}

static void gen_pad(void)
{
    const scale_t *s = &SCALES[g_scale];
    static const int8_t STEP[8] = {-2, -1, -1, 1, 1, 2, 3, -3};
    static const int8_t TONE[4] = {0, 2, 4, 6};
    int32_t dens = (PAR(P_DENSITY) * g_sthin) >> 8, n = (int32_t)s->n, top = range_top(s), note = 0;
    int32_t most = 1 + ((PAR(P_VOICES) * 5 / 127) * g_sthin >> 8);
    uint32_t k, t, sounding = 0;
    pv_t *v;
    for (k = 0; k < NV; k++) sounding += pv[k].gate && pv[k].id < 0;
    if (((int32_t)sounding >= most && !gn.bloom) || !(v = pad_free())) return;
    for (t = 0; t < 5u; t++) {
        int32_t deg, dup = 0, r = (int32_t)arnd_n(100);
        if (gn.bloom || r < 42) {               /* a tone of the chord (its seventh now and then) */
            deg = gn.chord + TONE[arnd_n(arnd_n(127) < (uint32_t)par[P_EVOLVE] / 2u ? 4u : 3u)] + n * (int32_t)arnd_n(2);
        } else if (r < 72) {                    /* the motif's next note, on this chord */
            deg = gn.chord + gn.mot[gn.mi % gn.mlen];
            gn.mi = (gn.mi + 1) % gn.mlen;
            if (deg < 0) deg += n;
        } else {                                /* a step or two from the last */
            gn.deg += STEP[arnd_n(8)];
            if (gn.deg < 0) gn.deg = -gn.deg;
            if (gn.deg > top) gn.deg = 2 * top - gn.deg;
            deg = gn.deg;
        }
        if (deg > top) deg -= n;
        note = pad_base() + scale_note(s, deg < 0 ? 0 : deg);
        while (note > 88) note -= 12;
        for (k = 0; k < NV; k++) dup |= (pv[k].gate || pv[k].env) && pv[k].note == note;
        if (!dup) break;
    }
    if (gn.bloom > 0) gn.bloom--;
    pad_start(v, note, -1, BLK(((4200 + (int32_t)arnd_n(6500) + (127 - dens) * 30) * len256_of()) >> 8), atk10_of());
}
static void gen_bell(void)                      /* a bell, or a phrase of the motif: up or down it, an octave up */
{
    const scale_t *s = &SCALES[g_scale];
    int32_t base = 72 + EROOT + 12 * g_oct, deg;
    if (arnd_n(100) < 55u) {
        gn.phrase = 1 + (int32_t)arnd_n((uint32_t)gn.mlen), gn.ph_i = (int32_t)arnd_n((uint32_t)gn.mlen), gn.ph_dir = arnd_n(3) ? 1 : -1;
        deg = gn.chord + gn.mot[gn.ph_i];
        if (gn.phrase > 1) gn.t_next = BLK(260 + (int32_t)arnd_n(480)), gn.next_note = 1;
        gn.phrase--;
    } else {
        deg = gn.chord + (int32_t)arnd_n((uint32_t)(2 * s->n + 1)) - (int32_t)s->n / 2;
    }
    bell_strike(base + scale_note(s, deg < 0 ? deg + (int32_t)s->n : deg), 50 + (int32_t)arnd_n(70), 0);
}
static void gen_tick(void)                      /* once a block */
{
    int32_t dens = (PAR(P_DENSITY) * g_sthin) >> 8, bells = (PAR(P_BELLS) * g_sthin) >> 8, base, mean;
    while (ev_r != ev_w) {                      /* the keys and MIDI first */
        uint32_t e = evq[ev_r % 32u], id = e >> 16, k;
        int32_t note = (int32_t)(e >> 8 & 127u);
        ev_r = ev_r + 1u;
        if (e & 4u) {                           /* (the choir) */
            if (e & 1u) choir_on(note, (int32_t)id, 0, 256, 7, 1);
            else choir_off((int32_t)id);
        } else if (e & 2u) {
            if (e & 1u) bell_strike(note, 100, 2);
        } else if (e & 1u) {
            pad_start(pad_take(), note, (int32_t)id, 0, 1 + par[P_KATK] * 30 / 127);   /* KEYS: 0.1 .. 3 s */
        } else {
            for (k = 0; k < NV; k++)
                if (pv[k].gate && pv[k].id == (int32_t)id) pv[k].gate = 0;
        }
    }
    if (--gn.t_sec <= 0) gen_section();
    gen_lean();
    if (gn.t_next > 0 && --gn.t_next == 0 && gn.next_note) {   /* the bell phrase goes on */
        const scale_t *s = &SCALES[g_scale];
        int32_t deg;
        gn.ph_i = ((gn.ph_i + gn.ph_dir) % gn.mlen + gn.mlen) % gn.mlen;
        deg = gn.chord + gn.mot[gn.ph_i];
        bell_strike(72 + EROOT + 12 * g_oct + scale_note(s, deg < 0 ? deg + (int32_t)s->n : deg), 36 + (int32_t)arnd_n(50), 0);
        if (gn.phrase-- > 0) gn.t_next = BLK(240 + (int32_t)arnd_n(520));
        else gn.next_note = 0;
    }
    if (!g_run) return;
    if (--gn.t_pad <= 0) {                      /* DENSITY: a new pad note every ~14 s .. ~0.9 s */
        base = 14000 >> (dens >> 5);
        mean = base - ((base >> 1) * (dens & 31) >> 5);
        gn.t_pad = BLK(mean / 2 + (int32_t)arnd_n((uint32_t)mean));
        if (gn.intro > 0) gn.intro--, gn.t_pad = BLK(700 + (int32_t)arnd_n(900));
        if (gn.bloom > 0) gn.t_pad = BLK(300 + (int32_t)arnd_n(900));
        gen_pad();
    }
    if (--gn.t_bell <= 0) {                     /* BELLS: off, or one (or a phrase) every ~12 s .. ~0.9 s */
        mean = 12000 - bells * 86 - dens * 6;
        gn.t_bell = BLK(mean / 2 + (int32_t)arnd_n((uint32_t)mean));
        if (bells > 0 && !gn.next_note) gen_bell();
    }
    if (--gn.t_chord <= 0) {                    /* CHORDS: the harmony moves on, the way chords lead */
        static const int8_t NEXT[7][4] = {{3, 4, 5, 1}, {4, 6, 3, 5}, {5, 3, 0, 4}, {0, 4, 1, 6}, {0, 5, 3, 2}, {3, 1, 4, 0}, {0, 4, 2, 5}};
        int32_t mean_c = 60000 - par[P_CHORDS] * 52000 / 127, n = (int32_t)SCALES[g_scale].n;   /* every ~60 s .. ~8 s */
        gn.t_chord = BLK(mean_c / 2 + (int32_t)arnd_n((uint32_t)mean_c));
        gn.chord = NEXT[gn.chord % 7][arnd_n(4)] % n;
        if ((int32_t)arnd_n(160) < par[P_EVOLVE]) gn.bloom = 2 + (int32_t)arnd_n(2), gn.t_pad = BLK(200);   /* a bloom */
        vis_chord = gn.chord;
        vis_chord_seq = vis_chord_seq + 1u;
        if (g_sthin > 128 && (int32_t)arnd_n(254) < par[P_MCHOIR] * (gn.mood == MOOD_NIGHT || gn.mood == MOOD_CALM ? 2 : 1)) {
            const scale_t *sc = &SCALES[g_scale];   /* CHOIR: the machine's choir swells in on the new chord, softly */
            int32_t nn = 2 + (int32_t)arnd_n(2), j, hold = BLK(7000 + (int32_t)arnd_n(8000));
            static const int8_t CT[3] = {0, 2, 4};
            for (j = 0; j < nn; j++)
                choir_on(pad_base() - 12 + scale_note(sc, gn.chord + CT[j]) + (j == 2 ? 12 : 0), 200 + j, hold - j * BLK(600), 150, 25, 0);
        }
    }
}

static void out_usb16(const int32_t *o, int32_t *u, uint32_t n)   /* the output's 24-bit words as 16 bit (USB audio) */
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t v = (o[i] + 64) >> 7;
        u[i] = v > 32767 ? 32767 : v < -32767 ? -32767 : v;
    }
}

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i, k;
    int32_t m = clamp(fm1_master(), 0, 4092), pk = 0, space = PAR(P_SPACE), sv = g_svol;
    if (n != VB) return;
    a_blk++;
    for (i = 0; i < VB; i++) bus_l[i] = bus_r[i] = bus_rev[i] = bus_dly[i] = 0;
    kb_update();
    gen_tick();
    for (k = 0; k < NV; k++) pad_render(&pv[k], k);
    for (k = 0; k < NB; k++) bell_render(&bell[k]);
    for (k = 0; k < NCH; k++) choir_render(&chv[k], k);
    drone_render();
    wind_render();
    lake_render();
    drums_render();
    echo_render();
    rv_size = g_freeze ? RV_INF : 46 + space * 80 / 127;   /* SPACE: ~2 s .. ~30 s; FREEZE: for ever */
    rv_damp = 74 - PAR(P_BRIGHT) * 40 / 127;
    rv_mix = 50 + space * 77 / 127;
    rv_pre = 18;
    hall_render(bus_rev, out, VB);              /* (out comes zeroed: the reverb goes straight in) */
    for (i = 0; i < VB; i++) {                  /* the mix: a soft ceiling, then MASTER, into the DAC's 24-bit words */
        int32_t l = soft_knee(clamp(bus_l[i] + out[2u * i], -400000, 400000), 15500);
        int32_t r = soft_knee(clamp(bus_r[i] + out[2u * i + 1u], -400000, 400000), 15500);
        int32_t a = l < 0 ? -l : l, b = r < 0 ? -r : r;
        if (a > pk) pk = a;
        if (b > pk) pk = b;
        out[2u * i] = ((l * m) >> 5) * sv >> 8;
        out[2u * i + 1u] = ((r * m) >> 5) * sv >> 8;
    }
    vis_out = pk;
    {                                           /* the MIX pages' columns: each part's level now */
        int32_t pads = 0, bl = 0, ch = 0;
        for (k = 0; k < NV; k++) pads += vis_lvl[k];
        for (k = 0; k < NB; k++) bl += bell[k].amp;
        for (k = 0; k < NCH; k++) ch += vis_ch_lvl[k];
        vis_mix[0] = clamp(pads / 3, 0, 255), vis_mix[1] = clamp(bl / 60, 0, 255), vis_mix[2] = clamp(ch / 2, 0, 255);
        vis_mix[3] = clamp(dr.a_prev / 36, 0, 255), vis_mix[4] = clamp(wn.a_prev / 30, 0, 255);
        vis_mix[5] = clamp(((lk.le[0] + lk.le[1]) * PAR(P_LAKE)) >> 14, 0, 255), vis_mix[6] = clamp(lk.la_prev / 28, 0, 255);
        vis_mix[7] = clamp((dm.kamp + dm.samp + dm.hamp * 3) / 90, 0, 255);
    }
}

/* ------------------------------------------------------------------ the sky (the main loop) */
#define HZ 168                                  /* the horizon's row: sky above, water from here down */
#define STRIP 24u                               /* a frame goes to the screen in strips of this many rows */
#define NSTRIP (240u / STRIP)
#define FRAME_MS 50u                            /* a new frame at most this often */
#define NC 3u                                   /* curtains */
#define NSTAR 84u
#define NMET 14u

static int16_t cu_y[NC][240];                   /* each curtain, each column: its lower edge's row, */
static uint8_t cu_i[NC][240];                   /* its light there, */
static uint16_t cu_inv[NC][240];                /* and 64 * 256 over its height in rows */
static uint8_t prof[64];                        /* the light up a curtain: brightest at its edge, fading */
static uint8_t ramp[16][3];                     /* its colour up from the edge */
static uint8_t sky_c[HZ][3];                    /* the sky behind, row by row */
static uint8_t tree_h[240];                     /* the far shore: its height over the horizon */
static int32_t pal_now[5][3];                   /* the palette on its way to the scene's (x 256): c0 c1 c2 sky0 sky1 */
static struct { uint8_t x, y, b, sp; uint16_t ph; uint8_t flash; } star[NSTAR];
static struct { int32_t x, y, dx, dy; uint32_t t0, on, life; } met[NMET];
static uint32_t v_t, v_tc, v_last, v_frame_t, v_strip, v_bell_seen, v_boot, v_chord_seen;
static int32_t v_en, v_run_lvl = 256, v_ice, v_flash;   /* loudness; the machine playing (0..256); frozen (0..256);
                                                 * a fall of bells' flare (0..255) */
static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
static const uint8_t ICE[5][3] = {{225, 250, 255}, {150, 215, 255}, {120, 140, 240}, {4, 10, 22}, {30, 54, 80}};

/* the main loop's state the screen shows */
static uint32_t page, kmode, page_t, view;      /* the page open, the keys' mode, when the page opened, the page shown */
static int32_t vis_sleep = -1;                  /* the sleep timer: how far through (0..256), -1 off; 256 asleep */

/* the HUD: what a knob just changed, or a line of news */
enum { HUD_OFF, HUD_KNOB, HUD_MSG };
static uint32_t hud_mode, hud_until, hud_param;
static char hud_l[32], hud_r[20];
static void hud_msg(const char *l, const char *r, uint32_t ms)
{
    char *d = hud_l;
    while (*l && d < hud_l + sizeof hud_l - 1) *d++ = *l++;
    *d = 0;
    d = hud_r;
    while (*r && d < hud_r + sizeof hud_r - 1) *d++ = *r++;
    *d = 0;
    hud_mode = HUD_MSG, hud_until = fm1_ms + ms;
}

static inline void sky_px(int32_t x, int32_t y, int32_t *pr, int32_t *pg, int32_t *pb)   /* the sky's light at x, y */
{
    int32_t r = sky_c[y][0], g = sky_c[y][1], b = sky_c[y][2];
    uint32_t k;
    for (k = 0; k < NC; k++) {
        int32_t in = cu_i[k][x], d, v;
        const uint8_t *c;
        if (!in) continue;
        d = cu_y[k][x] - y;
        if (d >= 0) {                           /* in the curtain: up from its edge */
            int32_t u = (d * cu_inv[k][x]) >> 8;
            if (u > 63) continue;
            v = in * prof[u];
            u = (u >> 2) + (int32_t)k;
            c = ramp[u > 15 ? 15 : u];
        } else if (d > -5) {                    /* just under it: the edge's glow */
            v = in * (5 + d) * 44;
            c = ramp[0];
        } else continue;
        r += (c[0] * v) >> 16, g += (c[1] * v) >> 16, b += (c[2] * v) >> 16;
    }
    *pr = r, *pg = g, *pb = b;
}
static inline uint16_t px_pack(int32_t r, int32_t g, int32_t b, uint32_t dth)   /* to the canvas's 565, dithered */
{
    r += (int32_t)(dth >> 1), g += (int32_t)(dth >> 2), b += (int32_t)(dth >> 1);
    r = r > 255 ? 255 : r, g = g > 255 ? 255 : g, b = b > 255 ? 255 : b;
    return swap16((uint32_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3));
}
static void px_add(int32_t x, int32_t y, uint32_t y0, int32_t r, int32_t g, int32_t b)   /* light added at x, y (screen) */
{
    uint16_t *p, c;
    y -= (int32_t)y0;
    if ((uint32_t)x >= 240u || (uint32_t)y >= STRIP || (r | g | b) <= 0) return;
    p = &cv_px[(uint32_t)y * 240u + (uint32_t)x];
    c = swap16(*p);
    r += (c >> 11) << 3, g += (c >> 5 & 63) << 2, b += (c & 31) << 3;
    r = r > 255 ? 255 : r, g = g > 255 ? 255 : g, b = b > 255 ? 255 : b;
    *p = swap16((uint32_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3));
}
static inline int32_t note_x(int32_t note) { return 20 + (clamp(note, 36, 92) - 36) * 200 / 56; }

static void sky_init(void)
{
    uint32_t k, x;
    int32_t p = 255 << 8;
    for (k = 0; k < 64u; k++) {                 /* e^(-u / 19), and the last rows fade out to nothing */
        prof[k] = (uint8_t)((p >> 8) * (k < 52u ? 12 : 63 - (int32_t)k) / 12);
        p -= p / 19;
    }
    for (x = 0; x < 240u; x++) {                /* the far shore: low hills */
        int32_t h = 5 + ((sin15(x * 23860929u + 0x30000000u) * 3 + sin15(x * 71582788u) * 2) >> 15);
        tree_h[x] = (uint8_t)h;
    }
    for (k = 0; k < 46u; k++) {                 /* and its pines */
        int32_t cx = (int32_t)vrnd_n(240), hh = 4 + (int32_t)vrnd_n(9), w = 2 + (int32_t)vrnd_n(2), dx;
        if (k < 14u) cx = 14 + (int32_t)vrnd_n(60), hh += 3;   /* (a stand of taller ones to the left) */
        for (dx = -hh / w; dx <= hh / w; dx++) {
            int32_t xx = cx + dx, t;
            if ((uint32_t)xx >= 240u) continue;
            t = 5 + hh - (dx < 0 ? -dx : dx) * w;
            if (t > tree_h[xx]) tree_h[xx] = (uint8_t)t;
        }
    }
    for (k = 0; k < NSTAR; k++) {
        uint32_t yy = vrnd_n(HZ - 14u);
        star[k].x = (uint8_t)vrnd_n(240);
        star[k].y = (uint8_t)((yy * yy) / (HZ - 14u) / 2u + yy / 2u);   /* more of them high up */
        star[k].b = (uint8_t)(k < 7u ? 200 + vrnd_n(55) : 40 + vrnd_n(120));
        star[k].sp = (uint8_t)(3 + vrnd_n(14));
        star[k].ph = (uint16_t)vrnd();
    }
    for (k = 0; k < 5u; k++) {
        const scene_t *sc = &SCENES[g_scene];
        const uint8_t *c = k < 3u ? sc->c[k] : sc->sky[k - 3u];
        pal_now[k][0] = c[0] << 8, pal_now[k][1] = c[1] << 8, pal_now[k][2] = c[2] << 8;
    }
}

/* ------------------------------------------------------------------ the pages' light: drawn over the sky, a strip at a
 * time, by adding light (px_add), so the aurora always shows through */
#define NOC 3u                                  /* lines of light across the sky (a curve a column) */
static int16_t oc_y[NOC][240];                  /* the line's row, x 16 */
static uint8_t oc_a[NOC][240];                  /* its light */
static uint8_t oc_col[NOC][3];
static uint32_t oc_n, oc_refl;                  /* how many; which ones the water mirrors (bits) */
#define NDOT 72u
static struct { int16_t x, y; uint8_t rad, a, r, g, b; } dot[NDOT];   /* glowing points */
static uint32_t ndot;
#define NSEG 8u
static struct { int16_t x0, y0, x1, y1; uint8_t a; } seg[NSEG];   /* threads between points */
static uint32_t nseg;
#define NRING 4u
static struct { int32_t r2, band, inv, a; uint8_t c[3]; } drng[NRING];   /* the drone's rings (squared radius, its band) */
static uint32_t nring;
#define RING_CY (HZ + 16)
#define NRIP 10u
static struct { int16_t x; uint8_t on, a; uint32_t t0; } rip[NRIP];   /* ripples on the water */
#define NPART 34u
static struct { int32_t x; uint8_t y, sp; uint16_t ph; } part[NPART];   /* the wind's snow */
#define NFROST 44u
static struct { uint8_t x, y; uint16_t t; } frost[NFROST];   /* frozen: sparks of ice */
static int32_t ov_fade;                         /* the page's light: 0 .. 256 */
static uint32_t ov_page;                        /* the page it shows (it stays while it fades) */
static int32_t glow_horizon;                    /* EFFECTS: the drone's glow along the horizon */
static struct { int16_t h, a; uint8_t c[3]; } mixc[4];   /* MIX: the columns: height, light, colour */
static uint32_t nmix;
static int32_t star_gain = 256;                 /* EFFECTS: the stars brighter with BELLS */

static void dot_add(int32_t x, int32_t y, int32_t rad, int32_t a, const int32_t *c)
{
    if (ndot >= NDOT || a <= 0) return;
    dot[ndot].x = (int16_t)x, dot[ndot].y = (int16_t)y, dot[ndot].rad = (uint8_t)rad, dot[ndot].a = (uint8_t)(a > 255 ? 255 : a);
    dot[ndot].r = (uint8_t)clamp(c[0] >> 8, 0, 255), dot[ndot].g = (uint8_t)clamp(c[1] >> 8, 0, 255);
    dot[ndot].b = (uint8_t)clamp(c[2] >> 8, 0, 255);
    ndot++;
}
static void oc_colour(uint32_t k, uint32_t pal)
{
    oc_col[k][0] = (uint8_t)clamp(pal_now[pal][0] >> 8, 0, 255);
    oc_col[k][1] = (uint8_t)clamp(pal_now[pal][1] >> 8, 0, 255);
    oc_col[k][2] = (uint8_t)clamp(pal_now[pal][2] >> 8, 0, 255);
}
static void ripple(int32_t x, int32_t a, uint32_t t0)
{
    uint32_t k, o = 0;
    for (k = 0; k < NRIP; k++) {
        if (!rip[k].on) { o = k; break; }
        if ((int32_t)(rip[k].t0 - rip[o].t0) < 0) o = k;   /* (all on: the oldest makes way) */
    }
    rip[o].on = 1, rip[o].x = (int16_t)x, rip[o].a = (uint8_t)clamp(a, 0, 255), rip[o].t0 = t0;
}

static void ov_setup(uint32_t now)              /* this frame's light for the page open */
{
    const scale_t *s = &SCALES[g_scale];
    uint32_t k, x, pg = ov_page, t = v_t;
    int32_t f = ov_fade, wpal[3];
    oc_n = 0, oc_refl = 0, ndot = 0, nseg = 0, nring = 0, glow_horizon = 0, star_gain = 256, nmix = 0;
    for (k = 0; k < 3u; k++) wpal[k] = 255 << 8;
    if (f <= 0) return;
    if (pg == PG_ENV) {                         /* the envelope, as a ridge of light: up over ATTACK, along for LENGTH,
                                                 * down over RELEASE; each voice a point riding it */
        int32_t A = atk10_of(), L = (((7450 + (127 - par[P_DENSITY]) * 30) * len256_of()) >> 8) / 100, R = rel10_of(), T, x0 = 14, w = 212;
        if (L < 4) L = 4;
        T = A + L + R;
        oc_n = 1, oc_colour(0, 0);
        for (x = 0; x < 240u; x++) {
            int32_t tt = ((int32_t)x - x0) * T / w, a;
            if ((int32_t)x < x0 || (int32_t)x > x0 + w) { oc_a[0][x] = 0; continue; }
            if (tt < A) a = tt * 256 / A, a = (a * a) >> 8;
            else if (tt < A + L) a = 256;
            else a = 256 - (tt - A - L) * 256 / R, a = a < 0 ? 0 : (a * a) >> 8;
            oc_y[0][x] = (int16_t)((150 << 4) - a * 100 * 16 / 256);
            oc_a[0][x] = (uint8_t)((200 * f) >> 8);
        }
        for (k = 0; k < NV; k++) {
            int32_t tt, a, px, py, lvl = vis_lvl[k];
            if (lvl <= 0 && vis_stage[k] == 2) continue;
            if (vis_stage[k] == 0) tt = vis_env[k] * A / 256;
            else if (vis_stage[k] == 1) tt = A + vis_hf[k] * L / 256;
            else tt = A + L + (256 - vis_env[k]) * R / 256;
            if (vis_env[k] <= 0 && vis_stage[k] == 2) continue;
            px = x0 + tt * w / T;
            a = (vis_env[k] * vis_env[k]) >> 8;
            py = 150 - a * 100 / 256;
            dot_add(px, py, vis_kind[k] ? 5 : 4, (230 * f) >> 8, vis_kind[k] ? wpal : pal_now[1]);
        }
    } else if (pg == PG_EDIT) {                 /* the pads' wave on the horizon: sine to saw (WAVE), its harmonics up to
                                                 * BRIGHT, a peak at the cut (RESO), the octaves over it (SHIMMER), and its
                                                 * twin beating against it (DETUNE) */
        static const int16_t INV_H[13] = {0, 20860, 10430, 6953, 5215, 4172, 3477, 2980, 2607, 2318, 2086, 1896, 1738};
        int32_t wave = par[P_WAVE], H = 2 + par[P_BRIGHT] * 10 / 127, reso = par[P_RESO], sh = par[P_SHIMMER], c, h;
        uint32_t beat = t * (uint32_t)(800 + par[P_DETUNE] * 26000);
        oc_n = 2, oc_refl = 3u, oc_colour(0, 1), oc_colour(1, 2);
        for (c = 0; c < 2; c++)
            for (x = 0; x < 240u; x++) {
                uint32_t ph = x * 26843546u + (c ? beat : 0u) + t * 900000u;
                int32_t saw = 0, sv;
                for (h = 1; h <= H; h++) {
                    int32_t g = INV_H[h];
                    if (h == H) g += (g * reso) >> 5;
                    saw += (sin15(ph * (uint32_t)h) * g) >> 15;
                }
                sv = (sin15(ph) * (128 - wave) + saw * wave) >> 7;
                if (sh) sv += (((sin15(ph * 2u) + (sin15(ph * 4u) >> 1)) >> 2) * sh) >> 7;
                oc_y[c][x] = (int16_t)((112 << 4) - ((sv * 30 * 16) >> 15));
                oc_a[c][x] = (uint8_t)(((c ? 110 : 220) * f) >> 8);
            }
    } else if (pg == PG_LFO) {                  /* three ribbons of the voices' movement, moving at RATE: SWEEP, WIDTH,
                                                 * and TREMOLO pulsing */
        int32_t r = par[P_RATE], speed = 64 + r * r * 960 / 16129, amp[3], tr = par[P_TREMOLO];
        uint32_t ph0 = t * (uint32_t)speed * 2400u;
        amp[0] = 6 + par[P_SWEEP] * 22 / 127, amp[1] = 4 + par[P_WIDTH] * 24 / 127, amp[2] = 10;
        oc_n = 3, oc_colour(0, 0), oc_colour(1, 1), oc_colour(2, 2);
        for (k = 0; k < 3u; k++) {
            int32_t pulse = 256;
            if (k == 2u) pulse = 256 - ((tr * 2 * ((32767 + sin15(ph0 * 5u)) >> 1)) >> 15);
            if (pulse < 40) pulse = 40;
            for (x = 0; x < 240u; x++) {
                uint32_t ph = x * (17895697u + k * 6000000u) + ph0 * (k + 1u) + k * 0x55555555u;
                int32_t yc = 52 + (int32_t)k * 34;
                oc_y[k][x] = (int16_t)((yc << 4) + ((sin15(ph) * amp[k] * 16) >> 15));
                oc_a[k][x] = (uint8_t)((((150 + (sin15(x * 9000000u + ph0) >> 9)) * pulse >> 8) * f) >> 8);
            }
        }
    } else if (pg == PG_GLO) {                  /* the drone: rings rising from under the horizon, the root, its fifth, its
                                                 * octave (FIFTH), a wide one for SUB; breathing with it; TONE their warmth */
        int32_t lvl = vis_drone, base = 40 + par[P_DTONE] * 30 / 127, a, rr[4], w[4], amt[4];
        uint32_t n = 0;
        lvl = clamp(lvl * 256 / 9200, 0, 256);
        a = ((150 + lvl * 2 / 5) * f) >> 8;
        rr[0] = base, rr[1] = base * 3 / 2, rr[2] = base * 2, rr[3] = base * 3;
        w[0] = 5, w[1] = 4, w[2] = 3, w[3] = 6;
        amt[0] = 256, amt[1] = par[P_FIFTH] * 2, amt[2] = par[P_FIFTH] * 3 / 2, amt[3] = par[P_SUB] * 2;
        for (k = 0; k < 4u; k++) {
            int32_t r = rr[k] + ((sin15(t * (40000u + k * 13000u) + k * 0x40000000u) * (2 + par[P_BREATH] / 16)) >> 15);
            int32_t aa = (a * clamp(amt[k], 0, 256)) >> 8;
            uint32_t pal = k == 3u ? 2u : k;
            if (aa <= 4) continue;
            drng[n].r2 = r * r, drng[n].band = 2 * r * w[k], drng[n].inv = 65536 / drng[n].band, drng[n].a = aa;
            drng[n].c[0] = (uint8_t)clamp(pal_now[pal][0] >> 8, 0, 255);
            drng[n].c[1] = (uint8_t)clamp((pal_now[pal][1] >> 8) * (128 + par[P_DTONE]) / 255, 0, 255);
            drng[n].c[2] = (uint8_t)clamp(pal_now[pal][2] >> 8, 0, 255);
            n++;
        }
        nring = n;
        glow_horizon = (lvl * f) >> 9;
    } else if (pg == PG_SEQ) {                  /* the machine: the scale as an arch of stars over RANGE, its chord bright,
                                                 * the notes it plays lit and threaded together */
        int32_t top = range_top(s), n = (int32_t)s->n, d, ch = vis_chord, base = pad_base();
        int32_t lit_x[NV], lit_y[NV], nl = 0;
        for (d = 0; d <= top; d++) {
            int32_t xx = 14 + d * 212 / (top > 0 ? top : 1), dx = xx - 120, yy = 36 + dx * dx * 52 / 14400, note, a, rad = 2;
            int32_t m = ((d - ch) % n + n) % n, sounding = 0;
            note = base + scale_note(s, d);
            while (note > 88) note -= 12;
            for (k = 0; k < NV; k++)
                if (vis_kind[k] == 0 && vis_lvl[k] > 10 && vis_note[k] == note) sounding = vis_lvl[k];
            a = m == 0 || m == 2 || m == 4 ? 170 : 70;
            if (d % n == 0) rad = 3;            /* (the root, in each octave) */
            if (sounding) {
                a = 255, rad = 4 + sounding / 64;
                if (nl < (int32_t)NV) lit_x[nl] = xx, lit_y[nl] = yy, nl++;
            }
            dot_add(xx, yy, rad, (a * f) >> 8, sounding ? wpal : pal_now[m == 0 || m == 2 || m == 4 ? 0 : 2]);
        }
        for (d = 1; d < nl && nseg < NSEG; d++) {
            seg[nseg].x0 = (int16_t)lit_x[d - 1], seg[nseg].y0 = (int16_t)lit_y[d - 1];
            seg[nseg].x1 = (int16_t)lit_x[d], seg[nseg].y1 = (int16_t)lit_y[d], seg[nseg].a = (uint8_t)((120 * f) >> 8);
            nseg++;
        }
        if (vis_chord_seq != v_chord_seen) v_chord_seen = vis_chord_seq, v_flash = v_flash > 90 ? v_flash : 90;
    } else if (pg == PG_RHY) {                  /* the drums: four rows of lights low in the sky (blip, hats, snare, kick),
                                                 * the step playing bright */
        static const uint8_t ROWF[4] = {DK_PERC, DK_HAT | DK_OPEN, DK_SNARE, DK_KICK}, ROWR[4] = {3, 3, 4, 5}, ROWP[4] = {2, 1, 0, 0};
        int32_t st = vis_step, row;
        for (row = 0; row < 4; row++)
            for (k = 0; k < 16u; k++) {
                int32_t xx = 18 + (int32_t)k * 204 / 15, yy = 70 + row * 20, on = apat[k] & ROWF[row], a;
                a = on ? (int32_t)k == st ? 255 : 90 + aprob[k] * 6 / 5 : (int32_t)k == st ? 110 : 50;   /* (brighter: likelier) */
                if (!g_drums) a = a / 2;
                dot_add(xx, yy, on ? ROWR[row] : 1, (a * f) >> 8, (int32_t)k == st && on ? wpal : pal_now[ROWP[row]]);
            }
    } else if (pg == PG_MIX1 || pg == PG_MIX2) {   /* the mix: four columns of light from the lake, one over each knob,
                                                 * as tall as its level, glowing as that part sounds */
        uint32_t base = pg == PG_MIX1 ? 0u : 4u;
        for (k = 0; k < 4u; k++) {
            int32_t lvl = par[(pg == PG_MIX1 ? P_PADS : P_WIND) + k], live = vis_mix[base + k];
            mixc[k].h = (int16_t)(lvl ? 6 + lvl * (HZ - 44) / 127 : 0);
            mixc[k].a = (int16_t)(((110 + (live * 145 >> 8)) * f) >> 8);
            mixc[k].c[0] = (uint8_t)clamp(pal_now[k % 3u][0] >> 8, 0, 255), mixc[k].c[1] = (uint8_t)clamp(pal_now[k % 3u][1] >> 8, 0, 255);
            mixc[k].c[2] = (uint8_t)clamp(pal_now[k % 3u][2] >> 8, 0, 255);
            if (mixc[k].h) dot_add(30 + (int32_t)k * 60, HZ - mixc[k].h, 4, mixc[k].a + 50, pal_now[k % 3u]);
        }
        nmix = 4;
    } else if (pg == PG_NIGHT) {                /* the night: fireflies over the shore, as many as LAKE; a loon's call
                                                 * ripples across the water (drawn on every page) */
        static const int32_t FF[3] = {190 << 8, 255 << 8, 120 << 8};
        int32_t n = 4 + par[P_LAKE] / 9;
        for (k = 0; k < (uint32_t)n && k < 18u; k++) {
            uint32_t ph = t * (2000u + k * 330u) + k * 0x9E3779B9u;
            int32_t xx = 8 + (int32_t)((k * 53u + (t >> 7) * (k & 1u ? 1u : 3u)) % 224u) + (sin15(ph) >> 12);
            int32_t yy = HZ - 6 - (int32_t)((k * 37u) % 34u) + (sin15(ph * 3u) >> 13), b = sin15(ph * 7u + k * 0x3000000u);
            if (b > 8000) dot_add(xx, yy, 2, ((b >> 7) * f) >> 8, FF);
        }
    } else if (pg == PG_FX) {                   /* the effects: ripples on the water (ECHO), the stars (BELLS), snow on
                                                 * the wind (WIND), the horizon glowing (DRONE) */
        int32_t wind = par[P_WIND];
        star_gain = 200 + par[P_BELLS] * 2;
        glow_horizon = ((clamp(vis_drone * 256 / 9200, 0, 256) + par[P_DRONE]) * f) >> 9;
        for (k = 0; k < NPART; k++) {
            if (!part[k].sp) part[k].x = (int32_t)vrnd_n(240) << 8, part[k].y = (uint8_t)(8 + vrnd_n(HZ - 20)),
                             part[k].sp = (uint8_t)(1 + vrnd_n(4)), part[k].ph = (uint16_t)vrnd();
            part[k].x -= (int32_t)part[k].sp * (40 + wind * 6);
            if (part[k].x < 0) part[k].x += 240 << 8, part[k].y = (uint8_t)(8 + vrnd_n(HZ - 20));
        }
    }
    (void)now;
}

static void ov_strip(uint32_t y0, uint32_t now)  /* the page's light, the frozen sky's sparks, held notes' pillars: this strip */
{
    uint32_t k, x;
    int32_t y, f = ov_fade;
    int32_t ys = (int32_t)y0, ye = (int32_t)(y0 + STRIP);
    for (k = 0; k < oc_n; k++) {                /* the lines: a core and a glow 4 rows either side; the water mirrors some */
        int32_t cr = oc_col[k][0], cg = oc_col[k][1], cb = oc_col[k][2];
        for (x = 0; x < 240u; x++) {
            int32_t a = oc_a[k][x], cy = oc_y[k][x], yr = cy >> 4, y1, y2, lo = cy, hi = cy;
            if (!a) continue;
            if (x > 0u && oc_a[k][x - 1u]) {    /* (steep: the line fills down to the last column's row) */
                int32_t pc = oc_y[k][x - 1u];
                if (pc < lo) lo = pc + 16;
                if (pc > hi) hi = pc - 16;
                if (lo > cy) lo = cy;
                if (hi < cy) hi = cy;
            }
            y1 = (lo >> 4) - 4 < ys ? ys : (lo >> 4) - 4, y2 = (hi >> 4) + 4 >= ye ? ye - 1 : (hi >> 4) + 4;
            for (y = y1; y <= y2; y++) {
                int32_t yq = (y << 4) + 8, v, d = yq < lo ? lo - yq : yq > hi ? yq - hi : 0;
                if (d >= 64) continue;
                v = ((64 - d) * a) >> 6;
                v = (v * v) >> 8;
                if (d < 14) v += a >> 1;
                px_add((int32_t)x, y, y0, (cr * v) >> 8, (cg * v) >> 8, (cb * v) >> 8);
            }
            if (oc_refl >> k & 1u && yr < HZ) {
                int32_t ry = HZ + ((HZ - 1 - yr) * 4) / 7;
                if (ry >= ys && ry < ye) px_add((int32_t)x, ry, y0, (cr * a) >> 10, (cg * a) >> 10, (cb * a) >> 10);
            }
        }
    }
    for (k = 0; k < ndot; k++) {                /* the points: a soft round glow and a bright middle */
        int32_t r = dot[k].rad, cx = dot[k].x, cy = dot[k].y, dy, dx, r2 = r * r + 1, a = dot[k].a;
        if (cy + r < ys || cy - r >= ye) continue;
        for (dy = -r; dy <= r; dy++) {
            if (cy + dy < ys || cy + dy >= ye) continue;
            for (dx = -r; dx <= r; dx++) {
                int32_t d2 = dx * dx + dy * dy, v;
                if (d2 >= r2) continue;
                v = a - d2 * a / r2;
                v = (v * v) >> 8;
                if (d2 <= 1 && a > 100) v = 255;
                px_add(cx + dx, cy + dy, y0, (dot[k].r * v) >> 8, (dot[k].g * v) >> 8, (dot[k].b * v) >> 8);
            }
        }
    }
    for (k = 0; k < nseg; k++) {                /* the threads */
        int32_t x0 = seg[k].x0, y0s = seg[k].y0, dx = seg[k].x1 - x0, dy = seg[k].y1 - y0s, n, i, a = seg[k].a;
        if ((y0s < ys && seg[k].y1 < ys) || (y0s >= ye && seg[k].y1 >= ye)) continue;
        n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
        if (n < 1) continue;
        for (i = 0; i <= n; i += 2) px_add(x0 + dx * i / n, y0s + dy * i / n, y0, a, a, a);
    }
    if (nring && ys < HZ) {                     /* the drone's rings: only above the water, centred under the horizon */
        int32_t yl = ye > HZ ? HZ : ye;
        for (y = ys; y < yl; y++) {
            int32_t dy = y - RING_CY, dy2 = dy * dy;
            for (x = 0; x < 240u; x++) {
                int32_t dx = (int32_t)x - 120, d2 = dx * dx + dy2;
                if (y >= HZ - tree_h[x]) continue;
                for (k = 0; k < nring; k++) {
                    int32_t diff = d2 - drng[k].r2, v;
                    diff = diff < 0 ? -diff : diff;
                    if (diff >= drng[k].band) continue;
                    v = (((drng[k].band - diff) * drng[k].inv) >> 8) * drng[k].a >> 8;
                    v = (v * (128 + (v >> 1))) >> 8;
                    px_add((int32_t)x, y, y0, (drng[k].c[0] * v) >> 8, (drng[k].c[1] * v) >> 8, (drng[k].c[2] * v) >> 8);
                }
            }
        }
    }
    for (k = 0; k < nmix; k++) {                /* MIX: the columns, brightest at the lake, fading up; the water mirrors them */
        int32_t cx = 30 + (int32_t)k * 60, h = mixc[k].h, a = mixc[k].a, dx;
        if (!h) continue;
        for (y = ys; y < ye; y++) {
            int32_t v, up;
            if (y < HZ) { if (y < HZ - h) continue; up = HZ - y; v = a * (h - up / 2) / h; }
            else { up = y - HZ; if (up > h / 3) continue; v = (a * (h / 3 - up) / (h / 3 + 1)) >> 1; }
            for (dx = -9; dx <= 9; dx++) {
                int32_t ad = dx < 0 ? -dx : dx, w = ((10 - ad) * (10 - ad) * v) / 100;
                if (y < HZ && y >= HZ - tree_h[cx + dx]) w >>= 2;
                px_add(cx + dx, y, y0, (mixc[k].c[0] * w) >> 8, (mixc[k].c[1] * w) >> 8, (mixc[k].c[2] * w) >> 8);
            }
        }
    }
    if (glow_horizon > 0 && ye > HZ - 34 && ys < HZ + 6) {   /* the horizon's glow */
        int32_t c0 = pal_now[0][0] >> 8, c1 = pal_now[0][1] >> 8, c2 = pal_now[0][2] >> 8;
        for (y = ys; y < ye; y++) {
            int32_t d = y < HZ ? HZ - y : (y - HZ) * 5, v;
            if (d >= 34) continue;
            v = (glow_horizon * (34 - d) * (34 - d)) >> 10;
            for (x = 0; x < 240u; x++) {
                if (y < HZ && y >= HZ - tree_h[x]) continue;
                px_add((int32_t)x, y, y0, (c0 * v) >> 8, (c1 * v) >> 8, (c2 * v) >> 8);
            }
        }
    }
    if (vis_sleep >= 0 && ys < HZ) {            /* the sleep timer: a crescent moon sets across the sky as the time goes */
        int32_t p = vis_sleep > 256 ? 256 : vis_sleep, mx = 206 - p * 170 / 256, my = 44 + ((p * p) >> 8) * (HZ - 60) / 256, dy, dx;
        for (dy = -10; dy <= 10; dy++) {
            int32_t yy = my + dy;
            if (yy < ys || yy >= ye || yy >= HZ) continue;
            for (dx = -10; dx <= 10; dx++) {
                int32_t d2 = dx * dx + dy * dy, c2 = (dx - 3) * (dx - 3) + (dy + 2) * (dy + 2), v;
                if (d2 <= 42 && c2 > 30) v = 230;   /* (the crescent) */
                else if (d2 <= 100) v = (100 - d2) / 3;   /* (its glow) */
                else continue;
                if ((uint32_t)(mx + dx) < 240u && yy < HZ - tree_h[mx + dx]) px_add(mx + dx, yy, y0, v, v, (v * 7) >> 3);
            }
        }
    }
    {
        for (k = 0; k < NRIP; k++) {            /* the ripples: rings spreading on the water, flattened */
            uint32_t age;
            int32_t r, band, inv, a, cx;
            if (!rip[k].on) continue;
            age = now - rip[k].t0;
            if ((int32_t)age < 0) continue;
            if (age > 2600u) { rip[k].on = 0; continue; }
            r = 3 + (int32_t)age / 30, band = 2 * r * 2, inv = 65536 / band;
            a = rip[k].a * (int32_t)(2600u - age) / 2600;
            cx = rip[k].x;
            for (y = ys < HZ + 1 ? HZ + 1 : ys; y < ye; y++) {
                int32_t dy = (y - (HZ + 22)) * 3, dy2 = dy * dy;
                if (dy2 > (r + 2) * (r + 2)) continue;
                for (x = (uint32_t)(cx - r - 2 < 0 ? 0 : cx - r - 2); (int32_t)x <= cx + r + 2 && x < 240u; x++) {
                    int32_t dx = (int32_t)x - cx, diff = dx * dx + dy2 - r * r, v;
                    diff = diff < 0 ? -diff : diff;
                    if (diff >= band) continue;
                    v = (((band - diff) * inv) >> 8) * a >> 8;
                    px_add((int32_t)x, y, y0, v * 3 / 4, v, v);
                }
            }
        }
    }
    if (ov_page == PG_FX && f > 0) {
        for (k = 0; k < NPART; k++) {           /* the snow: streaks on the wind */
            int32_t px = part[k].x >> 8, py = part[k].y + ((sin15((uint32_t)part[k].ph * 65536u + v_t * 400000u) * 3) >> 15), j;
            int32_t a = ((120 + par[P_WIND]) * f) >> 8, len = 2 + par[P_WIND] / 24;
            if (py < ys || py >= ye || !part[k].sp) continue;
            for (j = 0; j <= len; j++) px_add(px + j, py, y0, a - j * 20, a - j * 20, a - j * 16);
        }
    }
    if (v_ice > 0) {                            /* frozen: sparks of ice that come and go */
        for (k = 0; k < NFROST; k++) {
            int32_t fx = frost[k].x, fy = frost[k].y, ph = (int32_t)((now / 4u + frost[k].t) & 511u), b;
            if (fy < ys - 2 || fy >= ye + 2) continue;
            b = ph < 256 ? ph : 511 - ph;
            b = ((b * b) >> 8) * v_ice >> 8;
            px_add(fx, fy, y0, b, b, b);
            if (b > 120) {
                px_add(fx - 1, fy, y0, b / 3, b / 3, b / 2), px_add(fx + 1, fy, y0, b / 3, b / 3, b / 2);
                px_add(fx, fy - 1, y0, b / 3, b / 3, b / 2), px_add(fx, fy + 1, y0, b / 3, b / 3, b / 2);
            }
        }
    }
    for (k = 0; k < NCH; k++) {                 /* the choir: shafts of pale light down from the top, where its notes are */
        int32_t lvl = vis_ch_lvl[k], cx, dx, yl;
        if (lvl <= 6 || ys >= HZ) continue;
        cx = note_x(vis_ch_note[k] + 12);
        yl = ye > HZ ? HZ : ye;
        for (y = ys; y < yl; y++) {
            int32_t fall = 256 - y * 256 / HZ, sway = (sin15((uint32_t)y * 60000000u + v_t * 500000u + k * 0x40000000u) * 3) >> 15;
            int32_t tw = 210 + (sin15((uint32_t)y * 90000000u + v_t * 700000u) >> 10);
            for (dx = -7; dx <= 7; dx++) {
                int32_t xx = cx + dx + sway, ad = dx < 0 ? -dx : dx, v;
                if ((uint32_t)xx >= 240u || y >= HZ - tree_h[xx]) continue;
                v = ((((lvl * fall) >> 8) * (8 - ad) * (8 - ad)) >> 6) * tw >> 8;
                px_add(xx, y, y0, (v * (200 + (pal_now[1][0] >> 10))) >> 9, (v * (190 + (pal_now[1][1] >> 10))) >> 9,
                       (v * (170 + (pal_now[1][2] >> 9))) >> 9);
            }
        }
    }
    for (k = 0; k < NV; k++) {                  /* held (latched) notes: pillars of light from the horizon up */
        int32_t lvl = vis_lvl[k], cx, dx, yl;
        if (vis_kind[k] != 2 || lvl <= 8 || ys >= HZ) continue;
        cx = note_x(vis_note[k]);
        yl = ye > HZ ? HZ : ye;
        for (y = ys; y < yl; y++) {
            int32_t up = HZ - y, fall = 256 - up * 256 / HZ, tw = 200 + (sin15((uint32_t)y * 140000000u - v_t * 900000u) >> 9);
            for (dx = -4; dx <= 4; dx++) {
                int32_t v = ((lvl * fall) >> 8) * (5 - (dx < 0 ? -dx : dx)) / 5;
                v = (v * tw) >> 8;
                if ((uint32_t)(cx + dx) >= 240u || y >= HZ - tree_h[cx + dx]) continue;
                px_add(cx + dx, y, y0, (pal_now[0][0] >> 9) * v >> 7, (pal_now[0][1] >> 9) * v >> 7, ((pal_now[0][2] >> 9) + 40) * v >> 7);
            }
        }
    }
}

/* the curtains: where each one hangs (its edge a slow wave), how long, how bright, its rays bunching where it folds */
static const struct { int16_t base, a1, a2, len, wt; uint32_t w1, s1, w2, s2, h1, h2, s3, s4, wp, s5, sa; } CU[NC] = {
    {116, 17, 6, 88, 256, 14316557u, 116080u, 61356675u, 330382u, 477218588u, 186737708u, 613566u, 858993u, 16519105u, 104755u, 91382u},
    {96, 12, 5, 62, 205, 22605091u, 148102u, 78090314u, 252645u, 390451572u, 226050910u, 715827u, 536870u, 23860929u, 81037u, 70409u},
    {134, 9, 4, 44, 170, 17895697u, 81037u, 47721858u, 390451u, 536870912u, 165191049u, 505290u, 954437u, 19522579u, 126322u, 51746u},
};
static void meteor(uint32_t now, int32_t x, int32_t y, int32_t dx, int32_t dy, uint32_t life)
{
    uint32_t k;
    for (k = 0; k < NMET; k++)
        if (!met[k].on) {
            met[k].on = 1, met[k].t0 = now, met[k].life = life;
            met[k].x = x, met[k].y = y, met[k].dx = dx, met[k].dy = dy;
            return;
        }
}
static void frame_setup(uint32_t now)
{
    const scene_t *sc = &SCENES[g_scene];
    uint32_t k, x, dt = now - v_last, t;
    static uint8_t boost[240];
    int32_t en, ice;
    v_last = now;
    if (dt > 250u) dt = 250u;
    v_t += dt * (uint32_t)(40 + par[P_DRIFT]) / 104u;   /* DRIFT: the sky moves at 0.4 .. 1.6 of its pace */
    v_ice += g_freeze ? (256 - v_ice + 7) / 8 : -(v_ice + 7) / 8;   /* FREEZE: the sky slows to a stop and turns to ice */
    v_ice = clamp(v_ice, 0, 256);
    v_tc += dt * (uint32_t)(40 + par[P_DRIFT]) * (uint32_t)(256 - v_ice) / (104u * 256u);
    {                                           /* waiting: the aurora sinks low; sleeping: down to the stars */
        int32_t want = g_run ? 256 : 90;
        if (vis_sleep >= 0) want = (want * g_svol) >> 8;
        v_run_lvl += (want - v_run_lvl + (want > v_run_lvl ? 15 : -15)) / 16;
    }
    ov_fade += view != PG_HOME && view == ov_page ? (256 - ov_fade + 3) / 4 : -(ov_fade + 3) / 4;
    ov_fade = clamp(ov_fade, 0, 256);
    if (ov_fade == 0) ov_page = view;
    t = v_tc;
    ice = v_ice;
    for (k = 0; k < 5u; k++) {                  /* the colours ease to the scene's (and to ice, frozen) */
        const uint8_t *c = k < 3u ? sc->c[k] : sc->sky[k - 3u];
        for (x = 0; x < 3u; x++) {
            int32_t want = (c[x] * (256 - ice) + ICE[k][x] * ice) >> 8, d = (want << 8) - pal_now[k][x];
            pal_now[k][x] += d / 12 + (d > 0) - (d < 0);
        }
    }
    for (k = 0; k < 16u; k++)                   /* the curtain's colour: c0 at the edge, c1 a third up, c2 at the top */
        for (x = 0; x < 3u; x++)
            ramp[k][x] = (uint8_t)clamp(k < 6u ? (pal_now[0][x] * (int32_t)(5u - k) + pal_now[1][x] * (int32_t)k) / (5 * 256)
                                               : (pal_now[1][x] * (int32_t)(15u - k) + pal_now[2][x] * (int32_t)(k - 5u)) / (10 * 256), 0, 255);
    for (k = 0; k < HZ; k++) {                  /* the sky: dark above, lighter towards the horizon (more so near it) */
        int32_t f = (int32_t)(k * k) * 256 / ((HZ - 1) * (HZ - 1));
        for (x = 0; x < 3u; x++)
            sky_c[k][x] = (uint8_t)clamp((pal_now[3][x] * (256 - f) + pal_now[4][x] * f) >> 16, 0, 255);
    }
    en = vis_out >> 6;                          /* how loud it is: the whole sky answers */
    v_en += (clamp(en, 0, 255) - v_en) / 4;
    for (x = 0; x < 240u; x++) boost[x] = 0;
    for (k = 0; k < NV; k++) {                  /* each pad voice: a pillar of light where its note is */
        int32_t lvl = vis_lvl[k], cx, dx;
        if (lvl <= 0) continue;
        cx = note_x(vis_note[k]);
        for (dx = -34; dx <= 34; dx++) {
            int32_t xx = cx + dx, g, s;
            if ((uint32_t)xx >= 240u) continue;
            g = (32767 + sin15((uint32_t)(dx + 34) * 63161284u - 0x40000000u)) >> 8;   /* a raised cosine, 0 .. 255 */
            s = boost[xx] + ((lvl * g) >> 8);
            boost[xx] = (uint8_t)(s > 255 ? 255 : s);
        }
    }
    for (k = 0; k < NC; k++) {
        int32_t act = 96 + ((sin15(t * CU[k].sa + k * 0x55555555u) * 64) >> 15) + v_en / 3 + v_flash / 3;
        act = (clamp(act, 0, 255) * CU[k].wt) >> 8;
        act = (act * v_run_lvl) >> 8;
        if (ov_page == PG_LFO && ov_fade > 0 && par[P_TREMOLO])   /* MOTION: the curtains pulse with TREMOLO */
            act = (act * (256 - ((par[P_TREMOLO] * ov_fade * ((32767 + sin15(v_t * 900000u)) >> 8)) >> 17))) >> 8;
        for (x = 0; x < 240u; x++) {
            int32_t e1 = sin15(x * CU[k].w1 + t * CU[k].s1 + k * 0x3A000000u);
            int32_t e2 = sin15(x * CU[k].w2 - t * CU[k].s2 + k * 0x91000000u);
            int32_t disp = (e1 * CU[k].a1 + e2 * CU[k].a2) >> 15, ray, patch, in, len;
            uint32_t fold = (uint32_t)(disp * 107374182);   /* (a 40th of a cycle a row: the rays bunch in the folds) */
            ray = 150 + ((sin15(x * CU[k].h1 + fold + t * CU[k].s3) * 62 + sin15(x * CU[k].h2 - (fold >> 1) - t * CU[k].s4) * 42) >> 15);
            patch = 128 + ((sin15(x * CU[k].wp + t * CU[k].s5 + k * 0x70000000u) * 127) >> 15);
            patch = (patch * patch) >> 8;
            in = (((patch * ray) >> 8) * act) >> 8;
            in += (boost[x] * ray * CU[k].wt) >> 16;
            len = (CU[k].len * (130 + (ray >> 1) + (patch >> 2))) >> 8;
            len += (len * (v_en + v_flash / 2)) >> 10;
            len += (len * boost[x]) >> 10;
            cu_y[k][x] = (int16_t)(CU[k].base + disp);
            cu_i[k][x] = (uint8_t)(in > 255 ? 255 : in);
            cu_inv[k][x] = (uint16_t)(16384 / (len < 8 ? 8 : len));
        }
    }
    if (vis_bell_seq != v_bell_seen) {          /* a bell: the star nearest its note flares; a fall of bells: a shower */
        uint32_t s = 0, best = 999u, kind = (uint32_t)vis_bell_kind;
        int32_t bx = note_x(vis_bell_note - 12);
        v_bell_seen = vis_bell_seq;
        for (k = 0; k < 12u; k++) {
            uint32_t c = vrnd_n(NSTAR), d = (uint32_t)(star[c].x > bx ? star[c].x - bx : bx - star[c].x);
            if (d < best) best = d, s = c;
        }
        star[s].flash = 255;
        if (kind == 1u) {                       /* a fall of bells: shooting stars down the sky, the aurora flares */
            int32_t j;
            v_flash = 255;
            for (j = 0; j < 3; j++)
                meteor(now - (uint32_t)j * 90u, 10 + (int32_t)vrnd_n(220), 2 + (int32_t)vrnd_n(40), (vrnd() & 1u ? 1 : -1) * (1 + (int32_t)vrnd_n(4)),
                       6 + (int32_t)vrnd_n(5), 900u + vrnd_n(400));
        } else if (vrnd_n(3) == 0u || kind == 2u) {
            meteor(now, 30 + (int32_t)vrnd_n(180), 6 + (int32_t)vrnd_n(60), (vrnd() & 1u ? 1 : -1) * (5 + (int32_t)vrnd_n(4)),
                   3 + (int32_t)vrnd_n(3), 520u);
        }
        if (ov_page == PG_FX) {                 /* EFFECTS: the bell's ripple, and its echoes' */
            int32_t e = par[P_ECHO], a = 120 + e, j, rx = note_x(vis_bell_note - 12);
            ripple(rx, a, now);
            for (j = 1; j <= e / 32 + 1; j++) ripple(rx + (j & 1 ? 14 : -14) * j, a >> j, now + (uint32_t)j * (j & 1 ? 500u : 375u));
        }
    }
    {                                           /* the drums in the sky: a kick, the aurora pulses; a snare, a star flares;
                                                 * the hats, stars twinkle */
        static uint32_t ks, ss, hs;
        if (vis_kick_seq != ks) ks = vis_kick_seq, v_flash = v_flash > 80 ? v_flash : 80;
        if (vis_snare_seq != ss) {
            ss = vis_snare_seq, star[vrnd_n(NSTAR)].flash = 230;
            if (vrnd_n(5) == 0u) meteor(now, 30 + (int32_t)vrnd_n(180), 6 + (int32_t)vrnd_n(50), (vrnd() & 1u ? 1 : -1) * (4 + (int32_t)vrnd_n(4)), 3, 420u);
        }
        if (vis_hat_seq != hs) hs = vis_hat_seq, star[vrnd_n(NSTAR)].flash = 110;
    }
    {                                           /* a loon calls: rings spread across the water from its side */
        static uint32_t ls;
        if (vis_loon_seq != ls) {
            int32_t j;
            ls = vis_loon_seq;
            for (j = 0; j < 3; j++) ripple(vis_loon_x + (j - 1) * 6, 200 - j * 40, now + (uint32_t)j * 700u);
        }
    }
    v_flash -= (v_flash >> 4) + (v_flash > 0);
    for (k = 0; k < NSTAR; k++) star[k].flash = (uint8_t)(star[k].flash - (star[k].flash >> 3) - (star[k].flash > 0));
    for (k = 0; k < NMET; k++)
        if (met[k].on && now - met[k].t0 > met[k].life) met[k].on = 0;
    if (v_ice > 0)                              /* the ice's sparks move on now and then */
        for (k = 0; k < 2u; k++) {
            uint32_t j = vrnd_n(NFROST);
            frost[j].x = (uint8_t)vrnd_n(240), frost[j].y = (uint8_t)vrnd_n(236), frost[j].t = (uint16_t)vrnd();
        }
    ov_setup(now);
}

static void hud_draw(uint32_t y0, uint32_t now)   /* the HUD's text into this strip */
{
    const char *tl = 0, *tr = 0;
    char val[12];
    uint16_t cl = RGB(170, 215, 205);
    cv_oy = -(int32_t)y0;
    if (now - v_boot < 4200u) {                 /* switched on: the name, fading */
        uint32_t a = now - v_boot, f = a < 600u ? a * 255u / 600u : a > 3200u ? (4200u - a) * 255u / 1000u : 255u;
        if (y0 + STRIP > 70u && y0 < 124u) {
            cv_text((240 - text_w(&FONT_L, "AURORA")) / 2, 70, &FONT_L, "AURORA", RGB(f, f, f));
            cv_text((240 - text_w(&FONT_S, "ambient machine")) / 2, 106, &FONT_S, "ambient machine", RGB(f * 150u / 255u, f * 200u / 255u, f * 190u / 255u));
        }
    }
    if (y0 == 0u) {                             /* the top line: news, a knob, the page, what is held */
        if (hud_mode == HUD_MSG) tl = hud_l, tr = hud_r;
        else if (hud_mode == HUD_KNOB && hud_param == P_SLEEP) {
            static const uint8_t MIN[6] = {15, 30, 45, 60, 90, 120};
            tl = "SLEEP", tr = par[P_SLEEP] ? (put_s(put_u(val, MIN[(par[P_SLEEP] - 1) * 6 / 127]), " MIN"), val) : "OFF";
        }
        else if (hud_mode == HUD_KNOB && hud_param < NPARAM) put_u(val, (uint32_t)par[hud_param]), tl = P_NAME[hud_param], tr = val;
        else if (view == PG_NIGHT && vis_sleep >= 0 && vis_sleep < 256) tl = PAGE_NAME[view], tr = "MOON SETTING", cl = C_WHITE;
        else if (view == PG_RHY) tl = PAGE_NAME[view], tr = g_drums ? "PLAYING" : "OFF: SEE MIX 2", cl = C_WHITE;
        else if (view != PG_HOME) tl = PAGE_NAME[view], tr = kmode != KM_PADS ? KM_NAME[kmode] : g_freeze ? "FROZEN" : "", cl = C_WHITE;
        else if (kmode != KM_PADS || g_freeze) tl = kmode != KM_PADS ? KM_NAME[kmode] : "", tr = g_freeze ? "FROZEN" : "";
        if (tl) cv_text(6, 4, &FONT_S, tl, cl);
        if (tr) cv_text(234 - text_w(&FONT_S, tr), 4, &FONT_S, tr, g_freeze && tr[0] == 'F' && tr[1] == 'R' ? RGB(190, 235, 255) : C_WHITE);
        if (hud_mode == HUD_KNOB && hud_param < NPARAM) cv_rect(6, 21, 2 + par[hud_param] * 226 / 127, 2, C_WHITE);
    }
    if ((hud_mode == HUD_KNOB || view != PG_HOME) && y0 == 240u - STRIP) {   /* the four knobs of this page */
        uint32_t k, pg = view != PG_HOME ? view : hud_param / 4u;
        for (k = 0; k < 4u; k++) {
            uint32_t p = pg * 4u + k;
            const char *s = P_NAME[p];
            uint16_t c = hud_mode == HUD_KNOB && p == hud_param ? C_WHITE : RGB(120, 160, 152);
            cv_text((int32_t)(k * 60u) + (60 - text_w(&FONT_S, s)) / 2, 218, &FONT_S, s, c);
            cv_rect((int32_t)(k * 60u) + 8, 236, 44, 2, RGB(40, 56, 54));
            cv_rect((int32_t)(k * 60u) + 8, 236, 1 + par[p] * 43 / 127, 2, c);
        }
    }
    cv_oy = 0;
}

static void strip_draw(uint32_t s, uint32_t now)
{
    uint32_t y0 = s * STRIP, yy, x, k, t = v_tc;
    int32_t dim_top = hud_mode != HUD_OFF || view != PG_HOME || kmode != KM_PADS || g_freeze;
    int32_t dim_bot = hud_mode == HUD_KNOB || view != PG_HOME, ice = v_ice;
    cv_begin(240, STRIP, C_BLACK);
    for (yy = 0; yy < STRIP; yy++) {
        int32_t y = (int32_t)(y0 + yy);
        uint16_t *p = &cv_px[yy * 240u];
        const uint8_t *bay = BAYER[y & 3];
        int32_t dim = (dim_top && y < 26) || (dim_bot && y >= 214);
        if (y < HZ) {
            for (x = 0; x < 240u; x++) {
                int32_t r, g, b;
                if (y >= HZ - tree_h[x]) { p[x] = ice ? px_pack(ice >> 5, ice >> 4, ice >> 4, 0) : 0; continue; }   /* the far shore (frosted) */
                sky_px((int32_t)x, y, &r, &g, &b);
                if (dim) r = (r * 3) >> 3, g = (g * 3) >> 3, b = (b * 3) >> 3;
                p[x] = px_pack(r, g, b, bay[x & 3u]);
            }
        } else {                                /* the water: the sky upside down, squeezed, rippled, darker; frozen: still
                                                 * ice, paler */
            int32_t dy = y - HZ, sy = HZ - 1 - ((dy * 7) >> 2);
            int32_t still = 256 - ice;
            int32_t off = (((sin15((uint32_t)dy * 286331153u + t * 1431655u) * (1 + dy / 11)) >> 15) * still) >> 8;
            int32_t wob = sin15((uint32_t)dy * 477218588u - t * 2386092u);
            for (x = 0; x < 240u; x++) {
                int32_t r, g, b, m;
                int32_t xs = (int32_t)x + off + ((((sin15(x * 119304647u + (uint32_t)wob * 9000u) * 3) >> 16) * still) >> 8);
                xs = xs < 0 ? 0 : xs > 239 ? 239 : xs;
                if (HZ - 1 - sy < tree_h[xs]) { p[x] = px_pack(ice >> 5, 1 + (ice >> 5), 3 + (ice >> 4), 0); continue; }   /* the shore in it */
                if (sy < 0) r = sky_c[0][0], g = sky_c[0][1], b = sky_c[0][2];
                else sky_px(xs, sy, &r, &g, &b);
                m = 150 + ((((sin15((uint32_t)dy * 1073741824u / 3u + x * 29826162u + t * 1908874u) * 34) >> 15) * still) >> 8) - dy / 2;
                r = (r * m) >> 8, g = (g * m) >> 8, b = ((b * m) >> 8) + 3;
                if (ice) {                      /* (ice: a pale sheen, faint cracks) */
                    int32_t crack = ((x * 7u + (uint32_t)dy * 13u) % 61u) == 0u ? 40 : 0;
                    r += (ice * (18 + crack)) >> 8, g += (ice * (26 + crack)) >> 8, b += (ice * (34 + crack)) >> 8;
                }
                if (dim) r = (r * 3) >> 3, g = (g * 3) >> 3, b = (b * 3) >> 3;
                p[x] = px_pack(r, g, b, bay[x & 3u]);
            }
        }
    }
    for (k = 0; k < NSTAR; k++) {               /* the stars, twinkling; a flare for a bell; the brightest in the water */
        int32_t sx = star[k].x, sy = star[k].y, tw, b, f = star[k].flash;
        if (sy >= HZ - 1 - tree_h[sx] || (dim_top && sy < 28)) continue;   /* (not behind the shore, nor the HUD's text) */
        tw = 190 + ((sin15((uint32_t)star[k].ph * 65536u + v_t * (uint32_t)star[k].sp * 110000u) * 65) >> 15);
        b = (((star[k].b * tw) >> 8) * star_gain) >> 8;
        b += f;
        if (b > 255) b = 255;
        if (sy >= (int32_t)y0 - 2 && sy < (int32_t)(y0 + STRIP) + 2) {
            px_add(sx, sy, y0, b, b, b);
            if (star[k].b > 190 || f > 40) {    /* a cross: the bright ones, and any that flares */
                int32_t h = (star[k].b > 190 ? b / 3 : 0) + f / 2;
                px_add(sx - 1, sy, y0, h, h, h), px_add(sx + 1, sy, y0, h, h, h);
                px_add(sx, sy - 1, y0, h, h, h), px_add(sx, sy + 1, y0, h, h, h);
                if (f > 120) {
                    px_add(sx - 2, sy, y0, f / 4, f / 4, f / 4), px_add(sx + 2, sy, y0, f / 4, f / 4, f / 4);
                    px_add(sx, sy - 2, y0, f / 4, f / 4, f / 4), px_add(sx, sy + 2, y0, f / 4, f / 4, f / 4);
                }
            }
        }
        if (b > 150 && sy > 44 && !ice) {
            int32_t ry = HZ + ((HZ - 1 - sy) * 4) / 7;
            px_add(sx + ((sin15((uint32_t)ry * 286331153u + t * 1431655u) * 3) >> 15), ry, y0, b / 3, b / 3, b / 3);
        }
    }
    for (k = 0; k < NMET; k++) {                /* the shooting stars: a head and a fading tail */
        uint32_t age, life = met[k].life;
        int32_t hx, hy, j, fade;
        if (!met[k].on) continue;
        age = now - met[k].t0;
        if (age > life) continue;
        hx = met[k].x + met[k].dx * (int32_t)age / 28, hy = met[k].y + met[k].dy * (int32_t)age / 28;
        fade = age < life - 160u ? 255 : (int32_t)(life - age) * 255 / 160;
        for (j = 0; j < 22; j++) {
            int32_t b = ((22 - j) * fade) / 22;
            b = (b * b) >> 8;
            px_add(hx - met[k].dx * j / 7, hy - met[k].dy * j / 7, y0, b, b, (b * 5) >> 2);
        }
    }
    ov_strip(y0, now);
    hud_draw(y0, now);
    cv_blit(0, y0);
}

/* ------------------------------------------------------------------ the panel (the main loop) */
#define SET_MAGIC 0x34525541u                   /* "AUR4" */
#define NMOM 4u                                 /* the moments: hold SAVE + one of the first four white keys (F G A B) keeps
                                                 * one, HOME + it brings it back */
#define MOMK(k) ((k) == 0u ? 0 : (k) == 2u ? 1 : (k) == 4u ? 2 : (k) == 6u ? 3 : -1)
typedef struct { uint8_t scene, scale, root, oct, kmode, used, par[NPARAM]; } mom_t;
typedef struct { uint32_t magic; uint8_t scene, scale, root, oct, run, kmode, drums, pad; uint8_t par[NPARAM]; mom_t mom[NMOM]; } set_t;
static set_t st;                                /* (the flash's copy: the power-on state and the moments) */
static void st_take(void)                       /* the power-on state from what is set now */
{
    uint32_t k;
    st.magic = SET_MAGIC, st.scene = (uint8_t)g_scene, st.scale = (uint8_t)g_scale, st.root = (uint8_t)g_root;
    st.oct = (uint8_t)(g_oct + 1), st.run = (uint8_t)g_run, st.kmode = (uint8_t)kmode, st.drums = (uint8_t)g_drums;
    for (k = 0; k < NPARAM; k++) st.par[k] = (uint8_t)(k == P_SLEEP ? 0 : par[k]);
}

static void drums_new(void)                     /* a new pattern, for the haze: BUSY says how much; each step a chance */
{
    int32_t b = par[P_BUSY], half = vrnd_n(10) < 7u, s;
    uint8_t p[16], a[16], pr[16];
    for (s = 0; s < 16; s++) p[s] = 0, pr[s] = 100, a[s] = (uint8_t)(40 + vrnd_n(40) + (s % 4 == 0 ? 30 : s % 2 == 0 ? 10 : 0));
    p[0] |= DK_KICK;                            /* the heartbeat: the one, and an answer */
    if (half) s = vrnd_n(2) ? 10 : 11, p[s] |= DK_KICK, pr[s] = (uint8_t)(55 + vrnd_n(30));
    else p[8] |= DK_KICK, pr[8] = 85;
    if (vrnd_n(100) < (uint32_t)(10 + b / 4)) s = vrnd_n(2) ? 6 : 14, p[s] |= DK_KICK, pr[s] = (uint8_t)(20 + vrnd_n(30));
    if (half) p[8] |= DK_SNARE, pr[8] = 90;     /* the rim: half time mostly */
    else p[4] |= DK_SNARE, p[12] |= DK_SNARE, pr[4] = pr[12] = 80;
    for (s = 1; s < 16; s += 2)                 /* ghost rims */
        if ((int32_t)vrnd_n(100) < b / 6) p[s] |= DK_SNARE, pr[s] = (uint8_t)(20 + vrnd_n(25)), a[s] = (uint8_t)(a[s] / 3);
    for (s = 0; s < 16; s++)                    /* the shaker: every sixteenth may, the eighths more often */
        if (vrnd_n(100) < (uint32_t)(40 + b / 2)) p[s] |= DK_HAT, pr[s] = pr[s] < 100 ? pr[s] : (uint8_t)(s % 2 ? 25 + b / 3 : 55 + b / 4);
    if (vrnd_n(2)) s = vrnd_n(2) ? 14 : 6, p[s] = (uint8_t)((p[s] & ~DK_HAT) | DK_OPEN), pr[s] = 60;
    for (s = 0; s < 2 + b / 32; s++) {          /* mallets: on the chord, the motif */
        int32_t j = (int32_t)vrnd_n(16);
        p[j] |= DK_PERC;
        if (pr[j] == 100 && j) pr[j] = (uint8_t)(45 + vrnd_n(40));
    }
    for (s = 0; s < 16; s++) dpat[s] = p[s], dacc[s] = a[s], dprob[s] = pr[s];
    g_dgen = g_dgen + 1u;
}
static void scene_load(uint32_t s, int with_sets)   /* a scene: its scale, its root and where its knobs start */
{
    const scene_t *sc;
    uint32_t k;
    g_scene = (int32_t)(s % NSCENE);
    sc = &SCENES[g_scene];
    if (!with_sets) return;
    g_scale = sc->scale, g_root = sc->root;
    for (k = 0; k < NPARAM; k++) par[k] = P_DEF[k];
    for (k = 0; k < 8u; k++) par[SCENE_P[k]] = sc->def[k];
    par[P_ATTACK] = (sc->atk - 1) * 127 / 79;
    par[P_RELEASE] = (sc->rel - 5) * 127 / 155;
    par[P_WAVE] = sc->wave;
    par[P_SHIMMER] = sc->shim;
}
static void hud_status(uint32_t ms)
{
    char l[32], r[20], *q;
    q = put_s(l, SCENES[g_scene].name);
    q = put_s(r, NOTE_NAME[EROOT]);
    q = put_s(q, " ");
    put_s(q, SCALES[g_scale].name);
    hud_msg(l, r, ms);
}

static void app_main(void)
{
    uint32_t prev_keys = 0, k, t_led = 0, latched = 0, sel_used = 0, busy_t = 0, sung = 0, save_used = 0, home_used = 0;
    uint32_t sleep_t0 = 0, sleep_len = 0, sleep_done = 0;
    int32_t shift_seen = 0, sleep_seen = 0;
    int32_t key_note[27];
    for (k = 0; k < 27u; k++) key_note[k] = -1;
    scene_load(0, 1);
    if (settings_load(&st, sizeof st) == 0 && st.magic == SET_MAGIC && st.scene < NSCENE && st.scale < NSCALE) {
        scene_load(st.scene, 0);
        g_scale = st.scale, g_root = st.root % 12u, g_oct = clamp((int32_t)st.oct - 1, -1, 1), g_run = st.run != 0;
        kmode = st.kmode < NKMODE ? st.kmode : KM_PADS;
        for (k = 0; k < NPARAM; k++) par[k] = st.par[k] & 127;
        par[P_SLEEP] = 0;
        if (par[P_DLEVEL] > 0) drums_new(), g_drums = 1;
    } else {
        for (k = 0; k < NMOM; k++) st.mom[k].used = 0;
        st_take();
    }
    v_seed ^= fm1_ticks();
    sky_init();
    v_boot = v_last = fm1_ms;
    hud_status(5200);

    for (;;) {
        uint32_t now, pressed, released, keys, edges, down, up;
        int32_t d;
        platform_poll();
        now = fm1_ms;

        /* the knobs: the page's four */
        view = page;
        for (k = 0; k < 4u; k++)
            if ((d = fm1_enc_take(ENC_KNOB1 + k)) != 0) {
                uint32_t p = view * 4u + k;
                if (p == P_BUSY) busy_t = now + 500u;   /* (BUSY: a new pattern once it settles) */
                par[p] = clamp(par[p] + d * 4, 0, 127);
                hud_mode = HUD_KNOB, hud_param = p, hud_until = now + 2600u;
            }
        if ((d = fm1_enc_take(ENC_ALGORITHM)) != 0) {
            scene_load((uint32_t)(((g_scene + d) % (int32_t)NSCENE + (int32_t)NSCENE) % (int32_t)NSCENE), 1);
            hud_status(3600);
        }
        if ((d = fm1_enc_take(ENC_SELECT)) != 0) {
            g_scale = ((g_scale + d) % (int32_t)NSCALE + (int32_t)NSCALE) % (int32_t)NSCALE;
            hud_msg("SCALE", SCALES[g_scale].name, 2600);
        }
        if ((d = fm1_enc_take(ENC_PRESETS)) != 0) {
            g_root = ((g_root + d) % 12 + 12) % 12;
            hud_msg("ROOT", NOTE_NAME[EROOT], 2600);
        }

        if (g_shift != shift_seen) {            /* the piece moved to a new key: say so */
            char r[20], *q;
            shift_seen = g_shift;
            q = put_s(r, NOTE_NAME[EROOT]), q = put_s(q, " "), put_s(q, SCALES[g_scale].name);
            hud_msg("NEW KEY", r, 3200);
        }

        /* the buttons */
        pressed = fm1_input_edges(&released);
        for (k = 0; k < NPAGE; k++)             /* a page's button: that page; again: back to the sky. FX: EFFECTS, then
                                                 * NIGHT, then the sky */
            if (k != PG_HOME && k != PG_NIGHT && k != PG_MIX1 && k != PG_MIX2 && pressed & 1u << PAGE_BTN[k]) {
                if (k == PG_FX) page = page == PG_FX ? PG_NIGHT : page == PG_NIGHT ? PG_HOME : PG_FX;
                else if (k == PG_RHY && page == PG_RHY) {   /* ARP on the drums' page: a new pattern (HOME leaves) */
                    drums_new();
                    hud_msg("DRUMS", g_drums ? "NEW PATTERN" : "NEW PATTERN READY", 1600);
                    continue;
                }
                else page = page == k ? PG_HOME : k;
                page_t = now;
                if (page == PG_HOME) hud_status(3000);
                else hud_mode = HUD_OFF;
            }
        if (pressed & 1u << BTN_HOME) home_used = 0;
        if (released & 1u << BTN_HOME && !home_used) {   /* HOME (let go, no key with it): the sky; again: MIX 1, MIX 2 */
            page = page == PG_HOME ? PG_MIX1 : page == PG_MIX1 ? PG_MIX2 : PG_HOME;
            page_t = now;
            if (page == PG_HOME) hud_status(3000);
            else hud_mode = HUD_OFF;
        }
        if (pressed & 1u << BTN_PLAY) {
            if (sleep_done || sleep_len) {      /* asleep (or falling asleep): PLAY wakes it */
                sleep_done = 0, sleep_len = 0, par[P_SLEEP] = 0, sleep_seen = 0, g_svol = g_sthin = 256, g_run = 1;
                hud_msg("GOOD MORNING", "", 2200);
            } else g_run = !g_run, hud_msg("THE MACHINE", g_run ? "PLAYS" : "WAITS", 2200);
        }
        if (par[P_SLEEP] != sleep_seen) {       /* SLEEP: 15 .. 120 minutes; 0: off */
            static const uint8_t MIN[6] = {15, 30, 45, 60, 90, 120};
            sleep_seen = par[P_SLEEP];
            sleep_done = 0, g_svol = g_sthin = 256;
            if (sleep_seen > 0) sleep_t0 = now, sleep_len = MIN[(sleep_seen - 1) * 6 / 127] * 60000u;
            else sleep_len = 0;
        }
        if (sleep_len) {                        /* the second half: the machine thins out; the last third (5 min at most):
                                                 * the sound fades; then quiet, the sky only stars */
            uint32_t el = now - sleep_t0, fw = sleep_len / 3 < 300000u ? sleep_len / 3 : 300000u;
            int32_t r = (int32_t)(sleep_len - el);
            if (r <= 0) {
                sleep_len = 0, sleep_done = 1, g_svol = 0, g_sthin = 0, g_run = 0;
            } else {
                g_sthin = (uint32_t)r < sleep_len / 2 ? (int32_t)((uint32_t)r * 256u / (sleep_len / 2)) : 256;
                g_svol = (uint32_t)r < fw ? (int32_t)((uint32_t)r * 256u / fw) : 256;
            }
            vis_sleep = (int32_t)(el * 256u / (sleep_len ? sleep_len : 1u));
        } else vis_sleep = sleep_done ? 256 : -1;
        if (pressed & 1u << BTN_REC) g_freeze = !g_freeze, hud_msg("FREEZE", g_freeze ? "ON" : "OFF", 2200);
        if (par[P_DLEVEL] > 0 && !g_drums) drums_new(), g_drums = 1;   /* the drums: LEVEL up from 0 starts them on a new
                                                 * pattern; down to 0 stops them */
        if (par[P_DLEVEL] == 0) g_drums = 0;
        if (busy_t && (int32_t)(now - busy_t) >= 0) {
            busy_t = 0;
            if (g_drums) drums_new(), hud_msg("DRUMS", "NEW PATTERN", 1600);
        }
        if (pressed & 1u << BTN_SEL) sel_used = 0;
        if (released & 1u << BTN_SEL && !sel_used) {   /* SEL tapped: the keys' next mode */
            if (kmode == KM_HOLD)               /* (leaving HOLD lets the held notes go) */
                for (k = 0; k < 27u; k++)
                    if (latched >> k & 1u) ev_put(64u + k, key_note[k], 0), key_note[k] = -1;
            latched = 0;
            kmode = (kmode + 1u) % NKMODE;
            hud_msg(KM_SAY[kmode], KM_HOW[kmode], 2600);
        }
        if (pressed & (1u << BTN_OCT_UP | 1u << BTN_OCT_DOWN)) {
            char r[4];
            if ((fm1_in.buttons >> BTN_OCT_UP & 1u) && (fm1_in.buttons >> BTN_OCT_DOWN & 1u)) g_oct = 0;
            else g_oct = clamp(g_oct + (pressed & 1u << BTN_OCT_UP ? 1 : -1), -1, 1);
            r[0] = g_oct < 0 ? '-' : g_oct > 0 ? '+' : ' ', r[1] = (char)('0' + (g_oct < 0 ? -g_oct : g_oct)), r[2] = 0;
            hud_msg("OCTAVE", r, 2200);
        }
        if (pressed & 1u << BTN_SAVE) save_used = 0;
        if (released & 1u << BTN_SAVE && !save_used) {   /* SAVE tapped: the power-on state */
            st_take();
            hud_msg("POWER-ON STATE", settings_save(&st, sizeof st) == 0 ? "SAVED" : "FAILED", 2600);
        }

        /* MIDI out: what the sound queued */
        while (moq_r != moq_w && midi_room() > 0) {
            uint32_t e = moq[moq_r & 63u];
            midi_send((uint8_t)(e >> 16), (uint8_t)(e >> 8 & 127u), (uint8_t)(e & 127u));
            moq_r = moq_r + 1u;
        }

        /* the keys: pad notes in the scale (PADS), latched (HOLD), or bells (BELLS); SCL held: the root */
        edges = fm1_input_note_edges();
        keys = fm1_in.notes;
        down = (keys | edges) & ~prev_keys, up = prev_keys & ~keys & ~edges;
        prev_keys = keys | edges;
        for (k = 0; k < 27u; k++) {
            if (down >> k & 1u) {
                int32_t note = scale_snap(53 + (int32_t)k + 12 * g_oct);
                if (MOMK(k) >= 0 && (fm1_in.buttons >> BTN_SAVE & 1u)) {   /* SAVE held + F G A B: keep this moment */
                    mom_t *mm = &st.mom[MOMK(k)];
                    uint32_t j;
                    char r[12];
                    save_used = 1;
                    mm->scene = (uint8_t)g_scene, mm->scale = (uint8_t)g_scale, mm->root = (uint8_t)g_root;
                    mm->oct = (uint8_t)(g_oct + 1), mm->kmode = (uint8_t)kmode, mm->used = 1;
                    for (j = 0; j < NPARAM; j++) mm->par[j] = (uint8_t)(j == P_SLEEP ? 0 : par[j]);
                    put_u(put_s(r, "MOMENT "), (uint32_t)MOMK(k) + 1u);
                    hud_msg(r, settings_save(&st, sizeof st) == 0 ? "KEPT" : "FAILED", 2600);
                    v_flash = 160;
                    continue;
                }
                if (MOMK(k) >= 0 && (fm1_in.buttons >> BTN_HOME & 1u)) {   /* HOME held + F G A B: back to that moment */
                    mom_t *mm = &st.mom[MOMK(k)];
                    uint32_t j;
                    char r[12];
                    home_used = 1;
                    put_u(put_s(r, "MOMENT "), (uint32_t)MOMK(k) + 1u);
                    if (!mm->used || mm->scene >= NSCENE || mm->scale >= NSCALE) { hud_msg(r, "EMPTY", 2000); continue; }
                    scene_load(mm->scene, 0);
                    g_scale = mm->scale, g_root = mm->root % 12u, g_oct = clamp((int32_t)mm->oct - 1, -1, 1);
                    kmode = mm->kmode < NKMODE ? mm->kmode : KM_PADS;
                    for (j = 0; j < NPARAM; j++) if (j != P_SLEEP) par[j] = mm->par[j] & 127;
                    if (par[P_DLEVEL] > 0) drums_new(), g_drums = 1;
                    hud_msg(r, SCENES[g_scene].name, 2600);
                    v_flash = 200;
                    continue;
                }
                if (fm1_in.buttons >> BTN_SEL & 1u) {
                    g_root = (int32_t)((5u + k + 12u - (uint32_t)g_shift) % 12u);
                    sel_used = 1;
                    hud_msg("ROOT", NOTE_NAME[EROOT], 2600);
                } else if (kmode == KM_BELLS) {
                    ev_put(k, note + 12, 3);
                } else if (kmode == KM_CHOIR) {         /* (an octave down: men under middle C, women over it) */
                    key_note[k] = note - 12, sung |= 1u << k;
                    ev_put(k, note - 12, 5);
                } else if (kmode == KM_HOLD) {
                    if (latched >> k & 1u) ev_put(64u + k, key_note[k], 0), latched &= ~(1u << k), key_note[k] = -1;
                    else key_note[k] = note, ev_put(64u + k, note, 1), latched |= 1u << k;
                } else {
                    key_note[k] = note;
                    ev_put(k, note, 1);
                }
            }
            if ((up >> k & 1u) && !(latched >> k & 1u) && key_note[k] >= 0)
                ev_put(k, key_note[k], sung >> k & 1u ? 4u : 0u), key_note[k] = -1, sung &= ~(1u << k);
        }
        {                                       /* MIDI in: notes play the pads too (as they come: not snapped) */
            uint8_t s, d1, d2;
            while (midi_get(&s, &d1, &d2)) {
                if ((s & 0xF0u) == 0x90u && d2) ev_put(32u + d1, d1, 1);
                else if ((s & 0xF0u) == 0x80u || (s & 0xF0u) == 0x90u) ev_put(32u + d1, d1, 0);
            }
        }

        /* the lights: the buttons' states; the page's button; the keys of the notes that sound (held ones always) */
        if (now - t_led >= 40u) {
            uint32_t lit = latched;
            t_led = now;
            for (k = 0; k < NV; k++)
                if (vis_lvl[k] > 24) {
                    int32_t kk = vis_note[k] - 53 - 12 * g_oct;
                    while (kk < 0) kk += 12;
                    while (kk > 26) kk -= 12;
                    lit |= 1u << kk;
                }
            for (k = 0; k < NCH; k++)               /* (and the choir's) */
                if (vis_ch_lvl[k] > 24) {
                    int32_t kk = vis_ch_note[k] + 12 - 53 - 12 * g_oct;
                    while (kk < 0) kk += 12;
                    while (kk > 26) kk -= 12;
                    lit |= 1u << kk;
                }
            for (k = 0; k < 27u; k++) fm1_led_key(KEY_LED(k), (int)(lit >> k & 1u));
            for (k = 0; k < NPAGE; k++)         /* (NIGHT: FX and HOME lit; MIX 1: SAVE; MIX 2: SAVE and HOME) */
                if (k != PG_NIGHT && k != PG_MIX1 && k != PG_MIX2)
                    fm1_led_key(PAGE_BTN[k], page == k || (k == PG_FX && page == PG_NIGHT) ||
                                (k == PG_HOME && (page == PG_NIGHT || page == PG_MIX2)));
            fm1_led_key(BTN_SAVE, page == PG_MIX1 || page == PG_MIX2);
            fm1_led_key(BTN_PLAY, g_run);
            fm1_led_key(BTN_REC, g_freeze);
            fm1_led_key(BTN_ARP, g_drums ? vis_step >= 0 && (vis_step & 3) < 2 : page == PG_RHY);   /* (playing: it blinks the beat) */
            fm1_led_key(BTN_SEL, (int)(fm1_in.buttons >> BTN_SEL & 1u) || (kmode != KM_PADS));   /* (lit: the keys are not playing pads; the screen says which) */
        }

        /* the screen: a frame at most every FRAME_MS, a strip at a time (every third millisecond at most: the rest
         * of the loop, and in a browser the audio thread, get their turn) */
        if (hud_mode != HUD_OFF && (int32_t)(now - hud_until) >= 0) hud_mode = HUD_OFF;
        if (v_strip == 0u) {
            if (now - v_frame_t < FRAME_MS) continue;
            v_frame_t = now;
            frame_setup(now);
        } else if (now % 3u) continue;
        strip_draw(v_strip, v_frame_t);
        v_strip = (v_strip + 1u) % NSTRIP;
    }
}
