/* HI5 audio engine: 5 voices (4 FM percussion + micro-loop hats), Euclidean
 * sequencer, punch-in FX, a tempo-synced master filter and an optional
 * mono + clock-pulse output. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define HI5_VERSION "0.0.4"

#define NV 5
#define NROW 3
#define NPARAM 9
#define MAX_STEPS 16
#define MAX_SAMPLES 24
#define NSWEEP 4
#define NFXSLOT 4 /* R2 + up / down / left / right */
#define NPPQN 7

enum { V_SUB, V_KICK, V_SNARE, V_PERC, V_HAT };
enum { ROW_HIT, ROW_STUT, ROW_GLITCH };
enum { TRIG_HIT = 1, TRIG_STUT = 2, TRIG_GLITCH = 4 };
/* punch-in effect types; the first three replay the output buffer and are
 * exclusive (latest press wins), the others are inserts that stack */
enum { FXT_REPEAT, FXT_TAPESTOP, FXT_REVERSE, FXT_CRUSH, FXT_GATE, NFXT };
enum { OUT_STEREO, OUT_MONO_CLOCK };

/* FM voice params (the SUB voice has DRIVE in the NOISE slot) */
enum { P_TUNE, P_RATIO, P_INDEX, P_DECAY, P_SWEEP, P_FEEDBK, P_NOISE, P_STUT, P_GLITCH };
#define P_DRIVE P_NOISE
/* hat voice params (P_DECAY, P_STUT, P_GLITCH shared with FM) */
enum { H_SAMPLE = 0, H_START = 1, H_LOOP = 2, H_PITCH = 4, H_TONE = 5, H_DRIFT = 6 };

typedef struct {
    uint8_t on, steps, hits, rot;
} Cell;

typedef struct {
    const char *name;
    int min, max;
} ParamDef;

typedef struct {
    int type, p1, p2, mix;
} FxSlot;

/* Everything the user edits; saved to disk as-is. Fields from sweep_rate
 * back form the v0.0.1 save layout, so new fields go at the end. */
typedef struct {
    uint32_t magic;
    Cell cell[NV][NROW];
    int param[NV][NPARAM];
    int vol[NV];
    uint8_t mute[NV], solo[NV];
    int master;
    float bpm;
    int sweep_rate;
    /* v0.0.2 */
    FxSlot fx[NFXSLOT];
    int out_mode, clk_side, clk_ppqn, clk_width;
} Song;

#define SONG_V1_SIZE offsetof(Song, fx)

typedef struct {
    float *data;
    int len;
    char name[12];
} Sample;

/* Shared between UI and audio thread. The UI writes these only while holding
 * the audio lock; the audio thread publishes playhead and meters. */
typedef struct {
    int playing;
    int filter_held;
    int fx_held[NFXSLOT]; /* R2 + direction held, per slot */
    int fx_src;           /* slot driving the buffer effect, or -1 */
    volatile int step;
    volatile uint32_t trig_count[NV];
    volatile float meter[NV + 1];
    volatile float cutoff_norm; /* 0..1, for the filter indicator */
} Live;

extern Song song;
extern Live live;
extern Sample samples[MAX_SAMPLES];
extern int nsamples;

extern const char *const voice_names[NV];

const ParamDef *param_def(int v, int p);
int param_max(int v, int p);
void param_format(int v, int p, char *buf, int n);

void engine_init(int sample_rate);
void engine_render(int16_t *out, int frames);
void engine_audition(int v);
void engine_set_playing(int on);

int euclid_hit(const Cell *c, int step);
void song_default(void);
int song_save(const char *path);
int song_load(const char *path);
int samples_load_dir(const char *dir);
const char *sweep_rate_name(int i);

const char *fx_type_name(int t);
int fx_is_source(int t);
/* which: 0 = P1, 1 = P2 */
void fx_param_range(int t, int which, const char **name, int *min, int *max);
void fx_param_format(int t, int which, int val, char *buf, int n);
void fx_slot_set_type(FxSlot *s, int t);
int clk_ppqn_value(int i);
