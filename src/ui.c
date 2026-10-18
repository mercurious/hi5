#include "ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "gfx.h"
#include "sync.h"

static const char *const sync_mode_names[NSYNCMODE] = {"OFF", "AUTO", "LEAD", "FOLLOW"};

#define C_BG 0x0d0d14
#define C_PANEL 0x161622
#define C_PANEL2 0x222234
#define C_LINE 0x34344a
#define C_TEXT 0xe8e8f2
#define C_DIM 0x6e6e88
#define C_ACCENT 0x8c64ff
#define C_PLAY 0x3cdc78
#define C_MUTE 0xff3c50
#define C_SOLO 0xffd23c

#define HEADER_H 44
#define BODY_Y 56
#define FOOTER_Y 456

static void nop(void) {}
void (*ui_lock)(void) = nop;
void (*ui_unlock)(void) = nop;

static const uint32_t vcol[NV] = {0xff465a, 0xffa028, 0xfae63c, 0x3cdca0, 0x50aaff};
static const char *const row_names[NROW] = {"HIT", "STUT", "GLCH"};
static const char *const page_names[PAGE_COUNT] = {"PATTERN", "MIX", "SYNTH", "FX", "SONG"};

static int page;
static int pat_v, pat_r, mix_v, mix_p, syn_v, syn_p, fx_r, fx_c;
/* song page cursor: 0..NSLOT-1 are the pattern slots, then the chain nodes */
static int sg_cur;
static struct {
    int has, slot, bars; /* slot is 1-based like Song.chain_slot */
} clip;
enum { MIX_LEVEL, MIX_PAN, MIX_DRIVE };
static const char *const mix_p_names[] = {"LEVEL", "PAN", "DRIVE"};
static int held[BTN_COUNT], hold_ms[BTN_COUNT], rep_ms[BTN_COUNT], rep_n[BTN_COUNT];
static int fx_order[NFXSLOT], fx_norder; /* d-pad presses, most recent last */
static uint32_t now_ms;
static uint32_t taps[6];
static int ntaps;
static char toast[64];
static int toast_ms;
static int quit, changed;
static float meter_disp[NV + 1], flash[NV];
static uint32_t seen_trig[NV];

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(toast, sizeof toast, fmt, ap);
    va_end(ap);
    toast_ms = 1400;
}

void ui_init(void)
{
    for (int v = 0; v < NV; v++)
        seen_trig[v] = live.trig_count[v];
}

int ui_quit_requested(void) { return quit; }

int ui_take_changed(void)
{
    int c = changed;
    changed = 0;
    return c;
}

static int wrap(int x, int n) { return (x % n + n) % n; }

/* ---------------------------------------------------------------- input */

static int is_dpad(int b) { return b <= BTN_RIGHT; }

/* d-pad directions double as FX slot indices */
static void update_fx(void)
{
    live.fx_src = -1;
    for (int sl = 0; sl < NFXSLOT; sl++)
        live.fx_held[sl] = held[BTN_R2] && held[sl];
    for (int i = fx_norder - 1; i >= 0; i--) {
        int sl = fx_order[i];
        if (live.fx_held[sl] && fx_is_source(song.fx[sl].type)) {
            live.fx_src = sl;
            break;
        }
    }
}

static void note_dpad_press(int b)
{
    int n = 0;
    for (int i = 0; i < fx_norder; i++)
        if (fx_order[i] != b)
            fx_order[n++] = fx_order[i];
    fx_order[n++] = b;
    fx_norder = n;
}

static void euclid_permute(Cell *c)
{
    static const uint8_t lens[] = {5, 6, 7, 8, 9, 10, 11, 12, 13, 16, 16, 16};
    int n = lens[rand() % (int)sizeof lens];
    int kmax = n * 2 / 3 > 1 ? n * 2 / 3 : 1;
    c->steps = n;
    c->hits = 1 + rand() % kmax;
    c->rot = rand() % n;
    c->on = 1;
}

static void tap_tempo(void)
{
    if (ntaps && now_ms - taps[ntaps - 1] > 2000)
        ntaps = 0;
    if (ntaps == 6) {
        memmove(taps, taps + 1, 5 * sizeof taps[0]);
        ntaps = 5;
    }
    taps[ntaps++] = now_ms;
    if (ntaps < 2) {
        say("TAP...");
        return;
    }
    float ms = (float)(taps[ntaps - 1] - taps[0]) / (ntaps - 1);
    float bpm = 60000.f / ms;
    bpm = bpm < 40.f ? 40.f : bpm > 250.f ? 250.f : bpm;
    song.bpm = roundf(bpm * 10.f) / 10.f;
    say("TAP  %.1f BPM", song.bpm);
}

static void adjust_param(int v, int p, int dir, int repeats)
{
    const ParamDef *d = param_def(v, p);
    int hi = param_max(v, p);
    int step = (repeats > 12 && hi - d->min > 30) ? 4 : 1;
    int *pp = param_ptr(v, p);
    int x = *pp + dir * step;
    x = x < d->min ? d->min : x > hi ? hi : x;
    if (x != *pp) {
        *pp = x;
        if (!live.playing && repeats == 0)
            engine_audition(v);
    }
}

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

