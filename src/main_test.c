/* Headless host harness: plays a scripted button sequence, writes a WAV of the
 * audio, PPM screenshots of each page, and times the DSP.
 *
 * usage: hi5_test OUTDIR [SAMPLE_DIR] */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "engine.h"
#include "gfx.h"
#include "ui.h"

#define RATE 48000

typedef struct {
    int ms, btn, down;
    const char *shot;
} Ev;

#define PRESS(t, b) {t, b, 1, NULL}, {t + 60, b, 0, NULL}
#define SHOT(t, name) {t, -1, 0, name}

static const Ev script[] = {
    PRESS(0, BTN_START),
    SHOT(2900, "pattern"),
    /* punch-in FX */
    {4000, BTN_R2, 1, NULL},
    {4050, BTN_UP, 1, NULL},
    SHOT(5200, "fx_repeat"),
    {6000, BTN_UP, 0, NULL},
    {6100, BTN_RIGHT, 1, NULL},
    {7000, BTN_RIGHT, 0, NULL},
    {7100, BTN_LEFT, 1, NULL},
    {8500, BTN_LEFT, 0, NULL},
    {8600, BTN_DOWN, 1, NULL},
    {10000, BTN_DOWN, 0, NULL},
    {10100, BTN_R2, 0, NULL},
    /* L2 filter sweep */
    {11000, BTN_L2, 1, NULL},
    SHOT(12300, "filter"),
    {15000, BTN_L2, 0, NULL},
    /* pattern edits: move to PERC/STUT, permute, thin out */
    PRESS(15200, BTN_RIGHT), PRESS(15300, BTN_RIGHT), PRESS(15400, BTN_RIGHT),
    PRESS(15500, BTN_DOWN), PRESS(15600, BTN_Y), PRESS(15700, BTN_B), PRESS(15800, BTN_B),
    SHOT(15900, "pattern_edit"),
    /* rotate the cell twice, then tap tempo at 140 BPM with R2 held */
    PRESS(15910, BTN_X), PRESS(15980, BTN_X),
    {16050, BTN_R2, 1, NULL},
    PRESS(16100, BTN_X), PRESS(16529, BTN_X), PRESS(16957, BTN_X), PRESS(17386, BTN_X),
    {17460, BTN_R2, 0, NULL},
    /* mix page */
    PRESS(17500, BTN_R1), PRESS(17600, BTN_RIGHT), PRESS(17700, BTN_RIGHT), PRESS(17800, BTN_Y),
    PRESS(17900, BTN_RIGHT), PRESS(18000, BTN_X), PRESS(18100, BTN_DOWN), PRESS(18200, BTN_DOWN),
    /* A -> PAN, nudge right twice; A -> DRIVE, up three */
    PRESS(18270, BTN_A), PRESS(18340, BTN_UP), PRESS(18410, BTN_UP),
    PRESS(18480, BTN_A), PRESS(18550, BTN_UP), PRESS(18620, BTN_UP), PRESS(18690, BTN_UP),
    SHOT(18800, "mix"),
    PRESS(18850, BTN_X), PRESS(18900, BTN_LEFT), PRESS(18950, BTN_Y),
    /* synth page: KICK ratio up, hat loop tweaks */
    PRESS(19000, BTN_R1), PRESS(19100, BTN_RIGHT), PRESS(19200, BTN_DOWN), PRESS(19300, BTN_X),
    {19400, BTN_X, 1, NULL}, {20400, BTN_X, 0, NULL},
    SHOT(20500, "synth"),
    PRESS(21000, BTN_START),
    SHOT(21100, "paused"),
    /* FX page: slot DOWN -> GATE, play it, then switch output to mono + clock */
    PRESS(21500, BTN_START), PRESS(21600, BTN_R1), PRESS(21700, BTN_DOWN),
    PRESS(21800, BTN_X), PRESS(21900, BTN_X), PRESS(22000, BTN_X),
    {22300, BTN_R2, 1, NULL}, {22350, BTN_DOWN, 1, NULL}, {22400, BTN_RIGHT, 1, NULL},
    SHOT(23000, "fx_gate"),
    {23500, BTN_RIGHT, 0, NULL}, {23550, BTN_DOWN, 0, NULL}, {23600, BTN_R2, 0, NULL},
    PRESS(24000, BTN_DOWN), PRESS(24100, BTN_DOWN), PRESS(24200, BTN_DOWN), PRESS(24300, BTN_X),
    PRESS(24400, BTN_RIGHT), PRESS(24500, BTN_RIGHT),
    SHOT(25000, "fx_clock"),
};

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static void write_wav(const char *path, const int16_t *pcm, int frames)
{
    FILE *f = fopen(path, "wb");
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + frames * 4);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 2);
    put32(f, RATE);
    put32(f, RATE * 4);
    put16(f, 4);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, frames * 4);
    fwrite(pcm, 4, frames, f);
    fclose(f);
}

