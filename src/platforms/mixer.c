/**
 * platforms/mixer.c
 *
 * 16 PCM voices point-resampled and summed as on the PSP and Vita, plus the
 * game's tunes played through TinySoundFont from a user-supplied General MIDI
 * soundfont. See mixer.h.
 */

#include "mixer.h"
#include "../platform.h"
#include "../types.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define TSF_IMPLEMENTATION
#define TML_IMPLEMENTATION
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#ifdef __clang__
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wnull-pointer-subtraction"
#endif
#include "third_party/tsf.h"
#include "third_party/tml.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#define MIXER_VOICES   16
/* Music is rendered in slices this long, so a MIDI event lands within 1.5 ms
 * of its time at 44.1 kHz. */
#define MUSIC_SLICE    64
/* The game's tunes use a handful of channels; 64 voices is ample and keeps
 * TinySoundFont from allocating on the audio thread. */
#define MUSIC_POLYPHONY 64

typedef struct {
    const uint8_t *data;
    uint32_t       len;
    uint32_t       idx;      /* whole sample index into data */
    uint32_t       frac;     /* fractional position, 0..0xFFFF */
    uint32_t       step;     /* 16.16 increment: rate / out rate */
    int            vol;      /* 0..127 */
    int            pan;      /* -128..127 */
    bool           loop;
    bool           active;
} mixer_voice_t;

static mixer_voice_t s_voices[MIXER_VOICES];
static platform_sem_t *s_lock;
static int  s_rate = 44100;
static int  s_sfx_vol = 255;
static int  s_music_vol = 255;
static bool s_ready;

static char s_sf_dir[512];
static bool s_sf_tried;
static tsf *s_synth;
static tml_message *s_tune;       /* head of the playing tune, owned */
static tml_message *s_tune_next;  /* next event to dispatch */
static double s_tune_ms;
static bool s_tune_loop;

static void lock(void)   { if (s_lock) platform_sem_wait(s_lock); }
static void unlock(void) { if (s_lock) platform_sem_post(s_lock); }

/* ── Soundfont ─────────────────────────────────────────────── */

static bool readable(const char *p) {
    return p && p[0] && access(p, R_OK) == 0;
}

/* Preferred banks first, then any .sf2; the same names Linux looks for.
 * TinySoundFont reads SF2 only, not the Vorbis-packed SF3. */
static bool scan_dir(const char *dir, char *out, size_t outlen) {
    static const char *const preferred[] = {
        "default.sf2", "FluidR3_GM.sf2", "GeneralUser.sf2",
        "GeneralUser_GS.sf2", "TimGM6mb.sf2", NULL
    };
    for (int i = 0; preferred[i]; i++) {
        snprintf(out, outlen, "%s/%s", dir, preferred[i]);
        if (readable(out)) return true;
    }

    DIR *d = opendir(dir);
    if (!d) return false;
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcasecmp(e->d_name + n - 4, ".sf2") != 0) continue;
        snprintf(out, outlen, "%s/%s", dir, e->d_name);
        found = readable(out);
    }
    closedir(d);
    return found;
}

static tsf *load_soundfont(void) {
    char path[1024];
    const char *env = getenv("ECSTATICA_SOUNDFONT");
    bool found = false;

    if (readable(env)) {
        snprintf(path, sizeof(path), "%s", env);
        found = true;
    }
    if (!found) found = scan_dir(".", path, sizeof(path));
    /* The re-release bundles a GM bank with its data. */
    if (!found) found = scan_dir("Audio/Music/Instruments", path, sizeof(path));
    if (!found && s_sf_dir[0]) found = scan_dir(s_sf_dir, path, sizeof(path));
    if (!found) {
        DBG_LOG(1, "[MIDI] no .sf2 soundfont found, music is silent\n");
        return NULL;
    }

    tsf *f = tsf_load_filename(path);
    if (!f) {
        DBG_LOG(1, "[MIDI] could not load soundfont %s\n", path);
        return NULL;
    }
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, s_rate, 0.0f);
    tsf_set_max_voices(f, MUSIC_POLYPHONY);
    tsf_channel_set_bank_preset(f, 9, 128, 0);   /* GM drums */
    DBG_LOG(1, "[MIDI] soundfont %s\n", path);
    return f;
}

/* ── Sequencer ─────────────────────────────────────────────── */

static void dispatch(tsf *f, const tml_message *m) {
    switch (m->type) {
    case TML_PROGRAM_CHANGE:
        tsf_channel_set_presetnumber(f, m->channel, m->program, m->channel == 9);
        break;
    case TML_NOTE_ON:
        tsf_channel_note_on(f, m->channel, m->key, m->velocity / 127.0f);
        break;
    case TML_NOTE_OFF:
        tsf_channel_note_off(f, m->channel, m->key);
        break;
    case TML_PITCH_BEND:
        tsf_channel_set_pitchwheel(f, m->channel, m->pitch_bend);
        break;
    case TML_CONTROL_CHANGE:
        tsf_channel_midi_control(f, m->channel, m->control, m->control_value);
        break;
    default:
        break;
    }
}

static void render_music(int *accum, int frames) {
    static float slice[MUSIC_SLICE * 2];
    int gain = s_music_vol;

    for (int done = 0; done < frames; ) {
        int n = frames - done;
        if (n > MUSIC_SLICE) n = MUSIC_SLICE;

        s_tune_ms += n * 1000.0 / s_rate;
        while (s_tune_next && s_tune_next->time <= s_tune_ms) {
            dispatch(s_synth, s_tune_next);
            s_tune_next = s_tune_next->next;
        }
        if (!s_tune_next && s_tune_loop && s_tune) {
            tsf_note_off_all(s_synth);
            s_tune_next = s_tune;
            s_tune_ms = 0.0;
        }

        tsf_render_float(s_synth, slice, n, 0);
        for (int i = 0; i < n * 2; i++)
            accum[(done * 2) + i] += (int)(slice[i] * 32767.0f) * gain / 255;
        done += n;
    }
}