static void adjust_fx(int dir, int repeats)
{
    if (fx_r < NFXSLOT) {
        FxSlot *sl = &song.fx[fx_r];
        if (fx_c == 0) {
            if (!repeats)
                fx_slot_set_type(sl, wrap(sl->type + dir, NFXT));
        }
        else if (fx_c == 3)
            sl->mix = clampi(sl->mix + dir * 5, 0, 100);
        else {
            const char *name;
            int lo, hi, *v = fx_c == 1 ? &sl->p1 : &sl->p2;
            fx_param_range(sl->type, fx_c - 1, &name, &lo, &hi);
            *v = clampi(*v + dir, lo, hi);
        }
        return;
    }
    switch (fx_c) {
    case 0:
        if (!repeats) {
            song.out_mode = !song.out_mode;
            say("%s", song.out_mode ? "MONO AUDIO + CLOCK OUT" : "STEREO OUT");
        }
        break;
    case 1:
        if (!repeats)
            song.clk_side = !song.clk_side;
        break;
    case 2: song.clk_ppqn = clampi(song.clk_ppqn + dir, 0, NPPQN - 1); break;
    case 3: song.clk_width = clampi(song.clk_width + dir, 1, 20); break;
    }
}

#define SG_SYNC (NSLOT + NCHAIN) /* the wifi sync panel is the last cursor stop */
static int sg_on_chain(void) { return sg_cur >= NSLOT && sg_cur < SG_SYNC; }

static void song_action(int b)
{
    if (b == BTN_LEFT || b == BTN_RIGHT) {
        sg_cur = wrap(sg_cur + (b == BTN_RIGHT ? 1 : -1), SG_SYNC + 1);
        return;
    }
    if (sg_cur == SG_SYNC) {
        if (b == BTN_UP || b == BTN_DOWN || b == BTN_A) {
            song.sync_mode = wrap(song.sync_mode + (b == BTN_DOWN ? -1 : 1), NSYNCMODE);
            engine_set_rate(1.0);
            say("WIFI SYNC  %s", sync_mode_names[song.sync_mode]);
        }
        return;
    }
    if (b == BTN_B) {
        song_mode_set(!song.song_mode);
        say("%s", song.song_mode ? "SONG MODE: CHAIN PLAYS" : "LOOP MODE: PATTERN REPEATS");
        return;
    }
    if (!sg_on_chain()) {
        int i = sg_cur;
        switch (b) {
        case BTN_UP:
        case BTN_DOWN: sg_cur ^= 4; break; /* two rows of four */
        case BTN_A:
            if (song.slot_used[i]) {
                slot_load(i);
                say("LOADED %c", 'A' + i);
            }
            else
                say("SLOT %c IS EMPTY", 'A' + i);
            break;
        case BTN_X:
            slot_save(i);
            say("SAVED PATTERN TO %c", 'A' + i);
            break;
        case BTN_Y:
            if (held[BTN_R2]) {
                slot_clear(i);
                say("CLEARED %c", 'A' + i);
            }
            else if (song.slot_used[i]) {
                clip.has = 1;
                clip.slot = i + 1;
                clip.bars = 1;
                say("COPIED %c", 'A' + i);
            }
            break;
        }
        return;
    }
    int i = sg_cur - NSLOT, s = song.chain_slot[i];
    switch (b) {
    case BTN_UP: song.chain_slot[i] = wrap(s + 1, NSLOT + 1); break;
    case BTN_DOWN: song.chain_slot[i] = wrap(s - 1, NSLOT + 1); break;
    case BTN_A:
        if (held[BTN_R2]) {
            if (i < chain_len()) {
                chain_play_from(i);
                say("SONG FROM STEP %d", i + 1);
            }
            else
                say("CHAIN ENDS AT STEP %d", chain_len());
        }
        else {
            int bars = song.chain_bars[i];
            song.chain_bars[i] = bars >= 8 ? 1 : bars * 2;
        }
        break;
    case BTN_X:
        if (clip.has) {
            song.chain_slot[i] = clip.slot;
            song.chain_bars[i] = clip.bars;
            say("PASTED %c x%d AT STEP %d", 'A' + clip.slot - 1, clip.bars, i + 1);
        }
        else
            say("NOTHING COPIED YET");
        break;
    case BTN_Y:
        if (held[BTN_R2]) {
            song.chain_slot[i] = 0;
            say("STEP %d CLEARED", i + 1);
        }
        else if (s) {
            clip.has = 1;
            clip.slot = s;
            clip.bars = song.chain_bars[i];
            say("COPIED %c x%d", 'A' + s - 1, clip.bars);
        }
        break;
    }
}

/* up / down on the mix page edits whichever row A has selected */
static void mix_adjust(int dir)
{
    if (mix_p == MIX_LEVEL) {
        int *vol = mix_v < NV ? &song.vol[mix_v] : &song.master;
        *vol = clampi(*vol + 2 * dir, 0, 100);
    }
    else if (mix_p == MIX_PAN) {
        if (mix_v < NV && song.out_mode != OUT_MONO_CLOCK)
            song.pan[mix_v] = clampi(song.pan[mix_v] + 5 * dir, -100, 100);
    }
    else {
        int *d = mix_v < NV ? &song.drive[mix_v] : &song.master_drive;
        *d = clampi(*d + 2 * dir, 0, 100);
    }
}

