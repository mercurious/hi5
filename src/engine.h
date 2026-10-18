/* HI5 audio engine: 5 voices (4 FM percussion + micro-loop hats), Euclidean
 * sequencer, punch-in FX, a tempo-synced master filter and an optional
 * mono + clock-pulse output. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define HI5_VERSION "0.0.7"

#define NV 5
#define NROW 3
#define NPARAM 9
#define MAX_STEPS 16
#define MAX_SAMPLES 24
#define NSWEEP 4
#define NFXSLOT 4 /* R2 + up / down / left / right */
#define NPPQN 7
#define NSLOT 8   /* pattern memory slots A..H */
#define NCHAIN 16 /* song chain entries */
#define NKIT 8    /* kit memory slots 1..8: synth params + mix balance */
#define NHATX 2   /* extra hat params after the shared NPARAM */

enum { V_SUB, V_KICK, V_SNARE, V_PERC, V_HAT };
enum { ROW_HIT, ROW_STUT, ROW_GLITCH };
enum { TRIG_HIT = 1, TRIG_STUT = 2, TRIG_GLITCH = 4 };
/* punch-in effect types; the first three replay the output buffer and are
 * exclusive (latest press wins), the others are inserts that stack */
enum { FXT_REPEAT, FXT_TAPESTOP, FXT_REVERSE, FXT_CRUSH, FXT_GATE, NFXT };
enum { OUT_STEREO, OUT_MONO_CLOCK };
enum { SYNC_OFF, SYNC_AUTO, SYNC_LEAD, SYNC_FOLLOW, NSYNCMODE };

/* FM voice params (the SUB voice has DRIVE in the NOISE slot) */
enum { P_TUNE, P_RATIO, P_INDEX, P_DECAY, P_SWEEP, P_FEEDBK, P_NOISE, P_STUT, P_GLITCH };
#define P_DRIVE P_NOISE
/* hat voice params (P_DECAY, P_STUT, P_GLITCH shared with FM) */
enum { H_SAMPLE = 0, H_START = 1, H_LOOP = 2, H_PITCH = 4, H_TONE = 5, H_DRIFT = 6 };
/* hat extras live in Song.hat_x and are addressed as params NPARAM.. */
enum { HX_RESO, HX_SHINE };

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
    /* v0.0.5 */
    int pan[NV], drive[NV], master_drive;
    /* v0.0.6 */
    int hat_x[NHATX];
    Cell slot_cell[NSLOT][NV][NROW];
    uint8_t slot_mute[NSLOT][NV];
    uint8_t slot_used[NSLOT];
    uint8_t chain_slot[NCHAIN]; /* 0 = end of chain, 1..NSLOT = slot */
    uint8_t chain_bars[NCHAIN];
    int song_mode;
    /* v0.0.7 */
    int sync_mode;
    /* v0.0.8 */
    int kit_param[NKIT][NV][NPARAM];
    int kit_hat_x[NKIT][NHATX];
    int kit_vol[NKIT][NV], kit_pan[NKIT][NV], kit_drive[NKIT][NV], kit_master_drive[NKIT];
    uint8_t kit_used[NKIT];
} Song;

#define SONG_V1_SIZE offsetof(Song, fx)
#define SONG_V3_SIZE offsetof(Song, pan)
#define SONG_V4_SIZE offsetof(Song, hat_x)
#define SONG_V5_SIZE offsetof(Song, sync_mode)
#define SONG_V6_SIZE offsetof(Song, kit_param)

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
    volatile int cur_slot;      /* slot the pattern was last loaded from / saved to, or -1 */
    volatile int cur_kit;       /* same for kits */
    volatile int chain_pos, chain_bar;
} Live;

extern Song song;
extern Live live;
extern Sample samples[MAX_SAMPLES];
extern int nsamples;

extern const char *const voice_names[NV];

const ParamDef *param_def(int v, int p);
int param_max(int v, int p);
int nparam(int v);           /* NPARAM, or NPARAM + NHATX for the hat */
int *param_ptr(int v, int p);
void param_format(int v, int p, char *buf, int n);

/* pattern slots and the song chain (UI calls these under the audio lock) */
void slot_save(int i);
void slot_load(int i);
void slot_clear(int i);
void kit_save(int i);
void kit_load(int i);
void kit_clear(int i);
int chain_len(void);
void chain_play_from(int pos);
void song_mode_set(int on);

void engine_init(int sample_rate);
void engine_render(int16_t *out, int frames);
void engine_audition(int v);
void engine_set_playing(int on);
/* transport position in 16th steps with fraction, and the sync hooks:
 * a hard jump, and a playback speed trim (1.0 = none); all under the lock */
double engine_position(void);
void engine_set_position(double steps);
void engine_set_rate(double r);

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
