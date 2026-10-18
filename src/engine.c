#include "engine.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SONG_MAGIC_V1 0x48493501u
#define SONG_MAGIC_V2 0x48493502u /* clk_ppqn indexed {1, 2, 4, 24} */
#define SONG_MAGIC_V3 0x48493503u /* no pan / drive fields */
#define SONG_MAGIC_V4 0x48493504u /* no hat extras / slots / chain */
#define SONG_MAGIC_V5 0x48493505u /* no sync_mode */
#define SONG_MAGIC_V6 0x48493506u /* no kits */
#define SONG_MAGIC 0x48493507u
#define TWO_PI 6.28318531f

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

Song song;
Live live;
Sample samples[MAX_SAMPLES];
int nsamples;

const char *const voice_names[NV] = {"SUB", "KICK", "SNARE", "PERC", "HAT"};

static const float ratio_table[] = {0.5f, 1.f, 1.41f, 1.5f, 2.f, 2.76f, 3.f,
                                    3.5f, 4.2f, 5.19f, 7.f, 8.33f, 11.1f};
#define NRATIO (int)(sizeof ratio_table / sizeof ratio_table[0])

static const float base_freq[4] = {41.2f, 48.f, 180.f, 420.f};
static const float voice_level[NV] = {0.95f, 1.0f, 0.7f, 0.45f, 0.6f};
static const int def_pan[NV] = {0, 0, -20, 30, -35};
static const int sweep_steps[NSWEEP] = {4, 8, 16, 32};
static const char *const sweep_names[NSWEEP] = {"1/4", "1/2", "1BAR", "2BAR"};

static const ParamDef fm_params[NPARAM] = {
    {"TUNE", -24, 24}, {"RATIO", 0, NRATIO - 1}, {"INDEX", 0, 100},
    {"DECAY", 0, 100}, {"SWEEP", 0, 100},        {"FEEDBK", 0, 100},
    {"NOISE", 0, 100}, {"STUTTR", 2, 8},         {"GLITCH", 0, 100}};
static const ParamDef sub_params[NPARAM] = {
    {"TUNE", -24, 24}, {"RATIO", 0, NRATIO - 1}, {"INDEX", 0, 100},
    {"DECAY", 0, 100}, {"SWEEP", 0, 100},        {"FEEDBK", 0, 100},
    {"DRIVE", 0, 100}, {"STUTTR", 2, 8},         {"GLITCH", 0, 100}};
static const ParamDef hat_params[NPARAM + NHATX] = {
    {"SAMPLE", 0, MAX_SAMPLES - 1}, {"START", 0, 100}, {"LOOP", 0, 100},
    {"DECAY", 0, 100},              {"PITCH", -24, 24}, {"TONE", 0, 100},
    {"DRIFT", 0, 100},              {"STUTTR", 2, 8},  {"GLITCH", 0, 100},
    {"RESO", 0, 100},               {"SHINE", 0, 100}};
/* inharmonic partials for the SHINE layer: a glassy, bell-like cluster */
#define NSHINE 5
static const float shine_ratio[NSHINE] = {1.f, 1.414f, 1.932f, 2.414f, 3.17f};
static const float shine_amp[NSHINE] = {1.f, 0.7f, 0.5f, 0.35f, 0.25f};

static const int def_params[NV][NPARAM] = {
    /* SUB   */ {0, 1, 4, 80, 12, 0, 35, 3, 35},
    /* KICK  */ {0, 1, 38, 50, 72, 12, 6, 4, 45},
    /* SNARE */ {0, 2, 45, 44, 28, 35, 62, 3, 55},
    /* PERC  */ {0, 7, 62, 26, 22, 45, 0, 6, 70},
    /* HAT   */ {0, 0, 40, 30, 0, 55, 20, 4, 50},
};
/* {on, steps, hits, rotation} for rows HIT / STUT / GLITCH */
static const Cell def_cells[NV][NROW] = {
    /* SUB   */ {{1, 8, 3, 0}, {0, 16, 2, 6}, {0, 13, 2, 0}},
    /* KICK  */ {{1, 16, 4, 0}, {1, 16, 1, 14}, {0, 7, 2, 1}},
    /* SNARE */ {{1, 16, 2, 4}, {1, 13, 1, 5}, {1, 11, 2, 3}},
    /* PERC  */ {{1, 13, 5, 0}, {1, 9, 2, 3}, {1, 7, 1, 2}},
    /* HAT   */ {{1, 16, 11, 0}, {1, 12, 3, 1}, {1, 9, 2, 4}},
};

/* {type, p1, p2, mix} for R2 + up / down / left / right */
static const FxSlot def_fx[NFXSLOT] = {
    {FXT_REPEAT, 2, 2, 100}, {FXT_TAPESTOP, 3, 0, 100},
    {FXT_REVERSE, 3, 1, 100}, {FXT_CRUSH, 7, 4, 100}};

typedef struct {
    const char *name;
    int min, max, def;
} FxParam;
static const struct {
    const char *name;
    FxParam p[2];
} fx_types[NFXT] = {
    [FXT_REPEAT] = {"REPEAT", {{"LENGTH", 0, 4, 2}, {"ROLL", 0, 3, 2}}},
    [FXT_TAPESTOP] = {"TAPESTOP", {{"TIME", 1, 5, 3}, {"MODE", 0, 1, 0}}},
    [FXT_REVERSE] = {"REVERSE", {{"LENGTH", 1, 5, 3}, {"SPEED", 0, 2, 1}}},
    [FXT_CRUSH] = {"CRUSH", {{"RATE", 1, 32, 7}, {"BITS", 1, 12, 4}}},
    [FXT_GATE] = {"GATE", {{"RATE", 0, 3, 1}, {"DUTY", 1, 9, 5}}},
};
static const float len_steps[] = {0.5f, 1.f, 2.f, 4.f, 8.f, 16.f};
static const char *const len_names[] = {"1/32", "1/16", "1/8", "1/4", "1/2", "1BAR"};
static const char *const roll_names[] = {"OFF", "SLOW", "MED", "FAST"};
static const int roll_loops[] = {0, 8, 4, 2};
static const float gate_steps[] = {2.f, 1.f, 0.5f, 0.25f};
static const char *const gate_names[] = {"1/8", "1/16", "1/32", "1/64"};
static const float rev_speeds[] = {0.5f, 1.f, 2.f};
static const char *const rev_speed_names[] = {"0.5x", "1x", "2x"};
static const int ppqn_values[NPPQN] = {1, 2, 4, 8, 16, 24, 32};

/* ---------------------------------------------------------------- params */

/* the SUB stretches to 4 s for 808-style tails, the rest stop at 2 s */
static float decay_seconds(int v, int d)
{
    return 0.008f * powf(v == V_SUB ? 500.f : 250.f, d / 100.f);
}
static float loop_ms(int x) { return 0.7f * powf(150.f / 0.7f, x / 99.f); }
static float tone_hz(int x) { return 100.f * powf(120.f, x / 100.f); }

const ParamDef *param_def(int v, int p)
{
    return v == V_HAT ? &hat_params[p] : v == V_SUB ? &sub_params[p] : &fm_params[p];
}