static void action(int b, int repeats)
{
    if (held[BTN_R2] && is_dpad(b))
        return; /* d-pad is a punch-in FX while R2 is held */
    if (held[BTN_R2] && b == BTN_X) {
        tap_tempo(); /* global, on every page */
        changed = 1;
        return;
    }

    switch (b) {
    case BTN_L1:
        page = wrap(page - 1, PAGE_COUNT);
        return;
    case BTN_R1:
        page = wrap(page + 1, PAGE_COUNT);
        return;
    case BTN_START:
        engine_set_playing(!live.playing);
        return;
    case BTN_SELECT:
        song.sweep_rate = wrap(song.sweep_rate + 1, NSWEEP);
        say("L2 SWEEP  %s", sweep_rate_name(song.sweep_rate));
        changed = 1;
        return;
    case BTN_MENU:
        quit = 1;
        return;
    }

    changed = 1;
    if (page == PAGE_PATTERN) {
        Cell *c = &song.cell[pat_v][pat_r];
        switch (b) {
        case BTN_UP: pat_r = wrap(pat_r - 1, NROW); break;
        case BTN_DOWN: pat_r = wrap(pat_r + 1, NROW); break;
        case BTN_LEFT: pat_v = wrap(pat_v - 1, NV); break;
        case BTN_RIGHT: pat_v = wrap(pat_v + 1, NV); break;
        case BTN_A: c->on = !c->on; break;
        case BTN_B:
            c->hits = c->hits > 1 ? c->hits - 1 : c->steps;
            say("%s %s  %d/%d", voice_names[pat_v], row_names[pat_r], c->hits, c->steps);
            break;
        case BTN_Y:
            euclid_permute(c);
            say("%s %s  E(%d,%d) >%d", voice_names[pat_v], row_names[pat_r], c->hits, c->steps, c->rot);
            break;
        case BTN_X:
            c->rot = (c->rot + 1) % c->steps;
            say("%s %s  >%d", voice_names[pat_v], row_names[pat_r], c->rot);
            break;
        }
    }
    else if (page == PAGE_MIX) {
        switch (b) {
        case BTN_LEFT: mix_v = wrap(mix_v - 1, NV + 1); break;
        case BTN_RIGHT: mix_v = wrap(mix_v + 1, NV + 1); break;
        case BTN_UP: mix_adjust(+1); break;
        case BTN_DOWN: mix_adjust(-1); break;
        case BTN_A:
            mix_p = wrap(mix_p + 1, 3);
            if (mix_p == MIX_PAN && (mix_v == NV || song.out_mode == OUT_MONO_CLOCK))
                mix_p = MIX_DRIVE; /* no pan on the master, or in mono + clock mode */
            say("%s", mix_p_names[mix_p]);
            break;
        case BTN_Y:
            if (mix_v < NV)
                song.mute[mix_v] = !song.mute[mix_v];
            break;
        case BTN_X:
            if (mix_v < NV)
                song.solo[mix_v] = !song.solo[mix_v];
            break;
        }
    }
    else if (page == PAGE_SONG)
        song_action(b);
    else if (page == PAGE_FX) {
        switch (b) {
        case BTN_UP: fx_r = wrap(fx_r - 1, NFXSLOT + 1); break;
        case BTN_DOWN: fx_r = wrap(fx_r + 1, NFXSLOT + 1); break;
        case BTN_LEFT: fx_c = wrap(fx_c - 1, 4); break;
        case BTN_RIGHT: fx_c = wrap(fx_c + 1, 4); break;
        case BTN_Y: adjust_fx(-1, repeats); break;
        case BTN_X: adjust_fx(+1, repeats); break;
        }
    }
    else {
        switch (b) {
        case BTN_UP: syn_p = wrap(syn_p - 1, nparam(syn_v)); break;
        case BTN_DOWN: syn_p = wrap(syn_p + 1, nparam(syn_v)); break;
        case BTN_LEFT:
        case BTN_RIGHT:
            syn_v = wrap(syn_v + (b == BTN_RIGHT ? 1 : -1), NV);
            if (syn_p >= nparam(syn_v))
                syn_p = nparam(syn_v) - 1;
            break;
        case BTN_Y: adjust_param(syn_v, syn_p, -1, repeats); break;
        case BTN_X: adjust_param(syn_v, syn_p, +1, repeats); break;
        case BTN_A: engine_audition(syn_v); break;
        }
    }
}

static int repeatable(int b)
{
    if (is_dpad(b))
        return !held[BTN_R2];
    if (b == BTN_X && held[BTN_R2])
        return 0; /* tap tempo never auto-repeats */
    return (page == PAGE_SYNTH || page == PAGE_FX) && (b == BTN_X || b == BTN_Y);
}

void ui_button(int b, int down)
{
    if (b < 0 || b >= BTN_COUNT || held[b] == down)
        return;
    held[b] = down;
    hold_ms[b] = rep_ms[b] = rep_n[b] = 0;
    ui_lock();
    if (b == BTN_L2)
        live.filter_held = down;
    if (down && is_dpad(b))
        note_dpad_press(b);
    if (down)
        action(b, 0);
    update_fx(); /* also picks up FX type edits */
    ui_unlock();
}

void ui_tick(int ms)
{
    now_ms += ms;
    if (toast_ms > 0)
        toast_ms -= ms;
    for (int b = 0; b < BTN_COUNT; b++) {
        if (!held[b] || !repeatable(b))
            continue;
        hold_ms[b] += ms;
        if (hold_ms[b] < 300)
            continue;
        rep_ms[b] += ms;
        int interval = hold_ms[b] > 1500 ? 35 : 75;
        while (rep_ms[b] >= interval) {
            rep_ms[b] -= interval;
            ui_lock();
            action(b, ++rep_n[b]);
            update_fx();
            ui_unlock();
        }
    }
    float k = expf(-ms / 120.f);
    for (int v = 0; v <= NV; v++) {
        float m = live.meter[v];
        meter_disp[v] = m > meter_disp[v] * k ? m : meter_disp[v] * k;
    }
    for (int v = 0; v < NV; v++) {
        uint32_t t = live.trig_count[v];
        if (t != seen_trig[v]) {
            seen_trig[v] = t;
            flash[v] = 1.f;
        }
        else
            flash[v] *= expf(-ms / 90.f);
    }
}

