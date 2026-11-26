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
 *   SEQ    MACHINE    CHORDS   RANGE    VOICES   ANSWER     the scale as a constellation, its chord lit
 *
 *   ALGORITHM    the scene (its sound, its sky, its scale, its settings)      SELECT   the scale
 *   PRESETS      the root note                                                OCT-/+   the register (both: middle)
 *   keys         PADS: play pads, snapped to the scale; HOLD: a key latches its note; BELLS: strike bells
 *   SEL          tap: the keys' mode (PADS, HOLD, BELLS); held + a key: that key is the root
 *   PLAY         the machine plays / waits (lit: playing)      REC    FREEZE: the reverb and the sky hold still (lit)
 *   ARP          a fall of bells now: a shower of stars        SAVE   keep all this as the power-on state
 *   MIDI in      notes play the pads
 */
#define APP_NAME "AURORA"
#include "platform.c"
#define LV_COUNT(c, x, k) ((void)0)
#include "dsp.c"
#include "hall.c"

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
       P_ECHO, P_BELLS, P_WIND, P_DRONE,        /* FX: the effects and the layers */
       P_ATTACK, P_RELEASE, P_LENGTH, P_PADS,   /* ENV: the pads' shape */
       P_WAVE, P_DETUNE, P_RESO, P_SHIMMER,     /* EDIT: their tone */
       P_RATE, P_SWEEP, P_WIDTH, P_TREMOLO,     /* LFO: their motion */
       P_DTONE, P_FIFTH, P_SUB, P_BREATH,       /* GLO: the drone */
       P_CHORDS, P_RANGE, P_VOICES, P_ANSWER,   /* SEQ: the machine */
       NPARAM };
static const char *const P_NAME[NPARAM] = {
    "DENSITY", "BRIGHT", "SPACE", "DRIFT", "ECHO", "BELLS", "WIND", "DRONE",
    "ATTACK", "RELEASE", "LENGTH", "PADS", "WAVE", "DETUNE", "RESO", "SHIMMER",
    "RATE", "SWEEP", "WIDTH", "TREMOLO", "TONE", "FIFTH", "SUB", "BREATH",
    "CHORDS", "RANGE", "VOICES", "ANSWER"};
/* where the knobs start that a scene does not set */
static const uint8_t P_DEF[NPARAM] = {
    64, 64, 90, 56, 60, 40, 30, 80,
    43, 53, 64, 85, 50, 40, 23, 0,
    56, 20, 90, 0, 50, 64, 0, 24,
    64, 64, 80, 38};
enum { PG_HOME, PG_FX, PG_ENV, PG_EDIT, PG_LFO, PG_GLO, PG_SEQ, NPAGE };
static const uint8_t PAGE_BTN[NPAGE] = {BTN_HOME, BTN_FX, BTN_ENV, BTN_EDIT, BTN_LFO, BTN_GLO, BTN_SEQ};
static const char *const PAGE_NAME[NPAGE] = {"SKY", "EFFECTS", "PAD SHAPE", "PAD TONE", "MOTION", "DRONE", "MACHINE"};
enum { KM_PADS, KM_HOLD, KM_BELLS, NKMODE };
static const char *const KM_NAME[NKMODE] = {"PADS", "HOLD", "BELLS"};

static volatile int32_t par[NPARAM];            /* 0 .. 127 */
static volatile int32_t g_scene, g_scale, g_root, g_oct, g_run = 1, g_freeze;
static volatile uint32_t g_casc;                /* falls of bells asked for (ARP) */

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
        int32_t pc = ((note + TRY[t] - g_root) % 12 + 12) % 12;
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
    kb.sweep = 4 + par[P_DRIFT] / 10 + par[P_SWEEP] * 28 / 127;
    kb.width = par[P_WIDTH];
    kb.trem = par[P_TREMOLO] * 26000 / 127;
}

