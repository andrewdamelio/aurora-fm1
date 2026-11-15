/* SPDX-License-Identifier: GPL-3.0-only */
/* AURORA: an ambient machine for the M-VAVE FM-1. It plays by itself: slow pads that drift through a scale over a
 * drone, bells that fall into a long echo, wind, all inside a very large reverb; the screen is the sky it plays under.
 *
 * Built on GR!MM's tree (github.com/GRiMMBIENT/grimm, GPL-3.0), used unchanged: the platform (platform.c: Dinghy's, by
 * zvenson, on Felucca's / SLOOP's FM-1 work; in a browser fm1_web.c), the canvas (gfx.c), Felucca's fixed-point DSP
 * (dsp.c: the oscillators, the filter, the tables) and GR!MM's SPACE reverb (hall.c, Felucca's HALL). Everything in this
 * file is AURORA's own. Integers only, no 64-bit division: the chip's rules (docs/RULES.md).
 *
 *   KNOB 1..4    DENSITY, BRIGHT, SPACE, DRIFT;  FX: the second row: ECHO, BELLS, WIND, DRONE (FX lit)
 *   ALGORITHM    the scene (its sound, its sky, its scale, its settings)      SELECT   the scale
 *   PRESETS      the root note                                                OCT-/+   the register
 *   keys         play pad notes, snapped to the scale; SCL held + a key: that key is the root
 *   PLAY         the machine plays / waits (lit: playing)      REC    FREEZE: the reverb holds what is in it (lit)
 *   ARP          a fall of bells now                           HOME   the first row of knobs, and what is set
 *   SAVE         keep all this as the power-on state
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
enum { P_DENSITY, P_BRIGHT, P_SPACE, P_DRIFT, P_ECHO, P_BELLS, P_WIND, P_DRONE, NPARAM };
static const char *const P_NAME[NPARAM] = {"DENSITY", "BRIGHT", "SPACE", "DRIFT", "ECHO", "BELLS", "WIND", "DRONE"};
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
 * register), where its knobs start, and its sky: the curtains' colours from their lower edge up, the sky at the top
 * and at the horizon */