/* -------------------------------------------------------------- drawing */

static void text_c(int cx, int y, const char *s, uint32_t c, int font)
{
    gfx_text(cx - gfx_text_w(s, font) / 2, y, s, c, font);
}

static void play_icon(int x, int y, int h, uint32_t c)
{
    for (int j = 0; j < h; j++) {
        int w = (j < h / 2 ? j : h - 1 - j) + 1;
        gfx_fill(x, y + j, w, 1, c);
    }
}

/* a small header badge, right-aligned at *bx; moves *bx left past it */
static void badge(int *bx, const char *s, uint32_t bg, uint32_t fg)
{
    int w = gfx_text_w(s, FONT_SMALL) + 10;
    *bx -= w;
    gfx_fill(*bx, 11, w, 22, bg);
    gfx_text(*bx + 5, 11, s, fg, FONT_SMALL);
    *bx -= 6;
}

static void draw_header(void)
{
    gfx_fill(0, 0, SCREEN_W, HEADER_H, C_PANEL);
    gfx_text(10, 2, "HI5", C_TEXT, FONT_LARGE);

    int x = 76;
    for (int p = 0; p < PAGE_COUNT; p++) {
        int w = gfx_text_w(page_names[p], FONT_SMALL) + 18;
        if (p == page)
            gfx_fill(x, 10, w, 24, C_ACCENT);
        else
            gfx_rect(x, 10, w, 24, 1, C_LINE);
        gfx_text(x + 9, 11, page_names[p], p == page ? 0xffffff : C_DIM, FONT_SMALL);
        x += w + 6;
    }

    char bpm[16];
    snprintf(bpm, sizeof bpm, "%.1f", song.bpm);
    int bw = gfx_text_w(bpm, FONT_LARGE);
    gfx_text(SCREEN_W - 10 - bw, 2, bpm, C_TEXT, FONT_LARGE);
    int ix = SCREEN_W - 10 - bw - 26, bx = ix - 10;
    if (song.out_mode == OUT_MONO_CLOCK)
        badge(&bx, "CLK", C_SOLO, 0x000000);
    if (song.sync_mode != SYNC_OFF) {
        const SyncStatus *ss = sync_status();
        int linked = ss->leading || ss->have_leader;
        badge(&bx, ss->leading ? "LEAD" : ss->have_leader ? "FOLLOW" : "SYNC", linked ? 0x50aaff : C_PANEL2,
              linked ? 0x000000 : C_DIM);
    }
    if (song.song_mode) {
        /* song mode: the chain step and slot that are playing */
        char s[16];
        int n = chain_len();
        if (n && live.chain_pos < n && song.chain_slot[live.chain_pos])
            snprintf(s, sizeof s, "%d:%c", live.chain_pos + 1, 'A' + song.chain_slot[live.chain_pos] - 1);
        else
            snprintf(s, sizeof s, "SONG");
        badge(&bx, s, C_PLAY, 0x000000);
    }
    if (live.playing)
        play_icon(ix, 12, 20, C_PLAY);
    else {
        gfx_fill(ix, 12, 6, 20, C_DIM);
        gfx_fill(ix + 10, 12, 6, 20, C_DIM);
    }

    int cur = live.step % 16;
    for (int i = 0; i < 16; i++) {
        uint32_t c = i % 4 == 0 ? C_LINE + 0x101010 : C_LINE;
        if (i == cur && live.playing)
            c = C_TEXT;
        gfx_fill(i * 40 + 1, HEADER_H + 3, 38, 5, c);
    }
}

static void draw_footer(void)
{
    static const char *const hints[PAGE_COUNT] = {
        "A:ON/OFF B:FEWER Y:EUCLID X:ROTATE R2+X:TAP",
        "<>:VOICE ^v:ADJUST A:LEVEL/PAN/DRIVE Y:MUTE X:SOLO",
        "DPAD:SELECT Y:- X:+ A:AUDITION",
        "DPAD:SELECT Y:- X:+ R2+DPAD:PLAY",
        "", /* song page: depends on the cursor */
    };
    const char *hint = hints[page];
    if (page == PAGE_SONG)
        hint = sg_cur == SG_SYNC ? "^v:OFF/AUTO/LEAD/FOLLOW  (ONION WIFI MUST BE ON)"
               : sg_on_chain()   ? "^v:SLOT A:BARS X:PASTE Y:COPY R2+A:PLAY B:SONG"
                                 : "A:LOAD X:SAVE Y:COPY R2+Y:CLEAR B:SONG";
    gfx_fill(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, C_PANEL);
    gfx_text(8, FOOTER_Y + 2, hint, C_DIM, FONT_SMALL);

    int x = SCREEN_W - 8;
    char s[48] = "";
    if (live.fx_src >= 0)
        snprintf(s, sizeof s, "%s", fx_type_name(song.fx[live.fx_src].type));
    for (int sl = 0; sl < NFXSLOT; sl++)
        if (live.fx_held[sl] && !fx_is_source(song.fx[sl].type) && strlen(s) < 30)
            snprintf(s + strlen(s), sizeof s - strlen(s), "%s%s", s[0] ? "+" : "",
                     fx_type_name(song.fx[sl].type));
    if (s[0]) {
        int w = gfx_text_w(s, FONT_SMALL) + 12;
        x -= w;
        gfx_fill(x, FOOTER_Y + 2, w, 20, C_MUTE);
        gfx_text(x + 6, FOOTER_Y + 2, s, 0xffffff, FONT_SMALL);
        x -= 6;
    }
    if (live.filter_held) {
        int w = 96;
        x -= w;
        gfx_fill(x, FOOTER_Y + 2, w, 20, C_PANEL2);
        gfx_fill(x, FOOTER_Y + 2, (int)(w * live.cutoff_norm), 20, C_ACCENT);
        gfx_text(x + 6, FOOTER_Y + 2, "LPF", 0xffffff, FONT_SMALL);
    }
}