/* ── Mixing ────────────────────────────────────────────────── */

void mixer_init(int out_rate, const char *soundfont_dir) {
    if (s_ready) return;
    s_rate = out_rate > 0 ? out_rate : 44100;
    snprintf(s_sf_dir, sizeof(s_sf_dir), "%s", soundfont_dir ? soundfont_dir : "");
    memset(s_voices, 0, sizeof(s_voices));
    s_lock = platform_sem_create(1);
    s_ready = true;
}

void mixer_render(int16_t *out, int frames) {
    enum { CHUNK = 1024 };
    static int accum[CHUNK * 2];

    while (frames > 0) {
        int n = frames > CHUNK ? CHUNK : frames;
        memset(accum, 0, sizeof(int) * (size_t)n * 2);

        lock();
        for (int v = 0; v < MIXER_VOICES; v++) {
            mixer_voice_t *vo = &s_voices[v];
            if (!vo->active) continue;

            const uint8_t *data = vo->data;
            uint32_t idx = vo->idx, frac = vo->frac, step = vo->step, len = vo->len;
            int scale = (vo->vol * s_sfx_vol * 256) / (127 * 255);
            /* Attenuate the far side only, so centred sounds are not quieter. */
            int scale_l = vo->pan > 0 ? scale * (127 - vo->pan) / 127 : scale;
            int scale_r = vo->pan < 0 ? scale * (128 + vo->pan) / 128 : scale;

            for (int i = 0; i < n; i++) {
                if (idx >= len) {
                    if (!vo->loop) { vo->active = false; break; }
                    idx = 0;
                    frac = 0;
                }
                int s = ((int)data[idx] - 128) << 8;
                accum[i * 2 + 0] += (s * scale_l) >> 8;
                accum[i * 2 + 1] += (s * scale_r) >> 8;
                /* Index and fraction apart: a single 16.16 position caps the
                 * sample at 64 KB, and speech lines are longer. */
                frac += step;
                idx += frac >> 16;
                frac &= 0xFFFFu;
            }
            vo->idx = idx;
            vo->frac = frac;
        }
        if (s_synth && s_tune)
            render_music(accum, n);
        unlock();

        for (int i = 0; i < n * 2; i++) {
            int s = accum[i];
            if (s < -32768) s = -32768;
            if (s >  32767) s =  32767;
            out[i] = (int16_t)s;
        }
        out += n * 2;
        frames -= n;
    }
}

void mixer_shutdown(void) {
    if (!s_ready) return;
    lock();
    memset(s_voices, 0, sizeof(s_voices));
    if (s_tune) { tml_free(s_tune); s_tune = s_tune_next = NULL; }
    if (s_synth) { tsf_close(s_synth); s_synth = NULL; }
    s_sf_tried = false;
    unlock();
    s_ready = false;
}

/* ── platform.h ────────────────────────────────────────────── */

int platform_audio_play_pcm(const void *data, int length, int rate,
                            int volume, int pan, bool loop) {
    if (!s_ready || !data || length <= 0) return -1;
    if (rate <= 0) rate = 22050;

    lock();
    int slot = -1;
    for (int v = 0; v < MIXER_VOICES; v++)
        if (!s_voices[v].active) { slot = v; break; }
    if (slot < 0) slot = 0;   /* steal slot 0, as documented */

    mixer_voice_t *vo = &s_voices[slot];
    vo->data = (const uint8_t *)data;
    vo->len = (uint32_t)length;
    vo->idx = 0;
    vo->frac = 0;
    vo->step = (uint32_t)(((uint64_t)rate << 16) / (uint64_t)s_rate);
    vo->vol = volume < 0 ? 0 : (volume > 127 ? 127 : volume);
    vo->pan = pan < -128 ? -128 : (pan > 127 ? 127 : pan);
    vo->loop = loop;
    vo->active = true;
    unlock();
    return slot;
}

void platform_audio_stop_voice(int slot) {
    if (slot < 0 || slot >= MIXER_VOICES) return;
    lock();
    s_voices[slot].active = false;
    unlock();
}

void platform_audio_stop_all(void) {
    lock();
    for (int v = 0; v < MIXER_VOICES; v++)
        s_voices[v].active = false;
    unlock();
}

int platform_midi_play(const void *smf_data, int length, bool loop) {
    if (!s_ready || !smf_data || length <= 0) return -1;

    /* Loaded on the first tune, after the platform has found the data
     * folder; a missing soundfont is looked for once. */
    if (!s_sf_tried) {
        s_sf_tried = true;
        tsf *f = load_soundfont();
        lock();
        s_synth = f;
        unlock();
    }
    if (!s_synth) return -1;

    tml_message *tune = tml_load_memory(smf_data, length);
    if (!tune) return -1;

    lock();
    tml_message *old = s_tune;
    tsf_note_off_all(s_synth);
    s_tune = tune;
    s_tune_next = tune;
    s_tune_ms = 0.0;
    s_tune_loop = loop;
    unlock();

    if (old) tml_free(old);
    return 0;
}

void platform_midi_stop(void) {
    lock();
    tml_message *old = s_tune;
    s_tune = s_tune_next = NULL;
    if (s_synth) tsf_note_off_all(s_synth);
    unlock();
    if (old) tml_free(old);
}

void platform_set_sfx_volume(int vol) {
    s_sfx_vol = vol < 0 ? 0 : (vol > 255 ? 255 : vol);
}

void platform_set_music_volume(int vol) {
    s_music_vol = vol < 0 ? 0 : (vol > 255 ? 255 : vol);
}