static void write_ppm(const char *path, const uint32_t *fb)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint8_t rgb[3] = {fb[i] >> 16, fb[i] >> 8, fb[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : ".";
    srand(1);
    song_default();
    engine_init(RATE);
    if (argc > 2)
        samples_load_dir(argv[2]);
    ui_init();

    const int total_ms = 26000, chunk_ms = 10, chunk = RATE * chunk_ms / 1000;
    int16_t *pcm = malloc(sizeof(int16_t) * 2 * RATE * total_ms / 1000);
    static uint32_t fb[SCREEN_W * SCREEN_H];
    size_t ev = 0;
    char path[512];
    for (int t = 0, f = 0; t < total_ms; t += chunk_ms, f += chunk) {
        for (; ev < sizeof script / sizeof *script && script[ev].ms <= t; ev++) {
            if (script[ev].shot) {
                ui_draw(fb);
                snprintf(path, sizeof path, "%s/%s.ppm", out, script[ev].shot);
                write_ppm(path, fb);
            }
            else
                ui_button(script[ev].btn, script[ev].down);
        }
        engine_render(pcm + 2 * f, chunk);
        ui_tick(chunk_ms);
    }
    snprintf(path, sizeof path, "%s/session.wav", out);
    write_wav(path, pcm, RATE * total_ms / 1000);
    printf("bpm after tap: %.1f, samples: %d, PERC/STUT rot after 2x X: %d\n", song.bpm, nsamples,
           song.cell[V_PERC][ROW_STUT].rot);
    printf("mix: PERC vol=%d pan=%d drive=%d\n", song.vol[V_PERC], song.pan[V_PERC], song.drive[V_PERC]);

    /* pan hard left: the right channel goes quiet; drive raises the RMS */
    for (int k = 0; k < 3; k++) {
        song_default();
        song.out_mode = OUT_STEREO;
        song.pan[V_KICK] = k == 0 ? -100 : 0;
        song.drive[V_KICK] = k == 2 ? 100 : 0;
        engine_set_playing(0);
        for (int i = 0; i < 4; i++) /* let every earlier voice tail die */
            engine_render(pcm, RATE);
        engine_audition(V_KICK);
        engine_render(pcm, RATE / 2);
        int pl = 0, pr = 0;
        double rms = 0;
        for (int i = 0; i < RATE / 2; i++) {
            if (abs(pcm[2 * i]) > pl)
                pl = abs(pcm[2 * i]);
            if (abs(pcm[2 * i + 1]) > pr)
                pr = abs(pcm[2 * i + 1]);
            rms += (double)pcm[2 * i] * pcm[2 * i];
        }
        printf("kick pan=%d drive=%d: peak L %d R %d, rms L %.0f\n", song.pan[V_KICK], song.drive[V_KICK],
               pl, pr, sqrt(rms / (RATE / 2)));
    }

    /* SUB at max decay: time for an audition to fall 40 dB below its peak */
    song_default();
    song.out_mode = OUT_STEREO;
    song.param[V_SUB][P_DECAY] = 100;
    engine_set_playing(0);
    engine_render(pcm, RATE); /* let earlier voices die */
    engine_audition(V_SUB);
    engine_render(pcm, RATE * 8);
    float pk = 0.f;
    int last = 0;
    for (int i = 0; i < RATE * 8; i++)
        if (abs(pcm[2 * i]) > pk)
            pk = (float)abs(pcm[2 * i]);
    for (int i = 0; i < RATE * 8; i++)
        if (abs(pcm[2 * i]) > pk * 0.01f)
            last = i;
    printf("sub: decay 100 -> -40 dB after %.2f s\n", (float)last / RATE);
    snprintf(path, sizeof path, "%s/sub808.wav", out);
    write_wav(path, pcm, RATE * 8);

    /* mono + clock output: pulse spacing and width on the clock channel */
    int ppqn_idx[3] = {1, 5, 6};
    for (int k = 0; k < 3; k++) {
        song_default();
        song.bpm = 120.f;
        song.out_mode = OUT_MONO_CLOCK;
        song.clk_ppqn = ppqn_idx[k];
        engine_set_playing(0);
        engine_render(pcm, 4800);
        engine_set_playing(1);
        engine_render(pcm, RATE * 4);
        int edges = 0, first = -1, prev = 0, width = 0, inside = 0;
        double sum_gap = 0;
        float lmax = 0.f;
        for (int i = 0; i < RATE * 4; i++) {
            int hi = pcm[2 * i + 1] > 16000;
            if (hi && !prev) {
                if (first >= 0)
                    sum_gap += i - inside;
                if (first < 0)
                    first = i;
                inside = i;
                edges++;
            }
            if (hi && edges == 1)
                width++;
            prev = hi;
            if (abs(pcm[2 * i]) > lmax)
                lmax = (float)abs(pcm[2 * i]);
        }
        printf("clock: %d PPQN at 120 BPM -> %d pulses in 4 s, mean gap %.2f ms, width %.2f ms, "
               "audio peak on L %.2f\n",
               clk_ppqn_value(ppqn_idx[k]), edges, sum_gap / (edges - 1) * 1000.0 / RATE,
               width * 1000.0 / RATE, lmax / 32768.f);
    }
    engine_set_playing(0);

    /* a v0.0.1 save (shorter Song, old magic) still loads */
    song_default();
    song.bpm = 99.f;
    song.magic = 0x48493501u;
    snprintf(path, sizeof path, "%s/v1.sav", out);
    FILE *sf = fopen(path, "wb");
    fwrite(&song, SONG_V1_SIZE, 1, sf);
    fclose(sf);
    song_default();
    int ok = song_load(path);
    printf("v0.0.1 save: loaded=%d bpm=%.0f fx0=%s out_mode=%d\n", ok, song.bpm,
           fx_type_name(song.fx[0].type), song.out_mode);

    /* a v0.0.2 save at 24 PPQN (then index 3) still means 24 PPQN, and
     * saves without pan / drive get the default pans */
    song_default();
    song.magic = 0x48493502u;
    song.clk_ppqn = 3;
    song.pan[V_HAT] = 99;
    snprintf(path, sizeof path, "%s/v2.sav", out);
    sf = fopen(path, "wb");
    fwrite(&song, SONG_V3_SIZE, 1, sf);
    fclose(sf);
    song_default();
    song.pan[V_HAT] = 0;
    ok = song_load(path);
    printf("v0.0.2 save: loaded=%d ppqn=%d hat pan=%d\n", ok, clk_ppqn_value(song.clk_ppqn), song.pan[V_HAT]);

    /* DSP benchmark: 60 s of the default song with every glitch row on */
    song_default();
    for (int v = 0; v < NV; v++)
        for (int r = 0; r < NROW; r++)
            song.cell[v][r].on = 1;
    engine_set_playing(1);
    live.filter_held = 1;
    clock_t c0 = clock();
    for (int i = 0; i < 60 * RATE / 1024; i++)
        engine_render(pcm, 1024);
    double secs = (double)(clock() - c0) / CLOCKS_PER_SEC;
    printf("dsp: 60 s of audio rendered in %.3f s (%.2f%% of one host core)\n", secs, secs / 60 * 100);

    c0 = clock();
    for (int i = 0; i < 300; i++)
        ui_draw(fb);
    secs = (double)(clock() - c0) / CLOCKS_PER_SEC;
    printf("ui: %.2f ms per frame\n", secs / 300 * 1000);
    free(pcm);
    return 0;
}