static void draw_toast(void)
{
    if (toast_ms <= 0)
        return;
    int w = gfx_text_w(toast, FONT_SMALL) + 32, h = 34;
    int x = (SCREEN_W - w) / 2, y = FOOTER_Y - h - 8;
    gfx_fill_a(x, y, w, h, 0x000000, 210);
    gfx_rect(x, y, w, h, 2, C_ACCENT);
    gfx_text(x + 16, y + 6, toast, C_TEXT, FONT_SMALL);
}

static int voice_active(int v)
{
    int any_solo = 0;
    for (int i = 0; i < NV; i++)
        any_solo |= song.solo[i];
    return !song.mute[v] && (!any_solo || song.solo[v]);
}

static void draw_voice_headers(int sel)
{
    for (int v = 0; v < NV; v++) {
        int x = v * 128 + 3;
        uint32_t bg = gfx_mix(C_PANEL, vcol[v], 0.15f + 0.6f * flash[v]);
        gfx_fill(x, BODY_Y, 122, 22, bg);
        if (v == sel)
            gfx_fill(x, BODY_Y + 20, 122, 2, vcol[v]);
        text_c(x + 61, BODY_Y + 1, voice_names[v], voice_active(v) ? vcol[v] : C_DIM, FONT_SMALL);
    }
}

static void draw_pattern(void)
{
    draw_voice_headers(pat_v);
    const int y0 = BODY_Y + 24, rh = (FOOTER_Y - 2 - y0) / NROW;
    for (int v = 0; v < NV; v++)
        for (int r = 0; r < NROW; r++) {
            const Cell *c = &song.cell[v][r];
            int x = v * 128 + 3, y = y0 + r * rh, w = 122, h = rh - 4;
            int sel = v == pat_v && r == pat_r;
            uint32_t col = c->on ? vcol[v] : gfx_mix(C_DIM, vcol[v], 0.25f);
            gfx_fill(x, y, w, h, sel ? gfx_mix(C_PANEL2, vcol[v], 0.12f) : (c->on ? C_PANEL2 : C_PANEL));
            if (sel)
                gfx_rect(x, y, w, h, 3, vcol[v]);
            else
                gfx_rect(x, y, w, h, 1, C_LINE);

            gfx_text(x + 7, y + 3, row_names[r], c->on ? col : C_DIM, FONT_SMALL);
            char s[16];
            snprintf(s, sizeof s, ">%d", c->rot);
            gfx_text(x + w - 7 - gfx_text_w(s, FONT_SMALL), y + 3, s, C_DIM, FONT_SMALL);

            float cx = x + w / 2.f, cy = y + 24 + (h - 24) / 2.f, rad = (h - 24) / 2.f - 9.f;
            gfx_ring(cx, cy, rad, 1.5f, C_LINE);
            int n = c->steps, ph = live.step % n;
            for (int i = 0; i < n; i++) {
                float a = -1.5707963f + 6.2831853f * i / n;
                float px = cx + rad * cosf(a), py = cy + rad * sinf(a);
                if (euclid_hit(c, i))
                    gfx_disc(px, py, 6.f, col);
                else
                    gfx_disc(px, py, 2.5f, C_DIM);
                if (i == ph && (live.playing || live.step > 0)) {
                    gfx_ring(px, py, 9.5f, 2.f, C_TEXT);
                    if (c->on && euclid_hit(c, i) && flash[v] > 0.3f)
                        gfx_disc(px, py, 6.f, 0xffffff);
                }
            }
            if (c->on)
                snprintf(s, sizeof s, "%d/%d", c->hits, n);
            else
                snprintf(s, sizeof s, "OFF");
            text_c((int)cx, (int)cy - gfx_font_h(FONT_SMALL) / 2, s, c->on ? C_TEXT : C_DIM, FONT_SMALL);
        }
}