int param_max(int v, int p)
{
    if (v == V_HAT && p == H_SAMPLE)
        return nsamples > 0 ? nsamples - 1 : 0;
    return param_def(v, p)->max;
}

int nparam(int v) { return v == V_HAT ? NPARAM + NHATX : NPARAM; }

int *param_ptr(int v, int p)
{
    return p >= NPARAM ? &song.hat_x[p - NPARAM] : &song.param[v][p];
}

void param_format(int v, int p, char *b, int n)
{
    int x = *param_ptr(v, p);
    if (p >= NPARAM) {
        if (x == 0)
            snprintf(b, n, p - NPARAM == HX_RESO ? "HP" : "OFF");
        else
            snprintf(b, n, "%d%%", x);
        return;
    }
    if (p == P_DECAY) {
        float s = decay_seconds(v, x);
        if (s < 1.f)
            snprintf(b, n, "%dms", (int)(s * 1000.f + .5f));
        else
            snprintf(b, n, "%.2fs", s);
        return;
    }
    if (p == P_STUT) {
        snprintf(b, n, "x%d", x);
        return;
    }
    if (v == V_HAT) {
        switch (p) {
        case H_SAMPLE:
            snprintf(b, n, "%s", nsamples ? samples[x < nsamples ? x : 0].name : "-");
            return;
        case H_START:
            snprintf(b, n, "%d%%", x);
            return;
        case H_LOOP: {
            float ms = loop_ms(x);
            if (x >= 100)
                snprintf(b, n, "OFF");
            else if (ms < 10.f)
                snprintf(b, n, "%.1fms", ms);
            else
                snprintf(b, n, "%dms", (int)ms);
            return;
        }
        case H_PITCH:
            snprintf(b, n, "%+d", x);
            return;
        case H_TONE: {
            float hz = tone_hz(x);
            if (hz >= 1000.f)
                snprintf(b, n, "%.1fk", hz / 1000.f);
            else
                snprintf(b, n, "%d", (int)hz);
            return;
        }
        }
    }
    else if (p == P_TUNE) {
        snprintf(b, n, "%+d", x);
        return;
    }
    else if (p == P_RATIO) {
        snprintf(b, n, "%.2f", ratio_table[x]);
        return;
    }
    snprintf(b, n, "%d", x);
}

const char *sweep_rate_name(int i) { return sweep_names[i]; }

const char *fx_type_name(int t) { return fx_types[t].name; }
int fx_is_source(int t) { return t <= FXT_REVERSE; }
int clk_ppqn_value(int i) { return ppqn_values[i]; }

void fx_param_range(int t, int which, const char **name, int *min, int *max)
{
    const FxParam *p = &fx_types[t].p[which];
    *name = p->name;
    *min = p->min;
    *max = p->max;
}

void fx_slot_set_type(FxSlot *s, int t)
{
    s->type = t;
    s->p1 = fx_types[t].p[0].def;
    s->p2 = fx_types[t].p[1].def;
}

static float sr = 48000.f;

void fx_param_format(int t, int which, int x, char *b, int n)
{
    switch (t * 2 + which) {
    case FXT_REPEAT * 2:
    case FXT_TAPESTOP * 2:
    case FXT_REVERSE * 2: snprintf(b, n, "%s", len_names[x]); return;
    case FXT_REPEAT * 2 + 1: snprintf(b, n, "%s", roll_names[x]); return;
    case FXT_TAPESTOP * 2 + 1: snprintf(b, n, "%s", x ? "DRAG" : "STOP"); return;
    case FXT_REVERSE * 2 + 1: snprintf(b, n, "%s", rev_speed_names[x]); return;
    case FXT_CRUSH * 2: snprintf(b, n, "%.1fk", sr / x / 1000.f); return;
    case FXT_GATE * 2: snprintf(b, n, "%s", gate_names[x]); return;
    case FXT_GATE * 2 + 1: snprintf(b, n, "%d%%", x * 10); return;
    default: snprintf(b, n, "%d", x); return;
    }
}

int euclid_hit(const Cell *c, int step)
{
    int n = c->steps;
    if (n < 1 || c->hits < 1)
        return 0;
    int i = ((step - c->rot) % n + n) % n;
    return (i * c->hits) % n < c->hits;
}

/* ------------------------------------------------------------------- dsp */

#define SIN_BITS 12
#define SIN_N (1 << SIN_BITS)
#define SIN_FRAC (32 - SIN_BITS)
#define PM_SCALE 683565275.6f /* 2^32 / 2pi */

static float sintab[SIN_N + 1];

static inline float sin_u32(uint32_t ph)
{
    uint32_t i = ph >> SIN_FRAC;
    float f = (float)(ph & ((1u << SIN_FRAC) - 1)) * (1.f / (1u << SIN_FRAC));
    return sintab[i] + (sintab[i + 1] - sintab[i]) * f;
}