typedef struct {
    const char *name;
    uint8_t scale, root, wave, atk, rel;
    int8_t reg;
    uint8_t def[NPARAM];
    uint8_t c[3][3], sky[2][3];
} scene_t;
static const scene_t SCENES[] = {
    {"BOREAL", 0, 2, 58, 28, 70, 0, {52, 64, 92, 56, 60, 44, 30, 78},
     {{70, 255, 140}, {30, 205, 215}, {150, 70, 235}}, {{2, 4, 14}, {8, 20, 38}}},
    {"EMBER", 4, 9, 96, 40, 90, -1, {40, 46, 84, 40, 48, 26, 46, 96},
     {{255, 190, 60}, {255, 86, 40}, {170, 30, 120}}, {{9, 3, 8}, {40, 14, 14}}},
    {"ABYSS", 5, 0, 26, 50, 110, -1, {28, 38, 112, 70, 78, 34, 58, 104},
     {{60, 140, 255}, {74, 70, 235}, {150, 50, 210}}, {{1, 2, 10}, {4, 10, 34}}},
    {"HALO", 2, 5, 14, 22, 80, 1, {66, 84, 100, 48, 84, 92, 16, 56},
     {{255, 226, 240}, {255, 120, 200}, {140, 110, 255}}, {{8, 4, 16}, {34, 16, 40}}},
    {"GLACIER", 3, 7, 0, 34, 100, 0, {46, 72, 106, 30, 70, 62, 40, 70},
     {{170, 255, 250}, {60, 190, 255}, {60, 90, 235}}, {{2, 6, 16}, {10, 30, 48}}},
    {"NOCTURNE", 8, 4, 44, 36, 96, 0, {44, 54, 98, 84, 66, 50, 34, 88},
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

/* what the screen shows of the sound (the audio writes, the main loop reads) */
#define NV 6u                                   /* the pads' voices */
static volatile int32_t vis_lvl[NV], vis_note[NV];   /* each voice: 0 .. 255, its note */
static volatile int32_t vis_out;                /* the output's peak over the last block (Q15) */
static volatile uint32_t vis_bell_seq;          /* bells struck so far */
static volatile int32_t vis_bell_note;

/* notes from the keys and MIDI: id << 16 | note << 8 | on */
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
    int32_t atk, rel;                           /* Q24 a block */
    int32_t gate, hold, note, id, pan, det, wave;   /* hold: blocks until it lets go (0: until its key does) */
    uint32_t born;
} pv_t;
static pv_t pv[NV];
static uint32_t a_blk;                          /* blocks so far */

#define PAD_VOL 7400                            /* a voice at full level (Q15) */
static void pad_render(pv_t *v, uint32_t vi)
{
    int32_t bright = par[P_BRIGHT], drift = par[P_DRIFT], echo = par[P_ECHO];
    int32_t e, a0 = v->a_prev, a1, vib, spread, p16, gl, gr, pan, ds;
    uint32_t inc1, inc2, inc3, ph1 = v->ph1, ph2 = v->ph2, ph3 = v->ph3, i;
    int32_t ic1 = v->ic1, ic2 = v->ic2, wave = v->wave, ls;
    tsvf_t c;
    if (v->gate) {
        v->env += v->atk;
        if (v->env > 1 << 24) v->env = 1 << 24;
        if (v->hold > 0 && --v->hold == 0) v->gate = 0;
    } else {
        v->env -= v->rel;
        if (v->env < 0) v->env = 0;
    }
    e = v->env >> 9;
    a1 = (((e * e) >> 15) * PAD_VOL) >> 15;
    vis_lvl[vi] = (a1 * 255) / PAD_VOL;
    if (!a1 && !a0) { v->a_prev = 0; return; }
    v->lfo += v->lfo_inc;
    ls = sin15(v->lfo);
    vib = (ls * (2 + drift * 9 / 127)) >> 15;   /* cents: a slow lean, wider with DRIFT */
    spread = 3 + drift * 13 / 127;
    p16 = v->note * 16;
    inc1 = cents_inc(p16, v->det + vib + spread, 0);
    inc2 = cents_inc(p16, v->det + vib - spread, 0);
    inc3 = cents_inc(p16, v->det + vib, 0);
    tsvf_coef(&c, ((36 + bright * 62 / 127 + (v->note - 60) / 3) << 8) + ((sin15(v->lfo * 3u + 0x40000000u) * (4 + drift / 10)) >> 7)
                  + ((e * 10) >> 7), 18);
    pan = clamp(v->pan + ((sin15(v->lfo * 2u) * (10 + drift / 2)) >> 15), 0, 256);
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
    }
    v->ph1 = ph1, v->ph2 = ph2, v->ph3 = ph3, v->ic1 = ic1, v->ic2 = ic2, v->a_prev = a1;
}