#define PAD_VOL 7400                            /* a voice at full level (Q15) */
static void pad_render(pv_t *v, uint32_t vi)
{
    int32_t bright = par[P_BRIGHT], drift = par[P_DRIFT], echo = par[P_ECHO];
    int32_t e, a0 = v->a_prev, a1, vib, spread = kb.spread, p16, gl, gr, pan, ds, shk = kb.shk;
    uint32_t inc1, inc2, inc3, ph1 = v->ph1, ph2 = v->ph2, ph3 = v->ph3, i;
    int32_t ic1 = v->ic1, ic2 = v->ic2, wave = kb.wave, ls;
    tsvf_t c;
    if (v->gate) {
        v->env += v->atk;
        if (v->env > 1 << 24) v->env = 1 << 24;
        if (v->hold > 0 && --v->hold == 0) v->gate = 0;
    } else {
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
typedef struct { uint32_t pc, pm, ic, im; int32_t amp, a_prev, idx, kamp, pan; } bell_t;
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
    b->amp = 5000 + vel * 52;
    b->idx = clamp(13000 + (int32_t)arnd_n(22000) - (note - 72) * 550, 4000, 40000);
    b->kamp = note > 86 ? 7 : 8;                /* ~0.75 s or ~1.5 s to 1/e, then the echo and the reverb */
    b->pan = 30 + (int32_t)arnd_n(196);
    vis_bell_note = note;
    vis_bell_kind = kind;
    vis_bell_seq = vis_bell_seq + 1u;
}
static void bell_render(bell_t *b)
{
    int32_t a0 = b->a_prev, a1 = b->amp, idx = b->idx, gl = SINE[256 - b->pan], gr = SINE[b->pan];
    int32_t ds = 70 + par[P_ECHO] * 150 / 127;
    uint32_t pc = b->pc, pm = b->pm, i;
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

/* the drone: the root, low: a sine, two saws behind a dark filter (TONE), its fifth and octave coming and going (FIFTH),
 * a sine an octave under it (SUB), and its level breathing (BREATH) */
static struct { uint32_t p0, p1, p2, p5, p8, ps, l1, l2, l3, lb; int32_t p16, ic1, ic2, a_prev; } dr;
static void drone_render(void)
{
    int32_t lvl = par[P_DRONE], root = g_root, note = (root <= 5 ? 36 : 24) + root, tgt = note * 16;
    int32_t a0 = dr.a_prev, a1 = (lvl * lvl * 9200) >> 14, f5, f8, cut, d, fifth = par[P_FIFTH], sub = par[P_SUB] * 160;
    int32_t br = par[P_BREATH];
    uint32_t i0, i1, i2, i5, i8, is, i;
    tsvf_t c;
    dr.lb += 2900000u + (uint32_t)br * 9000u;   /* BREATH: its level swells and sinks, every ~9 .. ~5 s, deeper */
    if (br) a1 = (a1 * (32768 - ((br * 220 * ((32767 + sin15(dr.lb)) >> 1)) >> 15))) >> 15;
    a1 = a0 + (a1 - a0) / 48 + (a1 > a0) - (a1 < a0);   /* (its level moves over ~0.3 s: no step at switch-on or a new scene) */
    vis_drone = a1;
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
    cut = ((30 + par[P_DTONE] * 40 / 127 + par[P_BRIGHT] * 10 / 127) << 8) + ((sin15(dr.l3 * 9u) * 6) >> 7);
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
    int32_t lvl = par[P_WIND], a0 = wn.a_prev, a1, gl, gr, pan;
    uint32_t i;
    tsvf_t c;
    wn.l1 += 1083000u, wn.l2 += 1468000u, wn.l3 += 803000u;   /* cycles of 23, 17, 31 s */
    a1 = (((lvl * lvl) >> 2) * (11000 + ((sin15(wn.l2) + sin15(wn.l2 * 3u + wn.l1)) * 5000 >> 15))) >> 15;
    if (a1 < 0) a1 = 0;
    a1 = a0 + (a1 - a0) / 48 + (a1 > a0) - (a1 < a0);
    if (!a0 && !a1) return;
    tsvf_coef_k(&c, (72 << 8) + ((sin15(wn.l1) * 19) >> 7) + ((sin15(wn.l1 * 5u) * 5) >> 7) + par[P_BRIGHT] * 20, 2000);
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

/* the echo: two lines, each feeding the other (left, then right, then left), darker each time round */
#define DL_N 32768u
#define DL_L 22050u                             /* 0.5 s */
#define DL_R 16538u                             /* 0.375 s */
static int16_t dl_l[DL_N] __attribute__((section(".pool")));
static int16_t dl_r[DL_N] __attribute__((section(".pool")));
static uint32_t dl_w;
static int32_t dl_fl, dl_fr;
static void echo_render(void)
{
    int32_t echo = par[P_ECHO], fb = 9000 + echo * 12500 / 127, ret = 9000 + echo * 15000 / 127;
    uint32_t w = dl_w, i;
    int32_t fl = dl_fl, fr = dl_fr;
    for (i = 0; i < VB; i++) {
        int32_t ol = dl_l[(w - DL_L) & (DL_N - 1u)], or_ = dl_r[(w - DL_R) & (DL_N - 1u)];
        fl += (ol - fl) / 4;                    /* (towards zero: a quiet tail ends) */
        fr += (or_ - fr) / 4;
        dl_l[w] = (int16_t)clamp(bus_dly[i] + ((fr * fb) >> 15), -32767, 32767);
        dl_r[w] = (int16_t)clamp((fl * fb) >> 15, -32767, 32767);
        w = (w + 1u) & (DL_N - 1u);
        bus_l[i] += (ol * ret) >> 15;
        bus_r[i] += (or_ * ret) >> 15;
        bus_rev[i] += ((ol + or_) * 60) >> 8;
    }
    dl_w = w, dl_fl = fl, dl_fr = fr;
}

/* the machine: what plays the pads and the bells when nobody does */
static struct {
    int32_t t_pad, t_bell, t_chord, deg, chord;
    int32_t t_next, next_note, casc, casc_deg;  /* a bell's answer; a fall of bells under way */
    int32_t intro;                              /* the first notes after switch-on come quickly */
} gn = {30, 700, 5000, 2, 0, 0, 0, 0, 0, 3};
static uint32_t gn_casc_seen;

static int32_t pad_base(void) { return 48 + g_root + 12 * (g_oct + SCENES[g_scene].reg); }
static void gen_pad(void)
{
    const scale_t *s = &SCALES[g_scale];
    static const int8_t STEP[8] = {-2, -1, -1, 1, 1, 2, 3, -3};
    static const int8_t TONE[3] = {0, 2, 4};
    int32_t dens = par[P_DENSITY], n = (int32_t)s->n, top = range_top(s), note = 0, most = 1 + par[P_VOICES] * 5 / 127;
    uint32_t k, t, sounding = 0;
    pv_t *v;
    for (k = 0; k < NV; k++) sounding += pv[k].gate && pv[k].id < 0;
    if ((int32_t)sounding >= most || !(v = pad_free())) return;
    for (t = 0; t < 4u; t++) {
        int32_t deg, dup = 0;
        if (arnd_n(100) < 58u) {
            deg = gn.chord + TONE[arnd_n(3)] + n * (int32_t)arnd_n(2);
        } else {
            gn.deg += STEP[arnd_n(8)];
            if (gn.deg < 0) gn.deg = -gn.deg;
            if (gn.deg > top) gn.deg = 2 * top - gn.deg;
            deg = gn.deg;
        }
        note = pad_base() + scale_note(s, deg);
        while (note > 88) note -= 12;
        for (k = 0; k < NV; k++) dup |= (pv[k].gate || pv[k].env) && pv[k].note == note;
        if (!dup) break;
    }
    pad_start(v, note, -1, BLK(((4200 + (int32_t)arnd_n(6500) + (127 - dens) * 30) * len256_of()) >> 8), atk10_of());
}
static void gen_bell(void)
{
    const scale_t *s = &SCALES[g_scale];
    int32_t n = (int32_t)s->n, deg = (int32_t)arnd_n((uint32_t)(2 * n + 1));
    int32_t base = 72 + g_root + 12 * g_oct;
    bell_strike(base + scale_note(s, deg), 50 + (int32_t)arnd_n(70), 0);
    if ((int32_t)arnd_n(127) < par[P_ANSWER]) { /* ANSWER: sometimes an answer, a step or two up */
        gn.t_next = BLK(140 + (int32_t)arnd_n(380));
        gn.next_note = base + scale_note(s, deg + 1 + (int32_t)arnd_n(2));
    }
}
static void gen_tick(void)                      /* once a block */
{
    int32_t dens = par[P_DENSITY], bells = par[P_BELLS], base, mean;
    while (ev_r != ev_w) {                      /* the keys and MIDI first */
        uint32_t e = evq[ev_r % 32u], id = e >> 16, k;
        int32_t note = (int32_t)(e >> 8 & 127u);
        ev_r = ev_r + 1u;
        if (e & 2u) {
            if (e & 1u) bell_strike(note, 100, 2);
        } else if (e & 1u) {
            pad_start(pad_take(), note, (int32_t)id, 0, (atk10_of() + 2) / 3);
        } else {
            for (k = 0; k < NV; k++)
                if (pv[k].gate && pv[k].id == (int32_t)id) pv[k].gate = 0;
        }
    }
    if (g_casc != gn_casc_seen) {               /* ARP: a fall of bells from the top of the scale */
        gn_casc_seen = g_casc;
        gn.casc = 6, gn.casc_deg = 2 * (int32_t)SCALES[g_scale].n + 2, gn.t_next = 1, gn.next_note = 0;
    }
    if (gn.t_next > 0 && --gn.t_next == 0) {
        if (gn.casc > 0) {
            const scale_t *s = &SCALES[g_scale];
            bell_strike(72 + g_root + 12 * g_oct + scale_note(s, gn.casc_deg), 60 + gn.casc * 10, 1);
            gn.casc_deg -= 1 + (int32_t)arnd_n(2);
            if (gn.casc_deg < 0) gn.casc_deg = 0;
            if (--gn.casc > 0) gn.t_next = BLK(110 + (int32_t)arnd_n(90));
        } else if (gn.next_note) {
            bell_strike(gn.next_note, 40 + (int32_t)arnd_n(50), 0);
            gn.next_note = 0;
        }
    }
    if (!g_run) return;
    if (--gn.t_pad <= 0) {                      /* DENSITY: a new pad note every ~14 s .. ~0.9 s */
        base = 14000 >> (dens >> 5);
        mean = base - ((base >> 1) * (dens & 31) >> 5);
        gn.t_pad = BLK(mean / 2 + (int32_t)arnd_n((uint32_t)mean));
        if (gn.intro > 0) gn.intro--, gn.t_pad = BLK(700 + (int32_t)arnd_n(900));
        gen_pad();
    }
    if (--gn.t_bell <= 0) {                     /* BELLS: off, or one every ~12 s .. ~0.9 s (more with DENSITY) */
        mean = 12000 - bells * 86 - dens * 6;
        gn.t_bell = BLK(mean / 2 + (int32_t)arnd_n((uint32_t)mean));
        if (bells > 0 && !gn.casc) gen_bell();
    }
    if (--gn.t_chord <= 0) {                    /* CHORDS: the harmony moves: the pads lean on another degree a while */
        static const int8_t CH[6] = {0, 0, 3, 4, 5, 2};
        int32_t mean_c = 60000 - par[P_CHORDS] * 52000 / 127;   /* every ~60 s .. ~8 s */
        gn.t_chord = BLK(mean_c / 2 + (int32_t)arnd_n((uint32_t)mean_c));
        gn.chord = CH[arnd_n(6)];
        vis_chord = gn.chord;
        vis_chord_seq = vis_chord_seq + 1u;
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
    int32_t m = clamp(fm1_master(), 0, 4092), pk = 0, space = par[P_SPACE];
    if (n != VB) return;
    a_blk++;
    for (i = 0; i < VB; i++) bus_l[i] = bus_r[i] = bus_rev[i] = bus_dly[i] = 0;
    kb_update();
    gen_tick();
    for (k = 0; k < NV; k++) pad_render(&pv[k], k);
    for (k = 0; k < NB; k++) bell_render(&bell[k]);
    drone_render();
    wind_render();
    echo_render();
    rv_size = g_freeze ? RV_INF : 46 + space * 80 / 127;   /* SPACE: ~2 s .. ~30 s; FREEZE: for ever */
    rv_damp = 74 - par[P_BRIGHT] * 40 / 127;
    rv_mix = 50 + space * 77 / 127;
    rv_pre = 18;
    hall_render(bus_rev, out, VB);              /* (out comes zeroed: the reverb goes straight in) */
    for (i = 0; i < VB; i++) {                  /* the mix: a soft ceiling, then MASTER, into the DAC's 24-bit words */
        int32_t l = soft_knee(clamp(bus_l[i] + out[2u * i], -400000, 400000), 15500);
        int32_t r = soft_knee(clamp(bus_r[i] + out[2u * i + 1u], -400000, 400000), 15500);
        int32_t a = l < 0 ? -l : l, b = r < 0 ? -r : r;
        if (a > pk) pk = a;
        if (b > pk) pk = b;
        out[2u * i] = (l * m) >> 5;
        out[2u * i + 1u] = (r * m) >> 5;
    }
    vis_out = pk;
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
static uint32_t page, kmode, page_t;            /* the page open, the keys' mode, when the page opened */

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
#define NDOT 32u
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
    oc_n = 0, oc_refl = 0, ndot = 0, nseg = 0, nring = 0, glow_horizon = 0, star_gain = 256;
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
                if (d2 <= 1) v = 255;
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
    if (ov_page == PG_FX && f > 0) {
        for (k = 0; k < NRIP; k++) {            /* the ripples: rings spreading on the water, flattened */
            uint32_t age;
            int32_t r, band, inv, a, cx;
            if (!rip[k].on) continue;
            age = now - rip[k].t0;
            if ((int32_t)age < 0) continue;
            if (age > 2600u) { rip[k].on = 0; continue; }
            r = 3 + (int32_t)age / 30, band = 2 * r * 2, inv = 65536 / band;
            a = (rip[k].a * (int32_t)(2600u - age) / 2600 * f) >> 8;
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
    v_run_lvl += g_run ? (256 - v_run_lvl + 15) / 16 : (90 - v_run_lvl) / 24;   /* waiting: the aurora sinks low */
    ov_fade += page != PG_HOME && page == ov_page ? (256 - ov_fade + 3) / 4 : -(ov_fade + 3) / 4;
    ov_fade = clamp(ov_fade, 0, 256);
    if (ov_fade == 0) ov_page = page;
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
    char val[8];
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
        else if (hud_mode == HUD_KNOB && hud_param < NPARAM) put_u(val, (uint32_t)par[hud_param]), tl = P_NAME[hud_param], tr = val;
        else if (page != PG_HOME) tl = PAGE_NAME[page], tr = kmode != KM_PADS ? KM_NAME[kmode] : g_freeze ? "FROZEN" : "", cl = C_WHITE;
        else if (kmode != KM_PADS || g_freeze) tl = kmode != KM_PADS ? KM_NAME[kmode] : "", tr = g_freeze ? "FROZEN" : "";
        if (tl) cv_text(6, 4, &FONT_S, tl, cl);
        if (tr) cv_text(234 - text_w(&FONT_S, tr), 4, &FONT_S, tr, g_freeze && tr[0] == 'F' && tr[1] == 'R' ? RGB(190, 235, 255) : C_WHITE);
        if (hud_mode == HUD_KNOB && hud_param < NPARAM) cv_rect(6, 21, 2 + par[hud_param] * 226 / 127, 2, C_WHITE);
    }
    if ((hud_mode == HUD_KNOB || page != PG_HOME) && y0 == 240u - STRIP) {   /* the four knobs of this page */
        uint32_t k, pg = page != PG_HOME ? page : hud_param / 4u;
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
    int32_t dim_top = hud_mode != HUD_OFF || page != PG_HOME || kmode != KM_PADS || g_freeze;
    int32_t dim_bot = hud_mode == HUD_KNOB || page != PG_HOME, ice = v_ice;
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
#define SET_MAGIC 0x32525541u                   /* "AUR2" */
typedef struct { uint32_t magic; uint8_t scene, scale, root, oct, run, kmode, par[NPARAM]; } set_t;

static void scene_load(uint32_t s, int with_sets)   /* a scene: its scale, its root and where its knobs start */
{
    const scene_t *sc;
    uint32_t k;
    g_scene = (int32_t)(s % NSCENE);
    sc = &SCENES[g_scene];
    if (!with_sets) return;
    g_scale = sc->scale, g_root = sc->root;
    for (k = 0; k < NPARAM; k++) par[k] = k < 8u ? sc->def[k] : P_DEF[k];
    par[P_ATTACK] = (sc->atk - 1) * 127 / 79;
    par[P_RELEASE] = (sc->rel - 5) * 127 / 155;
    par[P_WAVE] = sc->wave;
    par[P_SHIMMER] = sc->shim;
}
static void hud_status(uint32_t ms)
{
    char l[32], r[20], *q;
    q = put_s(l, SCENES[g_scene].name);
    q = put_s(r, NOTE_NAME[g_root]);
    q = put_s(q, " ");
    put_s(q, SCALES[g_scale].name);
    hud_msg(l, r, ms);
}

static void app_main(void)
{
    uint32_t prev_keys = 0, k, t_led = 0, latched = 0, sel_used = 0;
    int32_t key_note[27];
    set_t st;
    for (k = 0; k < 27u; k++) key_note[k] = -1;
    scene_load(0, 1);
    if (settings_load(&st, sizeof st) == 0 && st.magic == SET_MAGIC && st.scene < NSCENE && st.scale < NSCALE) {
        scene_load(st.scene, 0);
        g_scale = st.scale, g_root = st.root % 12u, g_oct = clamp((int32_t)st.oct - 1, -1, 1), g_run = st.run != 0;
        kmode = st.kmode < NKMODE ? st.kmode : KM_PADS;
        for (k = 0; k < NPARAM; k++) par[k] = st.par[k] & 127;
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
        for (k = 0; k < 4u; k++)
            if ((d = fm1_enc_take(ENC_KNOB1 + k)) != 0) {
                uint32_t p = page * 4u + k;
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
            hud_msg("ROOT", NOTE_NAME[g_root], 2600);
        }

        /* the buttons */
        pressed = fm1_input_edges(&released);
        for (k = 0; k < NPAGE; k++)             /* a page's button: that page; again: back to the sky */
            if (pressed & 1u << PAGE_BTN[k]) {
                page = page == k ? PG_HOME : k;
                page_t = now;
                if (page == PG_HOME) hud_status(3000);
                else hud_mode = HUD_OFF;
            }
        if (pressed & 1u << BTN_PLAY) g_run = !g_run, hud_msg("THE MACHINE", g_run ? "PLAYS" : "WAITS", 2200);
        if (pressed & 1u << BTN_REC) g_freeze = !g_freeze, hud_msg("FREEZE", g_freeze ? "ON" : "OFF", 2200);
        if (pressed & 1u << BTN_ARP) g_casc = g_casc + 1u, hud_msg("A FALL OF BELLS", "", 1800);
        if (pressed & 1u << BTN_SEL) sel_used = 0;
        if (released & 1u << BTN_SEL && !sel_used) {   /* SEL tapped: the keys' next mode */
            if (kmode == KM_HOLD)               /* (leaving HOLD lets the held notes go) */
                for (k = 0; k < 27u; k++)
                    if (latched >> k & 1u) ev_put(64u + k, key_note[k], 0), key_note[k] = -1;
            latched = 0;
            kmode = (kmode + 1u) % NKMODE;
            hud_msg("KEYS", KM_NAME[kmode], 2200);
        }
        if (pressed & (1u << BTN_OCT_UP | 1u << BTN_OCT_DOWN)) {
            char r[4];
            if ((fm1_in.buttons >> BTN_OCT_UP & 1u) && (fm1_in.buttons >> BTN_OCT_DOWN & 1u)) g_oct = 0;
            else g_oct = clamp(g_oct + (pressed & 1u << BTN_OCT_UP ? 1 : -1), -1, 1);
            r[0] = g_oct < 0 ? '-' : g_oct > 0 ? '+' : ' ', r[1] = (char)('0' + (g_oct < 0 ? -g_oct : g_oct)), r[2] = 0;
            hud_msg("OCTAVE", r, 2200);
        }
        if (pressed & 1u << BTN_SAVE) {
            st.magic = SET_MAGIC, st.scene = (uint8_t)g_scene, st.scale = (uint8_t)g_scale, st.root = (uint8_t)g_root;
            st.oct = (uint8_t)(g_oct + 1), st.run = (uint8_t)g_run, st.kmode = (uint8_t)kmode;
            for (k = 0; k < NPARAM; k++) st.par[k] = (uint8_t)par[k];
            hud_msg("POWER-ON STATE", settings_save(&st, sizeof st) == 0 ? "SAVED" : "FAILED", 2600);
        }

        /* the keys: pad notes in the scale (PADS), latched (HOLD), or bells (BELLS); SCL held: the root */
        edges = fm1_input_note_edges();
        keys = fm1_in.notes;
        down = (keys | edges) & ~prev_keys, up = prev_keys & ~keys & ~edges;
        prev_keys = keys | edges;
        for (k = 0; k < 27u; k++) {
            if (down >> k & 1u) {
                int32_t note = scale_snap(53 + (int32_t)k + 12 * g_oct);
                if (fm1_in.buttons >> BTN_SEL & 1u) {
                    g_root = (int32_t)((5u + k) % 12u);
                    sel_used = 1;
                    hud_msg("ROOT", NOTE_NAME[g_root], 2600);
                } else if (kmode == KM_BELLS) {
                    ev_put(k, note + 12, 3);
                } else if (kmode == KM_HOLD) {
                    if (latched >> k & 1u) ev_put(64u + k, key_note[k], 0), latched &= ~(1u << k), key_note[k] = -1;
                    else key_note[k] = note, ev_put(64u + k, note, 1), latched |= 1u << k;
                } else {
                    key_note[k] = note;
                    ev_put(k, note, 1);
                }
            }
            if ((up >> k & 1u) && !(latched >> k & 1u) && key_note[k] >= 0) ev_put(k, key_note[k], 0), key_note[k] = -1;
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
            for (k = 0; k < 27u; k++) fm1_led_key(KEY_LED(k), (int)(lit >> k & 1u));
            for (k = 0; k < NPAGE; k++) fm1_led_key(PAGE_BTN[k], page == k);
            fm1_led_key(BTN_PLAY, g_run);
            fm1_led_key(BTN_REC, g_freeze);
            fm1_led_key(BTN_ARP, v_flash > 60);
            fm1_led_key(BTN_SEL, (int)(fm1_in.buttons >> BTN_SEL & 1u) || (kmode == KM_HOLD) || (kmode == KM_BELLS && (now & 512u)));
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