static uint32_t rng = 0x9e3779b9u;
static inline uint32_t xr(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static inline float frand(void) { return (xr() >> 8) * (1.f / 16777216.f); }
static inline float fnoise(void) { return frand() * 2.f - 1.f; }

static inline float softclip(float x)
{
    if (x > 3.f)
        return 1.f;
    if (x < -3.f)
        return -1.f;
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

typedef struct {
    int active;
    float vel, gl, gr, last;
    float env, envk;
    float dc_y, dc_g;
    /* FM */
    uint32_t phc, phm, inc_c, inc_m;
    float basef, base_semi, ratio, index, ieff, sweep, fb, noise, prevm, nz;
    float drive, drive_norm;
    float penv, penvk, ienv, ienvk, nenv, nenvk;
    int ctl;
    /* micro-loop sampler */
    const Sample *smp;
    float ph, rate, drift;
    int rs, rl, fade, loop_on, first;
    float h1, h2, ha1, ha2, ha3, hk, bp_mix, bp_gain;
    /* SHINE partials */
    uint32_t sh_ph[NSHINE], sh_inc[NSHINE];
    float sh_amp[NSHINE], sh_k[NSHINE], sh_lvl;
    /* ratchets */
    int rat_left, rat_cd, rat_int, rat_flags;
    float rat_vel;
} Voice;

static Voice vox[NV];
static float gain_cur[NV];
static float sps; /* samples per 16th step */
static double step_phase, clock_steps, sync_rate = 1.0;
static int next_step;
static float meter_acc[NV + 1];

#define RB_BITS 18
#define RB_N (1 << RB_BITS)
#define RB_MASK (RB_N - 1)
static float rbL[RB_N], rbR[RB_N];
static uint32_t wpos;
/* buffer (source) effect: one at a time */
static int src_on, src_slot, src_type, src_ack_slot = -1, src_ack_type = -1;
static float src_mix, src_mix_target;
static int rep_armed, rep_len, rep_t, rep_min, rep_roll;
static uint32_t rep_start;
static double tape_rp, rev_rp;
static float tape_rate, tape_dec, tape_floor, rev_speed;
static int rev_count, rev_len;
/* insert effects: one state per slot */
static float ins_mix[NFXSLOT], crush_l[NFXSLOT], crush_r[NFXSLOT], gate_g[NFXSLOT];
static int crush_cnt[NFXSLOT];
static float flt_mix, f_ic1[2], f_ic2[2], f_a1, f_a2, f_a3;
static int f_ctl;
#define FLT_K 0.5f

static void set_pan(Voice *o, float pan)
{
    float a = (pan + 1.f) * 0.25f * 3.14159265f;
    o->gl = cosf(a) * 1.41421356f;
    o->gr = sinf(a) * 1.41421356f;
}

static void fm_start(Voice *o, int v, const int *P, int glitch, float g, float dec)
{
    static const float jumps[] = {12, 7, -12, 19, 24, -5, 5};
    float semi = (float)P[P_TUNE];
    o->ratio = ratio_table[P[P_RATIO]];
    float ix = P[P_INDEX] / 100.f;
    o->index = ix * ix * 10.f;
    if (glitch) {
        if (frand() < 0.4f + 0.6f * g)
            semi += jumps[xr() % 7];
        if (frand() < g)
            o->ratio = ratio_table[xr() % NRATIO];
        o->index *= 1.f + 2.f * g * frand();
    }
    o->basef = base_freq[v];
    o->base_semi = semi;
    o->sweep = P[P_SWEEP] / 100.f * 48.f;
    o->penv = 1.f;
    o->penvk = expf(-4.6f / (0.045f * sr));
    o->ienv = 1.f;
    o->ienvk = expf(-4.6f / (fmaxf(0.01f, dec * 0.5f) * sr));
    o->fb = P[P_FEEDBK] / 100.f * 1.6f;
    o->noise = v == V_SUB ? 0.f : P[P_NOISE] / 100.f * 0.9f;
    o->drive = v == V_SUB ? P[P_DRIVE] / 100.f * 6.f : 0.f;
    o->drive_norm = 1.f / softclip(1.f + o->drive);
    o->nenv = 1.f;
    o->nenvk = expf(-4.6f / (dec * 0.6f * sr));
    o->phc = o->phm = 0;
    o->prevm = 0.f;
    o->ctl = 0;
}

static void hat_start(Voice *o, const int *P, int glitch, float g)
{
    if (nsamples < 1) {
        o->active = 0;
        return;
    }
    int si = P[H_SAMPLE] < nsamples ? P[H_SAMPLE] : 0;
    const Sample *s = &samples[si];
    float start = P[H_START] / 100.f;
    float lms = P[H_LOOP] >= 100 ? 0.f : loop_ms(P[H_LOOP]);
    float pitch = (float)P[H_PITCH];
    if (glitch) {
        if (frand() < 0.5f + 0.5f * g)
            start = frand() * 0.8f;
        if (lms > 0.f || frand() < g)
            lms = (lms > 0.f ? lms : 20.f) * (0.1f + 0.9f * frand());
        if (frand() < g)
            pitch += frand() < 0.5f ? 12.f : -7.f;
    }
    o->smp = s;
    o->rs = (int)(start * (s->len - 1));
    if (lms > 0.f) {
        o->rl = (int)(lms * 0.001f * sr);
        if (o->rl < 8)
            o->rl = 8;
        if (o->rl > s->len)
            o->rl = s->len;
        if (o->rs + o->rl > s->len)
            o->rs = s->len - o->rl;
        o->loop_on = 1;
    }
    else {
        o->rl = s->len - o->rs;
        o->loop_on = 0;
    }
    o->ph = 0.f;
    o->first = 1;
    o->rate = exp2f(fmaxf(-36.f, fminf(36.f, pitch)) / 12.f);
    o->drift = P[H_DRIFT] / 100.f;
    o->fade = o->rl / 4;
    if (o->fade > (int)(0.0006f * sr))
        o->fade = (int)(0.0006f * sr);
    if (o->fade < 1)
        o->fade = 1;
    /* RESO morphs the TONE high-pass into a ringing band-pass at TONE */
    float fc = fminf(tone_hz(P[H_TONE]), 0.45f * sr);
    float gg = tanf(3.14159265f * fc / sr);
    float r = song.hat_x[HX_RESO] / 100.f;
    o->hk = 1.2f - 1.15f * r;
    o->bp_mix = r;
    o->bp_gain = 0.5f * powf(o->hk, -0.6f); /* keeps the loudness roughly even as Q rises */
    o->ha1 = 1.f / (1.f + gg * (gg + o->hk));
    o->ha2 = gg * o->ha1;
    o->ha3 = gg * o->ha2;
    o->h1 = o->h2 = 0.f;

    /* SHINE: a short cluster of inharmonic sines on top of the sample */
    float sh = song.hat_x[HX_SHINE] / 100.f;
    o->sh_lvl = sh * 0.6f;
    if (sh > 0.f) {
        float base = 3200.f * exp2f(fmaxf(-36.f, fminf(36.f, pitch)) / 12.f);
        float dec = decay_seconds(V_HAT, P[P_DECAY]);
        for (int i = 0; i < NSHINE; i++) {
            float f = base * shine_ratio[i];
            if (glitch)
                f *= 1.f + fnoise() * 0.08f * g;
            f = fminf(f, 0.45f * sr);
            o->sh_inc[i] = (uint32_t)(f / sr * 4294967296.f);
            o->sh_ph[i] = (uint32_t)(frand() * 4294967296.f);
            o->sh_amp[i] = shine_amp[i];
            o->sh_k[i] = expf(-4.6f / (dec * (1.2f / (1.f + 0.5f * i)) * sr));
        }
    }
}

static void voice_trigger(int v, int flags, float vel, int from_ratchet)
{
    Voice *o = &vox[v];
    const int *P = song.param[v];
    float g = P[P_GLITCH] / 100.f;
    int glitch = flags & TRIG_GLITCH;

    o->dc_y = o->active ? o->last : 0.f;
    o->dc_g = 1.f;
    o->active = 1;
    o->vel = vel;
    /* mono + clock output sums the channels, so pan is centred there */
    float pan = song.out_mode == OUT_MONO_CLOCK ? 0.f : song.pan[v] / 100.f;
    if (glitch && song.out_mode != OUT_MONO_CLOCK)
        pan += fnoise() * (0.3f + 0.6f * g);
    set_pan(o, fmaxf(-1.f, fminf(1.f, pan)));

    float dec = decay_seconds(v, P[P_DECAY]);
    if (glitch)
        dec *= 1.f - 0.6f * g * frand();
    o->env = 1.f;
    o->envk = expf(-4.6f / (dec * sr));

    if (v == V_HAT)
        hat_start(o, P, glitch, g);
    else
        fm_start(o, v, P, glitch, g, dec);

    if (!from_ratchet) {
        o->rat_left = 0;
        if (flags & TRIG_STUT) {
            int r = P[P_STUT];
            o->rat_left = r - 1;
            o->rat_int = (int)(sps / r);
            o->rat_vel = vel;
            o->rat_flags = flags & TRIG_GLITCH;
        }
        else if (glitch && frand() < g * 0.5f) {
            o->rat_left = 1;
            o->rat_int = (int)(sps * 0.25f);
            o->rat_vel = vel * 0.85f;
            o->rat_flags = 0;
        }
        if (o->rat_int < 16)
            o->rat_int = 16;
        o->rat_cd = o->rat_int;
    }
    live.trig_count[v]++;
}

static inline float fm_tick(Voice *o)
{
    if (--o->ctl <= 0) {
        o->ctl = 16;
        float f = o->basef * exp2f((o->base_semi + o->sweep * o->penv) * (1.f / 12.f));
        float fc = fminf(f / sr, 0.49f);
        float fm = fminf(f * o->ratio / sr, 0.49f);
        o->inc_c = (uint32_t)(fc * 4294967296.f);
        o->inc_m = (uint32_t)(fm * 4294967296.f);
        o->ieff = o->index * (0.25f + 0.75f * o->ienv) * PM_SCALE;
    }
    o->penv *= o->penvk;
    o->ienv *= o->ienvk;
    o->env *= o->envk;
    float m = sin_u32(o->phm + (uint32_t)(int32_t)(o->fb * o->prevm * PM_SCALE));
    o->prevm = m;
    float c = sin_u32(o->phc + (uint32_t)(int64_t)(o->ieff * m));
    o->phm += o->inc_m;
    o->phc += o->inc_c;
    float y = c * o->env;
    if (o->drive > 0.f)
        y = softclip(y * (1.f + o->drive)) * o->drive_norm;
    if (o->noise > 0.f) {
        float w = fnoise();
        y += (w - o->nz) * 0.5f * o->nenv * o->noise;
        o->nz = w;
        o->nenv *= o->nenvk;
    }
    if (o->env < 5e-4f && (o->noise <= 0.f || o->nenv < 5e-4f))
        o->active = 0;
    return y;
}

static inline float hat_tick(Voice *o)
{
    const Sample *s = o->smp;
    int i = (int)o->ph;
    float fr = o->ph - (float)i;
    int idx = o->rs + i;
    int nx = idx + 1 < s->len ? idx + 1 : idx;
    float x = s->data[idx] + (s->data[nx] - s->data[idx]) * fr;

    float w = 1.f;
    if (!o->first && o->ph < o->fade)
        w = o->ph / o->fade;
    if (o->loop_on) {
        float rem = o->rl - o->ph;
        if (rem < o->fade)
            w = fminf(w, rem / o->fade);
    }

    o->ph += o->rate;
    while (o->ph >= o->rl) {
        if (!o->loop_on) {
            o->active = 0;
            o->ph = 0.f;
            break;
        }
        o->ph -= o->rl;
        o->first = 0;
        if (o->drift > 0.f) {
            o->rs += (int)(o->drift * o->rl * 0.5f) + 1;
            if (o->rs + o->rl >= s->len)
                o->rs = 0;
        }
    }

    if (o->sh_lvl > 0.f) {
        float s = 0.f;
        for (int i = 0; i < NSHINE; i++) {
            s += sin_u32(o->sh_ph[i]) * o->sh_amp[i];
            o->sh_ph[i] += o->sh_inc[i];
            o->sh_amp[i] *= o->sh_k[i];
        }
        x += s * o->sh_lvl;
    }

    float v3 = x - o->h2;
    float v1 = o->ha1 * o->h1 + o->ha2 * v3;
    float v2 = o->h2 + o->ha2 * o->h1 + o->ha3 * v3;
    o->h1 = 2.f * v1 - o->h1;
    o->h2 = 2.f * v2 - o->h2;
    float hp = x - o->hk * v1 - v2;
    float out = o->bp_mix > 0.f ? hp * (1.f - o->bp_mix) + v1 * o->bp_gain * o->bp_mix : hp;

    float y = out * w * o->env;
    o->env *= o->envk;
    if (o->env < 5e-4f)
        o->active = 0;
    return y;
}

/* ------------------------------------------------- pattern slots / chain */

static int chain_pos, chain_bar, chain_fresh;

void slot_save(int i)
{
    memcpy(song.slot_cell[i], song.cell, sizeof song.cell);
    memcpy(song.slot_mute[i], song.mute, sizeof song.mute);
    song.slot_used[i] = 1;
    live.cur_slot = i;
}

void slot_load(int i)
{
    if (!song.slot_used[i])
        return;
    memcpy(song.cell, song.slot_cell[i], sizeof song.cell);
    memcpy(song.mute, song.slot_mute[i], sizeof song.mute);
    live.cur_slot = i;
}

void slot_clear(int i)
{
    song.slot_used[i] = 0;
    memset(song.slot_cell[i], 0, sizeof song.slot_cell[i]);
    memset(song.slot_mute[i], 0, sizeof song.slot_mute[i]);
    if (live.cur_slot == i)
        live.cur_slot = -1;
}

void kit_save(int i)
{
    memcpy(song.kit_param[i], song.param, sizeof song.param);
    memcpy(song.kit_hat_x[i], song.hat_x, sizeof song.hat_x);
    memcpy(song.kit_vol[i], song.vol, sizeof song.vol);
    memcpy(song.kit_pan[i], song.pan, sizeof song.pan);
    memcpy(song.kit_drive[i], song.drive, sizeof song.drive);
    song.kit_master_drive[i] = song.master_drive;
    song.kit_used[i] = 1;
    live.cur_kit = i;
}

void kit_load(int i)
{
    if (!song.kit_used[i])
        return;
    memcpy(song.param, song.kit_param[i], sizeof song.param);
    memcpy(song.hat_x, song.kit_hat_x[i], sizeof song.hat_x);
    memcpy(song.vol, song.kit_vol[i], sizeof song.vol);
    memcpy(song.pan, song.kit_pan[i], sizeof song.pan);
    memcpy(song.drive, song.kit_drive[i], sizeof song.drive);
    song.master_drive = song.kit_master_drive[i];
    live.cur_kit = i;
}

void kit_clear(int i)
{
    song.kit_used[i] = 0;
    memset(song.kit_param[i], 0, sizeof song.kit_param[i]);
    if (live.cur_kit == i)
        live.cur_kit = -1;
}

int chain_len(void)
{
    int n = 0;
    while (n < NCHAIN && song.chain_slot[n])
        n++;
    return n;
}

static void chain_load(int pos)
{
    int s = song.chain_slot[pos];
    if (s >= 1 && s <= NSLOT)
        slot_load(s - 1);
    live.chain_pos = pos;
    live.chain_bar = chain_bar;
}

/* start (or restart) song mode at chain entry pos: the slot loads now and
 * its bar count starts at the next bar line */
void chain_play_from(int pos)
{
    song.song_mode = 1;
    chain_pos = pos;
    chain_bar = 0;
    chain_fresh = 1;
    chain_load(pos);
}

void song_mode_set(int on)
{
    if (on) {
        int n = chain_len();
        if (n)
            chain_play_from(chain_pos < n ? chain_pos : 0);
        else
            song.song_mode = 1;
    }
    else
        song.song_mode = 0;
}

/* one bar (16 steps) has elapsed while playing */
static void chain_bar_line(void)
{
    int n = chain_len();
    if (!song.song_mode || !n)
        return;
    if (chain_fresh) {
        chain_fresh = 0;
        chain_bar = 0;
    }
    else if (++chain_bar >= song.chain_bars[chain_pos] || chain_pos >= n) {
        chain_bar = 0;
        chain_pos = chain_pos + 1 < n ? chain_pos + 1 : 0;
        chain_load(chain_pos);
    }
    live.chain_bar = chain_bar;
}

static void fire_step(void)
{
    int s = next_step++;
    live.step = s;
    clock_steps = s;
    if (s % 16 == 0)
        chain_bar_line();
    for (int v = 0; v < NV; v++) {
        int flags = 0;
        for (int r = 0; r < NROW; r++)
            if (song.cell[v][r].on && euclid_hit(&song.cell[v][r], s))
                flags |= 1 << r;
        if (flags)
            voice_trigger(v, flags, 1.f, 0);
    }
    if (rep_armed) {
        rep_armed = 0;
        rep_start = wpos;
        rep_t = 0;
    }
}

static void fx_start(int slot)
{
    const FxSlot *s = &song.fx[slot];
    src_on = 1;
    src_slot = slot;
    src_type = s->type;
    switch (s->type) {
    case FXT_REPEAT:
        rep_len = (int)(sps * len_steps[s->p1]);
        rep_min = (int)(sps * 0.25f);
        if (rep_min < 64)
            rep_min = 64;
        if (rep_min > rep_len)
            rep_min = rep_len;
        rep_roll = roll_loops[s->p2];
        rep_t = 0;
        rep_start = wpos;
        rep_armed = live.playing;
        break;
    case FXT_TAPESTOP:
        tape_rp = wpos;
        tape_rate = 1.f;
        tape_floor = s->p2 ? 0.5f : 0.f;
        tape_dec = (1.f - tape_floor) / (sps * len_steps[s->p1]);
        break;
    case FXT_REVERSE:
        rev_speed = rev_speeds[s->p2];
        rev_rp = wpos;
        rev_count = 0;
        rev_len = (int)(sps * len_steps[s->p1]);
        if (rev_len * rev_speed > RB_N - 4096)
            rev_len = (int)((RB_N - 4096) / rev_speed);
        break;
    }
}

static inline float rb_read(const float *rb, double pos)
{
    uint32_t i = (uint32_t)pos;
    float fr = (float)(pos - i);
    return rb[i & RB_MASK] + (rb[(i + 1) & RB_MASK] - rb[i & RB_MASK]) * fr;
}

static inline float edge(int pos, int len)
{
    int f = len / 4 < 96 ? len / 4 : 96;
    if (f < 1)
        return 1.f;
    float a = pos < f ? (float)pos / f : 1.f;
    float b = len - pos < f ? (float)(len - pos) / f : 1.f;
    return a < b ? a : b;
}

static inline float quant(float x, float lv) { return (float)(int)(x * lv + (x >= 0.f ? .5f : -.5f)) / lv; }

void engine_set_playing(int on)
{
    if (on && !live.playing) {
        step_phase = 0.0;
        next_step = 0; /* start from the top of the bar */
        if (song.song_mode)
            chain_fresh = 1;
    }
    live.playing = on;
    if (!on && rep_armed) {
        rep_armed = 0;
        rep_start = wpos;
        rep_t = 0;
    }
}

void engine_audition(int v) { voice_trigger(v, TRIG_HIT, 1.f, 0); }

double engine_position(void)
{
    if (!live.playing)
        return (double)next_step;
    double frac = 1.0 - step_phase / sps;
    return (double)live.step + (frac < 0.0 ? 0.0 : frac > 1.0 ? 1.0 : frac);
}

void engine_set_position(double steps)
{
    if (steps < 0.0)
        steps = 0.0;
    int s = (int)steps;
    double frac = steps - s;
    live.step = s;
    clock_steps = steps;
    next_step = s + 1;
    step_phase = sps * (1.0 - frac);
    if (step_phase < 1.0)
        step_phase = 1.0;
}

void engine_set_rate(double r) { sync_rate = r < 0.9 ? 0.9 : r > 1.1 ? 1.1 : r; }

void engine_render(int16_t *out, int frames)
{
    sps = sr * 15.f / song.bpm;
    if (step_phase > sps)
        step_phase = sps;

    int any_solo = 0;
    for (int v = 0; v < NV; v++)
        any_solo |= song.solo[v];
    float gstep[NV], vdrive[NV], vnorm[NV];
    for (int v = 0; v < NV; v++) {
        float g = song.vol[v] / 100.f;
        g *= g;
        if (song.mute[v] || (any_solo && !song.solo[v]))
            g = 0.f;
        gstep[v] = (g - gain_cur[v]) / frames;
        vdrive[v] = song.drive[v] / 100.f * 6.f;
        vnorm[v] = 1.f / softclip(1.f + vdrive[v]);
    }
    float mdrive = song.master_drive / 100.f * 6.f;
    float mnorm = 1.f / softclip(1.f + mdrive);

    int want = live.fx_src;
    int want_type = want >= 0 ? song.fx[want].type : -1;
    if (want != src_ack_slot || want_type != src_ack_type) {
        src_ack_slot = want;
        src_ack_type = want_type;
        if (want >= 0 && fx_is_source(want_type)) {
            fx_start(want);
            src_mix_target = 1.f;
        }
        else
            src_mix_target = 0.f;
    }
    float fx_ramp = 1.f / (0.004f * sr);
    float gate_k = 1.f - expf(-1.f / (0.001f * sr));
    float flt_target = live.filter_held ? 1.f : 0.f;
    float flt_ramp = 1.f / (0.02f * sr);
    double clk_period = 4.0 / ppqn_values[song.clk_ppqn];
    float clk_pulse = fminf(song.clk_width * 0.001f * sr, (float)(clk_period * sps * 0.5));
    float master = song.master / 100.f;
    master *= master * 1.4f;
    float dc_step = 1.f / (0.003f * sr);

    for (int n = 0; n < frames; n++) {
        if (live.playing) {
            step_phase -= sync_rate;
            if (step_phase <= 0.0) {
                step_phase += sps;
                fire_step();
            }
        }
        double clk = clock_steps;
        clock_steps += sync_rate / sps;

        float L = 0.f, R = 0.f;
        for (int v = 0; v < NV; v++) {
            Voice *o = &vox[v];
            gain_cur[v] += gstep[v];
            if (o->rat_left > 0 && --o->rat_cd <= 0) {
                o->rat_cd = o->rat_int;
                o->rat_left--;
                o->rat_vel *= 0.82f;
                voice_trigger(v, o->rat_flags, o->rat_vel, 1);
            }
            if (!o->active)
                continue;
            float y = (v == V_HAT ? hat_tick(o) : fm_tick(o)) * o->vel * voice_level[v];
            if (o->dc_g > 0.f) {
                y += o->dc_y * o->dc_g;
                o->dc_g -= dc_step;
            }
            o->last = o->active ? y : 0.f;
            if (vdrive[v] > 0.f)
                y = softclip(y * (1.f + vdrive[v])) * vnorm[v];
            y *= gain_cur[v];
            L += y * o->gl;
            R += y * o->gr;
            float a = fabsf(y);
            if (a > meter_acc[v])
                meter_acc[v] = a;
        }

        /* punch-in: buffer effect (latest R2 + direction wins) */
        rbL[wpos & RB_MASK] = L;
        rbR[wpos & RB_MASK] = R;
        if (src_on) {
            float wl = L, wr = R;
            switch (src_type) {
            case FXT_REPEAT:
                if (!rep_armed) {
                    if (rep_t >= rep_len) {
                        int pos = rep_t % rep_len;
                        uint32_t ri = (rep_start + pos) & RB_MASK;
                        float w = edge(pos, rep_len);
                        wl = rbL[ri] * w;
                        wr = rbR[ri] * w;
                    }
                    rep_t++;
                    if (rep_roll && rep_t >= rep_len * (rep_roll + 1) && rep_len > rep_min) {
                        rep_len = rep_len / 2 > rep_min ? rep_len / 2 : rep_min;
                        rep_t = rep_len;
                    }
                }
                break;
            case FXT_TAPESTOP: {
                float w = tape_floor > 0.f ? 1.f : fminf(1.f, tape_rate * 3.f);
                wl = rb_read(rbL, tape_rp) * w;
                wr = rb_read(rbR, tape_rp) * w;
                tape_rp += tape_rate;
                tape_rate = fmaxf(tape_floor, tape_rate - tape_dec);
                if (wpos - tape_rp > RB_N - 4096) /* DRAG fell too far behind */
                    tape_rp += RB_N / 2;
                break;
            }
            case FXT_REVERSE: {
                float w = edge(rev_count, rev_len);
                wl = rb_read(rbL, rev_rp) * w;
                wr = rb_read(rbR, rev_rp) * w;
                rev_rp -= rev_speed;
                if (++rev_count >= rev_len) {
                    rev_count = 0;
                    rev_rp = wpos;
                }
                break;
            }
            }
            float m = src_mix * song.fx[src_slot].mix * 0.01f;
            L += (wl - L) * m;
            R += (wr - R) * m;
            if (src_mix < src_mix_target)
                src_mix = fminf(src_mix_target, src_mix + fx_ramp);
            else if (src_mix > src_mix_target)
                src_mix = fmaxf(src_mix_target, src_mix - fx_ramp);
            if (src_mix <= 0.f && src_mix_target <= 0.f)
                src_on = 0;
        }
        wpos++;

        /* punch-in: insert effects, stacked in slot order */
        for (int sl = 0; sl < NFXSLOT; sl++) {
            const FxSlot *fs = &song.fx[sl];
            if (fx_is_source(fs->type)) {
                ins_mix[sl] = 0.f;
                continue;
            }
            float target = live.fx_held[sl] ? fs->mix * 0.01f : 0.f;
            if (ins_mix[sl] <= 0.f && target <= 0.f)
                continue;
            float wl, wr;
            if (fs->type == FXT_CRUSH) {
                if (++crush_cnt[sl] >= fs->p1) {
                    float lv = (float)(1 << (fs->p2 - 1));
                    crush_cnt[sl] = 0;
                    crush_l[sl] = quant(L, lv);
                    crush_r[sl] = quant(R, lv);
                }
                wl = crush_l[sl];
                wr = crush_r[sl];
            }
            else {
                float per = gate_steps[fs->p1];
                float ph = (float)(fmod(clk, per) / per);
                gate_g[sl] += ((ph < fs->p2 * 0.1f ? 1.f : 0.f) - gate_g[sl]) * gate_k;
                wl = L * gate_g[sl];
                wr = R * gate_g[sl];
            }
            L += (wl - L) * ins_mix[sl];
            R += (wr - R) * ins_mix[sl];
            if (ins_mix[sl] < target)
                ins_mix[sl] = fminf(target, ins_mix[sl] + fx_ramp);
            else
                ins_mix[sl] = fmaxf(target, ins_mix[sl] - fx_ramp);
        }

        /* L2: tempo-synced resonant lowpass sweep */
        if (flt_mix > 0.f || flt_target > 0.f) {
            if (--f_ctl <= 0) {
                f_ctl = 16;
                int p = sweep_steps[song.sweep_rate];
                float ph = (float)(fmod(clock_steps, (double)p) / p);
                float cn = 0.5f + 0.5f * cosf(TWO_PI * ph);
                live.cutoff_norm = cn;
                float fc = fminf(90.f * powf(160.f, cn), 0.45f * sr);
                float g = tanf(3.14159265f * fc / sr);
                f_a1 = 1.f / (1.f + g * (g + FLT_K));
                f_a2 = g * f_a1;
                f_a3 = g * f_a2;
            }
            float in[2] = {L, R};
            for (int c = 0; c < 2; c++) {
                float v3 = in[c] - f_ic2[c];
                float v1 = f_a1 * f_ic1[c] + f_a2 * v3;
                float v2 = f_ic2[c] + f_a2 * f_ic1[c] + f_a3 * v3;
                f_ic1[c] = 2.f * v1 - f_ic1[c];
                f_ic2[c] = 2.f * v2 - f_ic2[c];
                in[c] += (v2 - in[c]) * flt_mix;
            }
            L = in[0];
            R = in[1];
            flt_mix += flt_mix < flt_target ? flt_ramp : -flt_ramp;
            flt_mix = fminf(1.f, fmaxf(0.f, flt_mix));
            if (flt_mix <= 0.f)
                f_ic1[0] = f_ic1[1] = f_ic2[0] = f_ic2[1] = 0.f;
        }

        L = softclip(L * master * (1.f + mdrive)) * mnorm;
        R = softclip(R * master * (1.f + mdrive)) * mnorm;
        float a = fmaxf(fabsf(L), fabsf(R));
        if (a > meter_acc[NV])
            meter_acc[NV] = a;

        /* mono audio on one side, sync pulses on the other */
        if (song.out_mode == OUT_MONO_CLOCK) {
            float mono = 0.5f * (L + R), c = 0.f;
            if (live.playing && fmod(clk, clk_period) * sps < clk_pulse)
                c = 0.95f;
            L = song.clk_side ? c : mono;
            R = song.clk_side ? mono : c;
        }
        out[2 * n] = (int16_t)(L * 32000.f);
        out[2 * n + 1] = (int16_t)(R * 32000.f);
    }

    for (int v = 0; v <= NV; v++) {
        live.meter[v] = meter_acc[v];
        meter_acc[v] = 0.f;
    }
}

/* --------------------------------------------------------------- samples */

static Sample *add_sample(const char *name, int len)
{
    if (nsamples >= MAX_SAMPLES || len < 16)
        return NULL;
    Sample *s = &samples[nsamples];
    s->data = calloc(len, sizeof(float));
    if (!s->data)
        return NULL;
    s->len = len;
    snprintf(s->name, sizeof s->name, "%s", name);
    nsamples++;
    return s;
}

static void normalize(Sample *s)
{
    float pk = 1e-9f;
    for (int i = 0; i < s->len; i++)
        pk = fmaxf(pk, fabsf(s->data[i]));
    for (int i = 0; i < s->len; i++)
        s->data[i] *= 0.9f / pk;
}

/* Modal cymbal model: inharmonic decaying partials (higher ones die faster),
 * a breathy noise layer and a stick transient. */
static void gen_modal(const char *name, float secs, int np, float fmin, float fmax, float tmin,
                      float tmax, float noise_amt, float noise_tau, float stick, uint32_t seed)
{
    Sample *s = add_sample(name, (int)(secs * sr));
    if (!s)
        return;
    rng = seed;
    float lr = logf(fmax / fmin);
    for (int p = 0; p < np; p++) {
        float r = frand();
        float f = fmin * expf(lr * r);
        float tau = tmax * powf(tmin / tmax, 0.7f * r + 0.3f * frand());
        float amp = (0.3f + 0.7f * frand()) / sqrtf((float)np);
        float w = TWO_PI * f / sr, ph = TWO_PI * frand();
        float c = cosf(w), sn = sinf(w), x = cosf(ph), y = sinf(ph);
        float d = expf(-1.f / (tau * sr));
        for (int i = 0; i < s->len && amp > 1e-5f; i++) {
            s->data[i] += amp * y;
            float nx = x * c - y * sn;
            y = x * sn + y * c;
            x = nx;
            amp *= d;
        }
    }
    float nz = 0.f, nd = expf(-1.f / (noise_tau * sr)), ne = noise_amt;
    int stick_len = (int)(0.0015f * sr);
    for (int i = 0; i < s->len; i++) {
        float w = fnoise(), hp = w - nz;
        nz = w;
        s->data[i] += hp * ne * 0.5f;
        ne *= nd;
        if (i < stick_len)
            s->data[i] += hp * stick * (1.f - (float)i / stick_len);
    }
    normalize(s);
}

static void gen_shaker(const char *name, uint32_t seed)
{
    Sample *s = add_sample(name, (int)(0.22f * sr));
    if (!s)
        return;
    rng = seed;
    float lp = 0.f, hpz = 0.f, grain = 0.f;
    int gcount = 0, glen = (int)(0.0003f * sr) + 1;
    for (int i = 0; i < s->len; i++) {
        float t = (float)i / sr;
        float env = t < 0.03f ? t / 0.03f : expf(-(t - 0.03f) / 0.05f);
        if (gcount-- <= 0) {
            gcount = glen;
            grain = frand() < 0.35f ? frand() : 0.f;
        }
        float w = fnoise() * grain;
        lp += (w - lp) * 0.55f;
        float bp = lp - hpz;
        hpz += (lp - hpz) * 0.25f;
        s->data[i] = bp * env;
    }
    normalize(s);
}

static void gen_builtin_samples(void)
{
    gen_modal("CHAT", 0.25f, 48, 3000.f, 16000.f, 0.012f, 0.08f, 0.35f, 0.03f, 0.6f, 0x1001);
    gen_modal("OHAT", 1.2f, 56, 2800.f, 16000.f, 0.25f, 0.9f, 0.3f, 0.3f, 0.5f, 0x2002);
    gen_modal("PEDAL", 0.18f, 32, 1800.f, 9000.f, 0.008f, 0.04f, 0.5f, 0.015f, 0.9f, 0x3003);
    gen_shaker("SHAKR", 0x4004);
    gen_modal("RIDE", 2.0f, 64, 450.f, 12000.f, 0.5f, 2.2f, 0.12f, 0.5f, 0.3f, 0x5005);
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }

static int load_wav(const char *path, const char *name)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 44 || sz > 16 * 1024 * 1024) {
        fclose(f);
        return 0;
    }
    uint8_t *buf = malloc(sz);
    int ok = buf && fread(buf, 1, sz, f) == (size_t)sz;
    fclose(f);
    if (!ok || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
        free(buf);
        return 0;
    }
    int fmt = 0, ch = 0, rate = 0, bits = 0;
    const uint8_t *data = NULL;
    uint32_t dlen = 0;
    for (long p = 12; p + 8 <= sz;) {
        uint32_t clen = rd32(buf + p + 4);
        if (clen > (uint32_t)(sz - p - 8))
            clen = (uint32_t)(sz - p - 8);
        if (!memcmp(buf + p, "fmt ", 4) && clen >= 16) {
            fmt = rd16(buf + p + 8);
            ch = rd16(buf + p + 10);
            rate = (int)rd32(buf + p + 12);
            bits = rd16(buf + p + 22);
            if (fmt == 0xFFFE && clen >= 26)
                fmt = rd16(buf + p + 32);
        }
        else if (!memcmp(buf + p, "data", 4)) {
            data = buf + p + 8;
            dlen = clen;
        }
        p += 8 + clen + (clen & 1);
    }
    int bps = bits / 8;
    if (!data || ch < 1 || rate < 4000 || bps < 1 || !((fmt == 1 && bps <= 4) || (fmt == 3 && bps == 4))) {
        free(buf);
        return 0;
    }
    int frames = (int)(dlen / (bps * ch));
    float ratio = (float)rate / sr;
    int out_len = (int)(frames / ratio);
    if (out_len > (int)(3.f * sr))
        out_len = (int)(3.f * sr);
    Sample *s = add_sample(name, out_len);
    if (!s) {
        free(buf);
        return 0;
    }
    for (int i = 0; i < out_len; i++) {
        float pos = i * ratio;
        int j = (int)pos;
        float fr = pos - j, acc[2] = {0, 0};
        for (int k = 0; k < 2; k++) {
            int fi = j + k < frames ? j + k : frames - 1;
            for (int c = 0; c < ch; c++) {
                const uint8_t *q = data + ((size_t)fi * ch + c) * bps;
                float x;
                if (fmt == 3) {
                    uint32_t u = rd32(q);
                    memcpy(&x, &u, 4);
                }
                else if (bps == 1)
                    x = (q[0] - 128) / 128.f;
                else if (bps == 2)
                    x = (int16_t)rd16(q) / 32768.f;
                else if (bps == 3)
                    x = (int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) / 2147483648.f;
                else
                    x = (int32_t)rd32(q) / 2147483648.f;
                acc[k] += x / ch;
            }
        }
        s->data[i] = acc[0] + (acc[1] - acc[0]) * fr;
    }
    free(buf);
    normalize(s);
    return 1;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int samples_load_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    char *names[64];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 64) {
        size_t l = strlen(e->d_name);
        if (l > 4 && e->d_name[0] != '.' && !strcasecmp(e->d_name + l - 4, ".wav"))
            names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, cmp_str);
    int loaded = 0;
    for (int i = 0; i < n; i++) {
        char path[512], label[12];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        int k = 0;
        for (const char *c = names[i]; *c && *c != '.' && k < 6; c++)
            label[k++] = (*c >= 'a' && *c <= 'z') ? *c - 32 : (*c >= 32 && *c < 127 ? *c : '?');
        label[k] = 0;
        if (load_wav(path, label)) {
            loaded++;
            fprintf(stderr, "hi5: loaded sample %s\n", path);
        }
        else
            fprintf(stderr, "hi5: skipped %s (unsupported wav)\n", path);
        free(names[i]);
    }
    return loaded;
}

