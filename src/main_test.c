/* Headless host harness: plays a scripted button sequence, writes a WAV of the
 * audio, PPM screenshots of each page, and times the DSP.
 *
 * usage: hi5_test OUTDIR [SAMPLE_DIR] */
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
    /* tap tempo at 140 BPM */
    PRESS(16000, BTN_X), PRESS(16429, BTN_X), PRESS(16857, BTN_X), PRESS(17286, BTN_X),
    /* mix page */
    PRESS(17500, BTN_R1), PRESS(17600, BTN_RIGHT), PRESS(17700, BTN_RIGHT), PRESS(17800, BTN_Y),
    PRESS(17900, BTN_RIGHT), PRESS(18000, BTN_X), PRESS(18100, BTN_DOWN), PRESS(18200, BTN_DOWN),
    SHOT(18400, "mix"),
    PRESS(18500, BTN_X), PRESS(18600, BTN_LEFT), PRESS(18700, BTN_Y),
    /* synth page: KICK ratio up, hat loop tweaks */
    PRESS(19000, BTN_R1), PRESS(19100, BTN_RIGHT), PRESS(19200, BTN_DOWN), PRESS(19300, BTN_X),
    {19400, BTN_X, 1, NULL}, {20400, BTN_X, 0, NULL},
    SHOT(20500, "synth"),
    PRESS(21000, BTN_START),
    SHOT(21100, "paused"),
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

    const int total_ms = 22000, chunk_ms = 10, chunk = RATE * chunk_ms / 1000;
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
    printf("bpm after tap: %.1f, samples: %d\n", song.bpm, nsamples);

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