static void draw_mix(void)
{
    const int cw = SCREEN_W / (NV + 1), ty = BODY_Y + 46, th = 200;
    for (int i = 0; i <= NV; i++) {
        int x = i * cw + 2, w = cw - 4;
        int is_master = i == NV;
        uint32_t col = is_master ? C_TEXT : vcol[i];
        int on = is_master || voice_active(i);
        int vol = is_master ? song.master : song.vol[i];
        if (i == mix_v) {
            gfx_fill(x, BODY_Y, w, FOOTER_Y - 4 - BODY_Y, gfx_mix(C_PANEL2, col, 0.10f));
            gfx_rect(x, BODY_Y, w, FOOTER_Y - 4 - BODY_Y, 3, col);
        }
        else
            gfx_fill(x, BODY_Y, w, FOOTER_Y - 4 - BODY_Y, C_PANEL);
        text_c(x + w / 2, BODY_Y + 8, is_master ? "MASTER" : voice_names[i], on ? col : C_DIM, FONT_SMALL);

        int fx = x + w / 2 - 16;
        int lvl_sel = i == mix_v && mix_p == MIX_LEVEL;
        gfx_fill(fx, ty, 14, th, C_LINE);
        int fh = th * vol / 100;
        gfx_fill(fx, ty + th - fh, 14, fh, on ? col : C_DIM);
        gfx_fill(fx - 8, ty + th - fh - 4, 30, 8, lvl_sel || i != mix_v ? 0xffffff : 0x9a9aaa);

        float m = sqrtf(fminf(1.f, meter_disp[i]));
        int mh = (int)(th * m);
        gfx_fill(fx + 26, ty, 10, th, C_PANEL2);
        gfx_fill(fx + 26, ty + th - mh, 10, mh, m > 0.95f ? C_MUTE : C_PLAY);

        char s[8];
        snprintf(s, sizeof s, "%d", vol);
        text_c(x + w / 2, ty + th + 6, s, C_TEXT, FONT_SMALL);

        /* pan and drive rows: label, value, and a thin bar */
        for (int row = MIX_PAN; row <= MIX_DRIVE; row++) {
            int ry = ty + th + 36 + (row - MIX_PAN) * 34, bx = x + 8, bw = w - 16;
            int sel = i == mix_v && mix_p == row;
            int pan = row == MIX_PAN && !is_master;
            if (row == MIX_PAN && is_master)
                continue;
            int val = pan ? song.pan[i] : is_master ? song.master_drive : song.drive[i];
            int mono = pan && song.out_mode == OUT_MONO_CLOCK;
            if (mono)
                snprintf(s, sizeof s, "MONO");
            else if (pan)
                snprintf(s, sizeof s, val == 0 ? "C" : val < 0 ? "L%d" : "R%d", abs(val));
            else
                snprintf(s, sizeof s, "%d", val);
            gfx_text(bx, ry, mix_p_names[row], sel ? 0xffffff : C_DIM, FONT_SMALL);
            gfx_text(bx + bw - gfx_text_w(s, FONT_SMALL), ry, s, mono ? C_DIM : sel ? col : C_TEXT, FONT_SMALL);
            gfx_fill(bx, ry + 20, bw, 6, C_LINE);
            if (mono)
                continue; /* pan is greyed out: the output is summed to mono */
            if (pan) {
                int cx = bx + bw / 2, dx = val * (bw / 2) / 100;
                gfx_fill(dx < 0 ? cx + dx : cx, ry + 20, abs(dx) + 1, 6, sel ? col : C_DIM);
                gfx_fill(cx - 1, ry + 18, 2, 10, 0xffffff);
            }
            else
                gfx_fill(bx, ry + 20, bw * val / 100, 6, sel ? col : C_DIM);
        }

        if (!is_master) {
            int by = FOOTER_Y - 40;
            gfx_fill(x + 8, by, 38, 28, song.mute[i] ? C_MUTE : C_PANEL2);
            text_c(x + 27, by + 4, "M", song.mute[i] ? 0xffffff : C_DIM, FONT_SMALL);
            gfx_fill(x + w - 46, by, 38, 28, song.solo[i] ? C_SOLO : C_PANEL2);
            text_c(x + w - 27, by + 4, "S", song.solo[i] ? 0x000000 : C_DIM, FONT_SMALL);
        }
    }
}

static void draw_synth(void)
{
    draw_voice_headers(syn_v);
    const int y0 = BODY_Y + 24;
    for (int v = 0; v < NV; v++) {
        int np = nparam(v), rh = (FOOTER_Y - 2 - y0) / np;
        for (int p = 0; p < np; p++) {
            const ParamDef *d = param_def(v, p);
            int x = v * 128 + 3, y = y0 + p * rh, w = 122, h = rh - 3;
            int sel = v == syn_v && p == syn_p;
            gfx_fill(x, y, w, h, sel ? gfx_mix(C_PANEL2, vcol[v], 0.15f) : C_PANEL);
            if (sel)
                gfx_rect(x, y, w, h, 2, vcol[v]);
            gfx_text(x + 5, y + 1, d->name, sel ? vcol[v] : C_DIM, FONT_SMALL);
            char s[16];
            param_format(v, p, s, sizeof s);
            gfx_text(x + w - 5 - gfx_text_w(s, FONT_SMALL), y + 1, s, C_TEXT, FONT_SMALL);

            int bx = x + 5, bw = w - 10, by = y + h - 9;
            gfx_fill(bx, by, bw, 5, C_LINE);
            int hi = param_max(v, p), lo = d->min, val = *param_ptr(v, p);
            float t = hi > lo ? (float)(val - lo) / (hi - lo) : 0.f;
            if (lo < 0) {
                int mid = bx + bw / 2, end = bx + (int)(bw * t);
                gfx_fill(end < mid ? end : mid, by, abs(end - mid) + 1, 5, vcol[v]);
            }
            else
                gfx_fill(bx, by, (int)(bw * t), 5, vcol[v]);
        }
    }
}

/* the song page keeps the pattern page's rings: each slot is a small ring
 * whose 15 dots are the voice rows it holds, and the chain is one big ring
 * of 16 nodes that the playhead walks around */