static void pad_start(pv_t *v, int32_t note, int32_t id, int32_t hold_blk, int32_t atk10)
{
    const scene_t *sc = &SCENES[g_scene];
    v->note = clamp(note, 24, 108);
    v->id = id;
    v->hold = hold_blk;
    v->atk = 975419 / (atk10 < 1 ? 1 : atk10);  /* 2^24 over atk10 tenths of a second, a block */
    v->rel = 975419 / sc->rel;
    v->wave = sc->wave;
    v->pan = 36 + (int32_t)arnd_n(185);
    v->det = (int32_t)arnd_n(9) - 4;
    v->lfo_inc = 1000000u + arnd_n(1900000u);   /* a cycle in 9 .. 25 s */
    if (!v->a_prev) v->ph2 = arnd(), v->lfo = arnd(), v->ic1 = v->ic2 = 0;
    v->born = a_blk;
    v->gate = 1;
    vis_note[v - pv] = v->note;
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
static void bell_strike(int32_t note, int32_t vel)   /* vel 0 .. 127 */
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

/* the drone: the root, low: a sine, two saws behind a dark filter, and its fifth and octave coming and going */
static struct { uint32_t p0, p1, p2, p5, p8, l1, l2, l3; int32_t p16, ic1, ic2, a_prev; } dr;
static void drone_render(void)
{
    int32_t lvl = par[P_DRONE], root = g_root, note = (root <= 5 ? 36 : 24) + root, tgt = note * 16;
    int32_t a0 = dr.a_prev, a1 = (lvl * lvl * 9200) >> 14, f5, f8, cut, d;
    a1 = a0 + (a1 - a0) / 48 + (a1 > a0) - (a1 < a0);   /* (its level moves over ~0.3 s: no step at switch-on or a new scene) */
    uint32_t i0, i1, i2, i5, i8, i;
    tsvf_t c;
    if (!dr.p16) dr.p16 = tgt;
    d = tgt - dr.p16;                           /* a new root: it slides there */
    dr.p16 += d > 0 ? (d + 15) >> 4 : -((-d + 15) >> 4);
    if (!a0 && !a1) return;
    dr.l1 += 124000u, dr.l2 += 81000u, dr.l3 += 53000u;   /* cycles of 200, 308, 470 s / 2 pi.. slow enough */
    i0 = cents_inc(dr.p16, 0, 0);
    i1 = cents_inc(dr.p16 + 192, 7, 0), i2 = cents_inc(dr.p16 + 192, -7, 0);
    i5 = cents_inc(dr.p16 + 19 * 16, 2, 0), i8 = cents_inc(dr.p16 + 24 * 16, -3, 0);
    f5 = 9000 + ((sin15(dr.l1 * 5u) * 8000) >> 15);
    f8 = 7000 + ((sin15(dr.l2 * 7u) * 6500) >> 15);
    cut = ((40 + par[P_BRIGHT] * 22 / 127) << 8) + ((sin15(dr.l3 * 9u) * 6) >> 7);
    tsvf_coef(&c, cut, 30);
    for (i = 0; i < VB; i++) {
        int32_t s, a, y, w;
        dr.p0 += i0, dr.p1 += i1, dr.p2 += i2, dr.p5 += i5, dr.p8 += i8;
        w = tsvf_lp(&c, (osc_saw(dr.p1, i1) + osc_saw(dr.p2, i2)) >> 1, &dr.ic1, &dr.ic2);
        s = (sin15(dr.p0) * 18000 + w * 15000) >> 15;
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
    int32_t dens = par[P_DENSITY], n = (int32_t)s->n, top = 2 * n + 1, note = 0, most = 2 + dens * 4 / 127;
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
    pad_start(v, note, -1, BLK(4200 + (int32_t)arnd_n(6500) + (127 - dens) * 30), SCENES[g_scene].atk);
}
static void gen_bell(void)
{
    const scale_t *s = &SCALES[g_scale];
    int32_t n = (int32_t)s->n, deg = (int32_t)arnd_n((uint32_t)(2 * n + 1));
    int32_t base = 72 + g_root + 12 * g_oct;
    bell_strike(base + scale_note(s, deg), 50 + (int32_t)arnd_n(70));
    if (arnd_n(100) < 30u) {                    /* sometimes an answer, a step or two up */
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
        if (e & 1u) {
            pad_start(pad_take(), note, (int32_t)id, 0, 4);
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
            bell_strike(72 + g_root + 12 * g_oct + scale_note(s, gn.casc_deg), 60 + gn.casc * 10);
            gn.casc_deg -= 1 + (int32_t)arnd_n(2);
            if (gn.casc_deg < 0) gn.casc_deg = 0;
            if (--gn.casc > 0) gn.t_next = BLK(110 + (int32_t)arnd_n(90));
        } else if (gn.next_note) {
            bell_strike(gn.next_note, 40 + (int32_t)arnd_n(50));
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
    if (--gn.t_chord <= 0) {                    /* the harmony moves: the pads lean on another degree for a while */
        static const int8_t CH[6] = {0, 0, 3, 4, 5, 2};
        gn.t_chord = BLK(18000 + (int32_t)arnd_n(26000));
        gn.chord = CH[arnd_n(6)];
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
#define NMET 3u

static int16_t cu_y[NC][240];                   /* each curtain, each column: its lower edge's row, */
static uint8_t cu_i[NC][240];                   /* its light there, */
static uint16_t cu_inv[NC][240];                /* and 64 * 256 over its height in rows */
static uint8_t prof[64];                        /* the light up a curtain: brightest at its edge, fading */
static uint8_t ramp[16][3];                     /* its colour up from the edge */
static uint8_t sky_c[HZ][3];                    /* the sky behind, row by row */
static uint8_t tree_h[240];                     /* the far shore: its height over the horizon */
static int32_t pal_now[5][3];                   /* the palette on its way to the scene's (x 256): c0 c1 c2 sky0 sky1 */
static struct { uint8_t x, y, b, sp; uint16_t ph; uint8_t flash; } star[NSTAR];
static struct { int32_t x, y, dx, dy; uint32_t t0, on; } met[NMET];
static uint32_t v_t, v_last, v_frame_t, v_strip, v_bell_seen, v_boot;
static int32_t v_en;
static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

/* the HUD: what a knob just changed (and the row of knobs), or a line of news */
enum { HUD_OFF, HUD_KNOB, HUD_MSG };
static uint32_t hud_mode, hud_until, hud_param, hud_page;
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
    if ((uint32_t)x >= 240u || (uint32_t)y >= STRIP) return;
    p = &cv_px[(uint32_t)y * 240u + (uint32_t)x];
    c = swap16(*p);
    r += (c >> 11) << 3, g += (c >> 5 & 63) << 2, b += (c & 31) << 3;
    r = r > 255 ? 255 : r, g = g > 255 ? 255 : g, b = b > 255 ? 255 : b;
    *p = swap16((uint32_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3));
}

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

/* the curtains: where each one hangs (its edge a slow wave), how long, how bright, its rays bunching where it folds */
static const struct { int16_t base, a1, a2, len, wt; uint32_t w1, s1, w2, s2, h1, h2, s3, s4, wp, s5, sa; } CU[NC] = {
    {116, 17, 6, 88, 256, 14316557u, 116080u, 61356675u, 330382u, 477218588u, 186737708u, 613566u, 858993u, 16519105u, 104755u, 91382u},
    {96, 12, 5, 62, 205, 22605091u, 148102u, 78090314u, 252645u, 390451572u, 226050910u, 715827u, 536870u, 23860929u, 81037u, 70409u},
    {134, 9, 4, 44, 170, 17895697u, 81037u, 47721858u, 390451u, 536870912u, 165191049u, 505290u, 954437u, 19522579u, 126322u, 51746u},
};
static void frame_setup(uint32_t now)
{
    const scene_t *sc = &SCENES[g_scene];
    uint32_t k, x, dt = now - v_last, t;
    static uint8_t boost[240];
    int32_t en;
    v_last = now;
    if (dt > 250u) dt = 250u;
    v_t += dt * (uint32_t)(40 + par[P_DRIFT]) / 104u;   /* DRIFT: the sky moves at 0.4 .. 1.6 of its pace */
    t = v_t;
    for (k = 0; k < 5u; k++) {                  /* the colours ease to the scene's */
        const uint8_t *c = k < 3u ? sc->c[k] : sc->sky[k - 3u];
        for (x = 0; x < 3u; x++) {
            int32_t d = (c[x] << 8) - pal_now[k][x];
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
        cx = 20 + (clamp(vis_note[k], 36, 92) - 36) * 200 / 56;
        for (dx = -34; dx <= 34; dx++) {
            int32_t xx = cx + dx, g, s;
            if ((uint32_t)xx >= 240u) continue;
            g = (32767 + sin15((uint32_t)(dx + 34) * 63161284u - 0x40000000u)) >> 8;   /* a raised cosine, 0 .. 255 */
            s = boost[xx] + ((lvl * g) >> 8);
            boost[xx] = (uint8_t)(s > 255 ? 255 : s);
        }
    }
    for (k = 0; k < NC; k++) {
        int32_t act = 96 + ((sin15(t * CU[k].sa + k * 0x55555555u) * 64) >> 15) + v_en / 3;
        act = (clamp(act, 0, 255) * CU[k].wt) >> 8;
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
            len += (len * v_en) >> 10;
            len += (len * boost[x]) >> 10;
            cu_y[k][x] = (int16_t)(CU[k].base + disp);
            cu_i[k][x] = (uint8_t)(in > 255 ? 255 : in);
            cu_inv[k][x] = (uint16_t)(16384 / (len < 8 ? 8 : len));
        }
    }
    if (vis_bell_seq != v_bell_seen) {          /* a bell: a star flares; one in three, a shooting star */
        uint32_t s = vrnd_n(NSTAR);
        v_bell_seen = vis_bell_seq;
        star[s].flash = 255;
        if (vrnd_n(3) == 0u) {
            for (k = 0; k < NMET; k++)
                if (!met[k].on) {
                    met[k].on = 1, met[k].t0 = now;
                    met[k].x = 30 + (int32_t)vrnd_n(180), met[k].y = 6 + (int32_t)vrnd_n(60);
                    met[k].dx = (vrnd() & 1u ? 1 : -1) * (5 + (int32_t)vrnd_n(4)), met[k].dy = 3 + (int32_t)vrnd_n(3);
                    break;
                }
        }
    }
    for (k = 0; k < NSTAR; k++) star[k].flash = (uint8_t)(star[k].flash - (star[k].flash >> 3) - (star[k].flash > 0));
    for (k = 0; k < NMET; k++)
        if (met[k].on && now - met[k].t0 > 520u) met[k].on = 0;
}

static void hud_draw(uint32_t y0, uint32_t now)   /* the HUD's text into this strip */
{
    cv_oy = -(int32_t)y0;
    if (now - v_boot < 4200u) {                 /* switched on: the name, fading */
        uint32_t a = now - v_boot, f = a < 600u ? a * 255u / 600u : a > 3200u ? (4200u - a) * 255u / 1000u : 255u;
        if (y0 + STRIP > 70u && y0 < 124u) {
            cv_text((240 - text_w(&FONT_L, "AURORA")) / 2, 70, &FONT_L, "AURORA", RGB(f, f, f));
            cv_text((240 - text_w(&FONT_S, "ambient machine")) / 2, 106, &FONT_S, "ambient machine", RGB(f * 150u / 255u, f * 200u / 255u, f * 190u / 255u));
        }
    }
    if (hud_mode != HUD_OFF && y0 == 0u) {
        cv_text(6, 4, &FONT_S, hud_l, RGB(170, 215, 205));
        cv_text(234 - text_w(&FONT_S, hud_r), 4, &FONT_S, hud_r, C_WHITE);
        if (hud_mode == HUD_KNOB && hud_param < NPARAM)
            cv_rect(6, 21, 2 + par[hud_param] * 226 / 127, 2, C_WHITE);
    }
    if (hud_mode == HUD_KNOB && y0 == 240u - STRIP) {   /* the four knobs of this row, the one turned in white */
        uint32_t k;
        for (k = 0; k < 4u; k++) {
            uint32_t p = hud_page * 4u + k;
            const char *s = P_NAME[p];
            uint16_t c = p == hud_param ? C_WHITE : RGB(120, 160, 152);
            cv_text((int32_t)(k * 60u) + (60 - text_w(&FONT_S, s)) / 2, 218, &FONT_S, s, c);
            cv_rect((int32_t)(k * 60u) + 8, 236, 44, 2, RGB(40, 56, 54));
            cv_rect((int32_t)(k * 60u) + 8, 236, 1 + par[p] * 43 / 127, 2, c);
        }
    }
    cv_oy = 0;
}

static void strip_draw(uint32_t s, uint32_t now)
{
    uint32_t y0 = s * STRIP, yy, x, k, t = v_t;
    int32_t dim_top = hud_mode != HUD_OFF, dim_bot = hud_mode == HUD_KNOB;
    cv_begin(240, STRIP, C_BLACK);
    for (yy = 0; yy < STRIP; yy++) {
        int32_t y = (int32_t)(y0 + yy);
        uint16_t *p = &cv_px[yy * 240u];
        const uint8_t *bay = BAYER[y & 3];
        int32_t dim = (dim_top && y < 26) || (dim_bot && y >= 214);
        if (y < HZ) {
            for (x = 0; x < 240u; x++) {
                int32_t r, g, b;
                if (y >= HZ - tree_h[x]) { p[x] = 0; continue; }   /* the far shore */
                sky_px((int32_t)x, y, &r, &g, &b);
                if (dim) r = (r * 3) >> 3, g = (g * 3) >> 3, b = (b * 3) >> 3;
                p[x] = px_pack(r, g, b, bay[x & 3u]);
            }
        } else {                                /* the water: the sky upside down, squeezed, rippled, darker */
            int32_t dy = y - HZ, sy = HZ - 1 - ((dy * 7) >> 2);
            int32_t off = (sin15((uint32_t)dy * 286331153u + t * 1431655u) * (1 + dy / 11)) >> 15;
            int32_t wob = sin15((uint32_t)dy * 477218588u - t * 2386092u);
            for (x = 0; x < 240u; x++) {
                int32_t r, g, b, m;
                int32_t xs = (int32_t)x + off + ((sin15(x * 119304647u + (uint32_t)wob * 9000u) * 3) >> 16);
                xs = xs < 0 ? 0 : xs > 239 ? 239 : xs;
                if (HZ - 1 - sy < tree_h[xs]) { p[x] = px_pack(0, 1, 3, 0); continue; }   /* the shore in it */
                if (sy < 0) r = sky_c[0][0], g = sky_c[0][1], b = sky_c[0][2];
                else sky_px(xs, sy, &r, &g, &b);
                m = 150 + ((sin15((uint32_t)dy * 1073741824u / 3u + x * 29826162u + t * 1908874u) * 34) >> 15) - dy / 2;
                r = (r * m) >> 8, g = (g * m) >> 8, b = ((b * m) >> 8) + 3;
                if (dim) r = (r * 3) >> 3, g = (g * 3) >> 3, b = (b * 3) >> 3;
                p[x] = px_pack(r, g, b, bay[x & 3u]);
            }
        }
    }
    for (k = 0; k < NSTAR; k++) {               /* the stars, twinkling; a flare for a bell; the brightest in the water */
        int32_t sx = star[k].x, sy = star[k].y, tw, b, f = star[k].flash;
        if (sy >= HZ - 1 - tree_h[sx] || (dim_top && sy < 28)) continue;   /* (not behind the shore, nor the HUD's text) */
        tw = 190 + ((sin15((uint32_t)star[k].ph * 65536u + t * (uint32_t)star[k].sp * 110000u) * 65) >> 15);
        b = (star[k].b * tw) >> 8;
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
        if (b > 150 && sy > 44) {
            int32_t ry = HZ + ((HZ - 1 - sy) * 4) / 7;
            px_add(sx + ((sin15((uint32_t)ry * 286331153u + t * 1431655u) * 3) >> 15), ry, y0, b / 3, b / 3, b / 3);
        }
    }
    for (k = 0; k < NMET; k++) {                /* the shooting stars: a head and a fading tail */
        uint32_t age;
        int32_t hx, hy, j, fade;
        if (!met[k].on) continue;
        age = now - met[k].t0;
        if (age > 520u) continue;
        hx = met[k].x + met[k].dx * (int32_t)age / 28, hy = met[k].y + met[k].dy * (int32_t)age / 28;
        fade = age < 360u ? 255 : (int32_t)(520u - age) * 255 / 160;
        for (j = 0; j < 22; j++) {
            int32_t b = ((22 - j) * fade) / 22;
            b = (b * b) >> 8;
            px_add(hx - met[k].dx * j / 7, hy - met[k].dy * j / 7, y0, b, b, (b * 5) >> 2);
        }
    }
    hud_draw(y0, now);
    cv_blit(0, y0);
}

/* ------------------------------------------------------------------ the panel (the main loop) */
#define SET_MAGIC 0x31525541u                   /* "AUR1" */
typedef struct { uint32_t magic; uint8_t scene, scale, root, oct, run, par[NPARAM]; } set_t;

static void scene_load(uint32_t s, int with_sets)   /* a scene: its scale, its root and where its knobs start */
{
    uint32_t k;
    g_scene = (int32_t)(s % NSCENE);
    if (!with_sets) return;
    g_scale = SCENES[g_scene].scale, g_root = SCENES[g_scene].root;
    for (k = 0; k < NPARAM; k++) par[k] = SCENES[g_scene].def[k];
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
    uint32_t prev_keys = 0, page = 0, k, t_led = 0, held_id[27] = {0};
    int32_t key_note[27];
    set_t st;
    for (k = 0; k < 27u; k++) key_note[k] = -1;
    scene_load(0, 1);
    if (settings_load(&st, sizeof st) == 0 && st.magic == SET_MAGIC && st.scene < NSCENE && st.scale < NSCALE) {
        scene_load(st.scene, 0);
        g_scale = st.scale, g_root = st.root % 12u, g_oct = clamp((int32_t)st.oct - 1, -1, 1), g_run = st.run != 0;
        for (k = 0; k < NPARAM; k++) par[k] = st.par[k] & 127;
    }
    v_seed ^= fm1_ticks();
    sky_init();
    v_boot = v_last = fm1_ms;
    hud_status(5200);
    (void)held_id;

    for (;;) {
        uint32_t now, pressed, released, keys, edges, down, up;
        int32_t d;
        platform_poll();
        now = fm1_ms;

        /* the knobs */
        for (k = 0; k < 4u; k++)
            if ((d = fm1_enc_take(ENC_KNOB1 + k)) != 0) {
                uint32_t p = page * 4u + k;
                char r[8];
                par[p] = clamp(par[p] + d * 4, 0, 127);
                put_u(r, (uint32_t)par[p]);
                hud_msg(P_NAME[p], r, 2600);
                hud_mode = HUD_KNOB, hud_param = p, hud_page = page;
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
        if (pressed & 1u << BTN_PLAY) g_run = !g_run, hud_msg("THE MACHINE", g_run ? "PLAYS" : "WAITS", 2200);
        if (pressed & 1u << BTN_REC) g_freeze = !g_freeze, hud_msg("FREEZE", g_freeze ? "ON" : "OFF", 2200);
        if (pressed & 1u << BTN_FX) {
            page ^= 1u;
            hud_msg(page ? "KNOBS: ROW 2" : "KNOBS: ROW 1", "", 2600);
            hud_mode = HUD_KNOB, hud_param = 99u, hud_page = page;   /* (the row's names; none of them turned) */
        }
        if (pressed & 1u << BTN_HOME) page = 0, hud_status(4000);
        if (pressed & 1u << BTN_ARP) g_casc = g_casc + 1u;
        if (pressed & (1u << BTN_OCT_UP | 1u << BTN_OCT_DOWN)) {
            char r[4];
            if ((fm1_in.buttons >> BTN_OCT_UP & 1u) && (fm1_in.buttons >> BTN_OCT_DOWN & 1u)) g_oct = 0;
            else g_oct = clamp(g_oct + (pressed & 1u << BTN_OCT_UP ? 1 : -1), -1, 1);
            r[0] = g_oct < 0 ? '-' : g_oct > 0 ? '+' : ' ', r[1] = (char)('0' + (g_oct < 0 ? -g_oct : g_oct)), r[2] = 0;
            hud_msg("OCTAVE", r, 2200);
        }
        if (pressed & 1u << BTN_SAVE) {
            st.magic = SET_MAGIC, st.scene = (uint8_t)g_scene, st.scale = (uint8_t)g_scale, st.root = (uint8_t)g_root;
            st.oct = (uint8_t)(g_oct + 1), st.run = (uint8_t)g_run;
            for (k = 0; k < NPARAM; k++) st.par[k] = (uint8_t)par[k];
            hud_msg("POWER-ON STATE", settings_save(&st, sizeof st) == 0 ? "SAVED" : "FAILED", 2600);
        }

        /* the keys: pad notes in the scale; SCL held: the root */
        edges = fm1_input_note_edges();
        keys = fm1_in.notes;
        down = (keys | edges) & ~prev_keys, up = prev_keys & ~keys & ~edges;
        prev_keys = keys | edges;
        for (k = 0; k < 27u; k++) {
            if (down >> k & 1u) {
                if (fm1_in.buttons >> BTN_SEL & 1u) {
                    g_root = (int32_t)((5u + k) % 12u);
                    hud_msg("ROOT", NOTE_NAME[g_root], 2600);
                } else {
                    key_note[k] = scale_snap(53 + (int32_t)k + 12 * g_oct);
                    ev_put(k, key_note[k], 1);
                }
            }
            if ((up >> k & 1u) && key_note[k] >= 0) ev_put(k, key_note[k], 0), key_note[k] = -1;
        }
        {                                       /* MIDI in: notes play the pads too (as they come: not snapped) */
            uint8_t s, d1, d2;
            while (midi_get(&s, &d1, &d2)) {
                if ((s & 0xF0u) == 0x90u && d2) ev_put(32u + d1, d1, 1);
                else if ((s & 0xF0u) == 0x80u || (s & 0xF0u) == 0x90u) ev_put(32u + d1, d1, 0);
            }
        }

        /* the lights: the buttons' states; the keys of the notes that sound */
        if (now - t_led >= 40u) {
            uint32_t lit = 0;
            t_led = now;
            for (k = 0; k < NV; k++)
                if (vis_lvl[k] > 24) {
                    int32_t kk = vis_note[k] - 53 - 12 * g_oct;
                    while (kk < 0) kk += 12;
                    while (kk > 26) kk -= 12;
                    lit |= 1u << kk;
                }
            for (k = 0; k < 27u; k++) fm1_led_key(KEY_LED(k), (int)(lit >> k & 1u));
            fm1_led_key(BTN_PLAY, g_run);
            fm1_led_key(BTN_REC, g_freeze);
            fm1_led_key(BTN_FX, (int)page);
            fm1_led_key(BTN_SEL, (int)(fm1_in.buttons >> BTN_SEL & 1u));
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