/* ------------------------------------------------------------ song state */

void song_default(void)
{
    memset(&song, 0, sizeof song);
    song.magic = SONG_MAGIC;
    memcpy(song.cell, def_cells, sizeof song.cell);
    memcpy(song.param, def_params, sizeof song.param);
    for (int v = 0; v < NV; v++)
        song.vol[v] = 80;
    song.master = 80;
    song.bpm = 124.f;
    song.sweep_rate = 2;
    memcpy(song.fx, def_fx, sizeof song.fx);
    song.out_mode = OUT_STEREO;
    song.clk_side = 0;
    song.clk_ppqn = 1;
    song.clk_width = 5;
    for (int v = 0; v < NV; v++)
        song.pan[v] = def_pan[v];
    for (int i = 0; i < NCHAIN; i++)
        song.chain_bars[i] = 1;
}

static void sanitize_cell(Cell *c)
{
    c->on = !!c->on;
    c->steps = clampi(c->steps, 1, MAX_STEPS);
    c->hits = clampi(c->hits, 1, c->steps);
    c->rot %= c->steps;
}

/* an older save is a prefix of the current layout: keep the defaults for
 * everything after it */
static void fill_defaults(Song *s, size_t from)
{
    Song d = song;
    song_default();
    memcpy((char *)s + from, (char *)&song + from, sizeof *s - from);
    song = d;
    s->magic = SONG_MAGIC;
}

