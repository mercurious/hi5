/* HI5 audio engine: 5 voices (4 FM percussion + micro-loop hats), Euclidean
 * sequencer, punch-in buffer FX and a tempo-synced master filter. */
#pragma once
#include <stdint.h>

#define NV 5
#define NROW 3
#define NPARAM 9
#define MAX_STEPS 16
#define MAX_SAMPLES 24
#define NSWEEP 4

enum { V_SUB, V_KICK, V_SNARE, V_PERC, V_HAT };
enum { ROW_HIT, ROW_STUT, ROW_GLITCH };
enum { TRIG_HIT = 1, TRIG_STUT = 2, TRIG_GLITCH = 4 };
enum { FX_NONE, FX_REPEAT, FX_TAPESTOP, FX_REVERSE };

/* FM voice params */
enum { P_TUNE, P_RATIO, P_INDEX, P_DECAY, P_SWEEP, P_FEEDBK, P_NOISE, P_STUT, P_GLITCH };
/* hat voice params (P_DECAY, P_STUT, P_GLITCH shared with FM) */
enum { H_SAMPLE = 0, H_START = 1, H_LOOP = 2, H_PITCH = 4, H_TONE = 5, H_DRIFT = 6 };

typedef struct {
    uint8_t on, steps, hits, rot;
} Cell;

typedef struct {
    const char *name;
    int min, max;
} ParamDef;

/* Everything the user edits; saved to disk as-is. */
typedef struct {
    uint32_t magic;
    Cell cell[NV][NROW];
    int param[NV][NPARAM];
    int vol[NV];
    uint8_t mute[NV], solo[NV];
    int master;
    float bpm;
    int sweep_rate;
} Song;

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
    int fx_req;   /* FX_NONE / FX_REPEAT / FX_TAPESTOP / FX_REVERSE */
    int fx_crush;
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