static void draw_song(void)
{
    gfx_text(14, BODY_Y + 2, "PATTERNS", C_DIM, FONT_SMALL);
    for (int i = 0; i < NSLOT; i++) {
        float cx = 50 + (i % 4) * 74, cy = BODY_Y + 96 + (i / 4) * 150;
        int used = song.slot_used[i], sel = sg_cur == i, cur = live.cur_slot == i;
        if (sel)
            gfx_ring(cx, cy, 40, 2.5f, C_ACCENT);
        gfx_ring(cx, cy, 25, cur ? 2.5f : 1.5f, used ? (cur ? C_TEXT : C_DIM) : C_LINE);
        char l[2] = {(char)('A' + i), 0};
        text_c((int)cx, (int)cy - gfx_font_h(FONT_LARGE) / 2, l, used ? C_TEXT : C_LINE, FONT_LARGE);
        if (!used)
            continue;
        for (int v = 0; v < NV; v++)
            for (int r = 0; r < NROW; r++) {
                float a = -1.5707963f + 6.2831853f * (v * NROW + r) / (NV * NROW);
                int on = song.slot_cell[i][v][r].on && !song.slot_mute[i][v];
                gfx_disc(cx + 32.f * cosf(a), cy + 32.f * sinf(a), on ? 3.f : 1.5f, on ? vcol[v] : C_DIM);
            }
    }

    const float cx = 466.f, cy = BODY_Y + 200.f, R = 136.f;
    int n = chain_len();
    gfx_ring(cx, cy, R, 1.5f, C_LINE);
    for (int i = 0; i < NCHAIN; i++) {
        float a = -1.5707963f + 6.2831853f * i / NCHAIN;
        float px = cx + R * cosf(a), py = cy + R * sinf(a);
        int s = song.chain_slot[i], sel = sg_cur == NSLOT + i, in = i < n;
        int playing = song.song_mode && in && live.chain_pos == i;
        if (s) {
            uint32_t disc = in ? gfx_mix(C_PANEL2, C_ACCENT, 0.55f) : C_PANEL2;
            gfx_disc(px, py, 15.f, disc);
            char l[2] = {(char)('A' + s - 1), 0};
            text_c((int)px, (int)py - gfx_font_h(FONT_SMALL) / 2, l, in ? 0xffffff : C_DIM, FONT_SMALL);
            if (song.chain_bars[i] > 1) {
                char b[6];
                snprintf(b, sizeof b, "x%d", song.chain_bars[i]);
                text_c((int)(cx + (R + 36.f) * cosf(a)), (int)(cy + (R + 36.f) * sinf(a)) - 9, b,
                       in ? C_TEXT : C_DIM, FONT_SMALL);
            }
        }
        else
            gfx_disc(px, py, 3.f, C_DIM);
        if (playing)
            gfx_ring(px, py, 19.f, 2.f, C_TEXT);
        if (sel)
            gfx_ring(px, py, playing ? 24.f : 20.f, 2.5f, C_ACCENT);
    }

    /* wifi sync panel under the slots */
    {
        const SyncStatus *ss = sync_status();
        int x = 14, y = BODY_Y + 304, w = 282, h = FOOTER_Y - 10 - y, sel = sg_cur == SG_SYNC;
        int on = song.sync_mode != SYNC_OFF, linked = ss->leading || ss->have_leader;
        gfx_fill(x, y, w, h, sel ? gfx_mix(C_PANEL2, C_ACCENT, 0.18f) : C_PANEL);
        if (sel)
            gfx_rect(x, y, w, h, 2, C_ACCENT);
        gfx_text(x + 8, y + 4, "WIFI SYNC", sel ? C_ACCENT + 0x303030 : C_DIM, FONT_SMALL);
        gfx_text(x + 7, y + 25, sync_mode_names[song.sync_mode], on ? 0x50aaff : C_TEXT, FONT_LARGE);
        char st[40];
        if (!on)
            snprintf(st, sizeof st, "SYNC TEMPO WITH OTHER HI5S");
        else if (ss->have_leader)
            snprintf(st, sizeof st, "FOLLOWING  %d PEER%s  %+dms", ss->peers, ss->peers == 1 ? "" : "S",
                     (int)ss->offset_ms);
        else if (ss->leading)
            snprintf(st, sizeof st, "LEADING  %d PEER%s", ss->peers, ss->peers == 1 ? "" : "S");
        else
            snprintf(st, sizeof st, "NO PEERS YET");
        gfx_text(x + 8, y + h - 24, st, linked ? C_TEXT : C_DIM, FONT_SMALL);
    }

    /* centre: mode and where the playhead is */
    char s[32];
    text_c((int)cx, (int)cy - 30, song.song_mode ? "SONG" : "LOOP", song.song_mode ? C_PLAY : C_DIM, FONT_LARGE);
    if (song.song_mode && n && live.chain_pos < n && song.chain_slot[live.chain_pos])
        snprintf(s, sizeof s, "%c  BAR %d/%d", 'A' + song.chain_slot[live.chain_pos] - 1, live.chain_bar + 1,
                 song.chain_bars[live.chain_pos]);
    else if (n == 0)
        snprintf(s, sizeof s, "CHAIN EMPTY");
    else if (live.cur_slot >= 0)
        snprintf(s, sizeof s, "PATTERN %c", 'A' + live.cur_slot);
    else
        snprintf(s, sizeof s, "%d STEPS", n);
    text_c((int)cx, (int)cy + 4, s, C_TEXT, FONT_SMALL);
    if (clip.has) {
        snprintf(s, sizeof s, "CLIP %c x%d", 'A' + clip.slot - 1, clip.bars);
        text_c((int)cx, (int)cy + 26, s, C_DIM, FONT_SMALL);
    }
}

