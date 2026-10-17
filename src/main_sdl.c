/* Miyoo Mini (Plus) front-end: SDL 1.2 as shipped with OnionOS. */
#include <SDL/SDL.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "engine.h"
#include "gfx.h"
#include "ui.h"

#define SAVE_PATH "hi5.sav"
#define SAMPLE_DIR "samples"

static volatile sig_atomic_t got_signal;
static void on_signal(int s) { got_signal = s; }

static void lock(void) { SDL_LockAudio(); }
static void unlock(void) { SDL_UnlockAudio(); }

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    engine_render((int16_t *)stream, len / 4);
}

static int map_key(SDLKey k)
{
    switch (k) {
    case SDLK_UP: return BTN_UP;
    case SDLK_DOWN: return BTN_DOWN;
    case SDLK_LEFT: return BTN_LEFT;
    case SDLK_RIGHT: return BTN_RIGHT;
    case SDLK_SPACE: return BTN_A;
    case SDLK_LCTRL: return BTN_B;
    case SDLK_LSHIFT: return BTN_X;
    case SDLK_LALT: return BTN_Y;
    case SDLK_e: return BTN_L1;
    case SDLK_t: return BTN_R1;
    case SDLK_TAB: return BTN_L2;
    case SDLK_BACKSPACE: return BTN_R2;
    case SDLK_RCTRL: return BTN_SELECT;
    case SDLK_RETURN: return BTN_START;
    case SDLK_ESCAPE: return BTN_MENU;
    default: return -1;
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    srand((unsigned)time(NULL));
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    song_default();
    if (song_load(SAVE_PATH))
        fprintf(stderr, "hi5: restored %s\n", SAVE_PATH);

    int rate = getenv("HI5_RATE") ? atoi(getenv("HI5_RATE")) : 48000;
    int bufsz = getenv("HI5_BUFFER") ? atoi(getenv("HI5_BUFFER")) : 1024;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "hi5: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_ShowCursor(SDL_DISABLE);
    SDL_Surface *video = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 32, SDL_HWSURFACE);
    SDL_Surface *screen = SDL_CreateRGBSurface(SDL_HWSURFACE, SCREEN_W, SCREEN_H, 32,
                                               0x00ff0000, 0x0000ff00, 0x000000ff, 0);
    if (!video || !screen) {
        fprintf(stderr, "hi5: video: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    engine_init(rate);
    int extra = samples_load_dir(SAMPLE_DIR);
    fprintf(stderr, "hi5: %d samples (%d from %s/)\n", nsamples, extra, SAMPLE_DIR);

    SDL_AudioSpec want;
    memset(&want, 0, sizeof want);
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = (Uint16)bufsz;
    want.callback = audio_cb;
    int have_audio = SDL_OpenAudio(&want, NULL) == 0;
    if (have_audio) {
        fprintf(stderr, "hi5: audio %d Hz, %d frames/buffer\n", rate, bufsz);
        ui_lock = lock;
        ui_unlock = unlock;
        SDL_PauseAudio(0);
    }
    else
        fprintf(stderr, "hi5: SDL_OpenAudio: %s\n", SDL_GetError());

    ui_init();
    Uint32 last = SDL_GetTicks(), last_save = last;
    while (!ui_quit_requested() && !got_signal) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP)
                ui_button(map_key(e.key.keysym.sym), e.type == SDL_KEYDOWN);
            else if (e.type == SDL_QUIT)
                got_signal = 1;
        }
        Uint32 now = SDL_GetTicks();
        ui_tick((int)(now - last));
        last = now;

        if (SDL_MUSTLOCK(screen))
            SDL_LockSurface(screen);
        if (screen->pitch == SCREEN_W * 4)
            ui_draw((uint32_t *)screen->pixels);
        else {
            static uint32_t fb[SCREEN_W * SCREEN_H];
            ui_draw(fb);
            for (int y = 0; y < SCREEN_H; y++)
                memcpy((uint8_t *)screen->pixels + y * screen->pitch, fb + y * SCREEN_W, SCREEN_W * 4);
        }
        if (SDL_MUSTLOCK(screen))
            SDL_UnlockSurface(screen);
        SDL_BlitSurface(screen, NULL, video, NULL);
        SDL_Flip(video);

        if (now - last_save > 15000) {
            last_save = now;
            if (ui_take_changed())
                song_save(SAVE_PATH);
        }
        Uint32 spent = SDL_GetTicks() - now;
        if (spent < 33)
            SDL_Delay(33 - spent);
    }

    song_save(SAVE_PATH);
    if (have_audio)
        SDL_CloseAudio();
    SDL_Quit();
    return 0;
}