int song_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    Song s = song;
    size_t got = fread(&s, 1, sizeof s, f);
    fclose(f);
    if (got == SONG_V1_SIZE && s.magic == SONG_MAGIC_V1)
        fill_defaults(&s, SONG_V1_SIZE);
    else if (got == SONG_V3_SIZE && (s.magic == SONG_MAGIC_V2 || s.magic == SONG_MAGIC_V3)) {
        /* v0.0.2 had four PPQN choices; its index 3 (24 PPQN) moved to 5 */
        if (s.magic == SONG_MAGIC_V2 && s.clk_ppqn == 3)
            s.clk_ppqn = 5;
        fill_defaults(&s, SONG_V3_SIZE);
    }
    else if (got == SONG_V4_SIZE && s.magic == SONG_MAGIC_V4)
        fill_defaults(&s, SONG_V4_SIZE);
    else if (got == SONG_V5_SIZE && s.magic == SONG_MAGIC_V5)
        fill_defaults(&s, SONG_V5_SIZE);
    else if (got == SONG_V6_SIZE && s.magic == SONG_MAGIC_V6)
        fill_defaults(&s, SONG_V6_SIZE);
    else if (got != sizeof s || s.magic != SONG_MAGIC)
        return 0;
    for (int v = 0; v < NV; v++) {
        for (int r = 0; r < NROW; r++)
            sanitize_cell(&s.cell[v][r]);
        for (int p = 0; p < NPARAM; p++)
            s.param[v][p] = clampi(s.param[v][p], param_def(v, p)->min, param_def(v, p)->max);
        s.vol[v] = clampi(s.vol[v], 0, 100);
        s.mute[v] = !!s.mute[v];
        s.solo[v] = !!s.solo[v];
    }
    s.master = clampi(s.master, 0, 100);
    if (!(s.bpm >= 40.f && s.bpm <= 250.f))
        s.bpm = 124.f;
    s.sweep_rate = clampi(s.sweep_rate, 0, NSWEEP - 1);
    for (int i = 0; i < NFXSLOT; i++) {
        FxSlot *x = &s.fx[i];
        x->type = clampi(x->type, 0, NFXT - 1);
        x->p1 = clampi(x->p1, fx_types[x->type].p[0].min, fx_types[x->type].p[0].max);
        x->p2 = clampi(x->p2, fx_types[x->type].p[1].min, fx_types[x->type].p[1].max);
        x->mix = clampi(x->mix, 0, 100);
    }
    s.out_mode = clampi(s.out_mode, 0, 1);
    s.clk_side = clampi(s.clk_side, 0, 1);
    s.clk_ppqn = clampi(s.clk_ppqn, 0, NPPQN - 1);
    s.clk_width = clampi(s.clk_width, 1, 20);
    for (int v = 0; v < NV; v++) {
        s.pan[v] = clampi(s.pan[v], -100, 100);
        s.drive[v] = clampi(s.drive[v], 0, 100);
    }
    s.master_drive = clampi(s.master_drive, 0, 100);
    for (int i = 0; i < NHATX; i++)
        s.hat_x[i] = clampi(s.hat_x[i], 0, 100);
    for (int i = 0; i < NSLOT; i++) {
        s.slot_used[i] = !!s.slot_used[i];
        for (int v = 0; v < NV; v++) {
            s.slot_mute[i][v] = !!s.slot_mute[i][v];
            for (int r = 0; r < NROW; r++)
                sanitize_cell(&s.slot_cell[i][v][r]);
        }
    }
    for (int i = 0; i < NCHAIN; i++) {
        s.chain_slot[i] = clampi(s.chain_slot[i], 0, NSLOT);
        s.chain_bars[i] = clampi(s.chain_bars[i], 1, 8);
    }
    s.song_mode = !!s.song_mode;
    s.sync_mode = clampi(s.sync_mode, 0, NSYNCMODE - 1);
    for (int k = 0; k < NKIT; k++) {
        s.kit_used[k] = !!s.kit_used[k];
        for (int v = 0; v < NV; v++) {
            for (int p = 0; p < NPARAM; p++)
                s.kit_param[k][v][p] = clampi(s.kit_param[k][v][p], param_def(v, p)->min, param_def(v, p)->max);
            s.kit_vol[k][v] = clampi(s.kit_vol[k][v], 0, 100);
            s.kit_pan[k][v] = clampi(s.kit_pan[k][v], -100, 100);
            s.kit_drive[k][v] = clampi(s.kit_drive[k][v], 0, 100);
        }
        for (int i = 0; i < NHATX; i++)
            s.kit_hat_x[k][i] = clampi(s.kit_hat_x[k][i], 0, 100);
        s.kit_master_drive[k] = clampi(s.kit_master_drive[k], 0, 100);
    }
    live.cur_slot = -1;
    live.cur_kit = -1;
    chain_pos = chain_bar = 0;
    chain_fresh = 1;
    song = s;
    return 1;
}

int song_save(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return 0;
    int ok = fwrite(&song, sizeof song, 1, f) == 1;
    ok &= fclose(f) == 0;
    return ok && rename(tmp, path) == 0;
}

void engine_init(int sample_rate)
{
    sr = (float)sample_rate;
    live.fx_src = -1;
    live.cur_slot = -1;
    live.cur_kit = -1;
    for (int i = 0; i <= SIN_N; i++)
        sintab[i] = sinf(TWO_PI * i / SIN_N);
    gen_builtin_samples();
    rng = 0x9e3779b9u;
    sps = sr * 15.f / song.bpm;
    for (int v = 0; v < NV; v++)
        set_pan(&vox[v], def_pan[v] / 100.f);
}