static void arrow(int cx, int cy, int r, int dir, uint32_t c)
{
    for (int j = 0; j <= r; j++) {
        int len = 2 * j + 1;
        switch (dir) {
        case BTN_UP: gfx_fill(cx - j, cy - r / 2 + j, len, 1, c); break;
        case BTN_DOWN: gfx_fill(cx - j, cy + r / 2 - j, len, 1, c); break;
        case BTN_LEFT: gfx_fill(cx - r / 2 + j, cy - j, 1, len, c); break;
        case BTN_RIGHT: gfx_fill(cx + r / 2 - j, cy - j, 1, len, c); break;
        }
    }
}

static void fx_cell(int x, int y, int w, int h, int sel, int dim, const char *label,
                    const char *value, uint32_t vcolr, float bar)
{
    gfx_fill(x, y, w, h, sel ? gfx_mix(C_PANEL2, C_ACCENT, 0.18f) : C_PANEL);
    if (sel)
        gfx_rect(x, y, w, h, 2, C_ACCENT);
    gfx_text(x + 8, y + 4, label, sel ? C_ACCENT + 0x303030 : C_DIM, FONT_SMALL);
    gfx_text(x + 7, y + 25, value, dim ? C_DIM : vcolr, FONT_LARGE);
    if (bar >= 0.f) {
        gfx_fill(x + 8, y + h - 10, w - 16, 5, C_LINE);
        gfx_fill(x + 8, y + h - 10, (int)((w - 16) * bar), 5, dim ? C_DIM : C_ACCENT);
    }
}

static void draw_fx(void)
{
    const int lx = 3, lw = 58, cw = 143, x0 = lx + lw + 4;
    const int rh = (FOOTER_Y - 2 - BODY_Y) / (NFXSLOT + 1), h = rh - 4;
    char v[24];
    for (int r = 0; r <= NFXSLOT; r++) {
        int y = BODY_Y + r * rh;
        if (r < NFXSLOT) {
            const FxSlot *sl = &song.fx[r];
            int on = live.fx_held[r] && (live.fx_src == r || !fx_is_source(sl->type));
            gfx_fill(lx, y, lw, h, on ? C_MUTE : C_PANEL2);
            gfx_text(lx + 11, y + 6, "R2", on ? 0xffffff : C_DIM, FONT_SMALL);
            arrow(lx + lw / 2, y + 48, 12, r, on ? 0xffffff : C_TEXT);

            int src = fx_is_source(sl->type);
            fx_cell(x0, y, cw - 3, h, fx_r == r && fx_c == 0, 0, src ? "TYPE  BUFFER" : "TYPE  INSERT",
                    fx_type_name(sl->type), src ? 0xff7a6a : 0x50aaff, -1.f);
            for (int k = 0; k < 2; k++) {
                const char *name;
                int lo, hi, val = k ? sl->p2 : sl->p1;
                fx_param_range(sl->type, k, &name, &lo, &hi);
                fx_param_format(sl->type, k, val, v, sizeof v);
                fx_cell(x0 + cw * (k + 1), y, cw - 3, h, fx_r == r && fx_c == k + 1, 0, name, v,
                        C_TEXT, (float)(val - lo) / (hi - lo));
            }
            snprintf(v, sizeof v, "%d%%", sl->mix);
            fx_cell(x0 + cw * 3, y, cw - 3, h, fx_r == r && fx_c == 3, 0, "MIX", v, C_TEXT, sl->mix / 100.f);
        }
        else {
            int clk = song.out_mode == OUT_MONO_CLOCK;
            gfx_fill(lx, y, lw, h, clk ? C_SOLO : C_PANEL2);
            text_c(lx + lw / 2, y + 26, "OUT", clk ? 0x000000 : C_DIM, FONT_SMALL);
            fx_cell(x0, y, cw - 3, h, fx_r == r && fx_c == 0, 0, "OUTPUT", clk ? "MONO+CLK" : "STEREO",
                    clk ? C_SOLO : C_TEXT, -1.f);
            fx_cell(x0 + cw, y, cw - 3, h, fx_r == r && fx_c == 1, !clk, "CLOCK ON",
                    song.clk_side ? "LEFT" : "RIGHT", C_TEXT, -1.f);
            int ppqn = clk_ppqn_value(song.clk_ppqn);
            snprintf(v, sizeof v, "%d", ppqn);
            fx_cell(x0 + cw * 2, y, cw - 3, h, fx_r == r && fx_c == 2, !clk,
                    ppqn == 2 ? "PPQN  PO/VOLCA" : ppqn == 24 ? "PPQN  DIN" : "PPQN", v, C_TEXT,
                    (float)song.clk_ppqn / (NPPQN - 1));
            snprintf(v, sizeof v, "%dms", song.clk_width);
            fx_cell(x0 + cw * 3, y, cw - 3, h, fx_r == r && fx_c == 3, !clk, "PULSE", v, C_TEXT,
                    (song.clk_width - 1) / 19.f);
        }
    }
}

void ui_draw(uint32_t *fb)
{
    gfx_target(fb);
    gfx_clear(C_BG);
    draw_header();
    if (page == PAGE_PATTERN)
        draw_pattern();
    else if (page == PAGE_MIX)
        draw_mix();
    else if (page == PAGE_SONG)
        draw_song();
    else if (page == PAGE_SYNTH)
        draw_synth();
    else
        draw_fx();
    draw_footer();
    draw_toast();
}
