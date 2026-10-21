/**
 * music.c
 *
 * Music and sound playback system: MIDI-like tune playback,
 * ambient sounds, sound effect management, volume control.
 */

#include "music.h"
#include "game.h"
#include "init.h"
#include "platform.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int16_t sound_driver = 0;
bool sound_is_on = false;
bool music_on = false;
bool sound_fx_on = false;
bool subtitles_on = true;

/* Port addition for the "Match voice" subtitle hold: when the audio in
 * flight finishes (see music.h). */
int32_t sample_end_rt = 0;
bool tune_playing = false;
char *sound_storage = NULL;
int32_t top_of_sound_data = 0;
int32_t top_of_texture_data = 0;
char *tune_buffer = NULL;
int32_t tune_buffer_length = 0;
int16_t current_tune = -1;
int32_t tune_position = 0;

int16_t num_ambients = 0;
int ambiant_name[20];
int16_t ambiant_vol[20];
int16_t ambiant_freq[20];
int16_t ambiant_rand[20];
int32_t ambiant_last[20];

int32_t tune_offset[10][96];

/* Tune names per sound index (E1 0x479F70), matched to MUSIC/<NAME>.<ext>
 * and to the re-release's MIDI files. E1 has the first 75; E2WIN95.EXE carries
 * the same list with E2's own 21 after them. */
const char *tune_names[NUM_TUNE_NAMES] = {
    "EEVILEND", "EBKGRD1",  "EBKGRD1A", "EBKGRD1B", "EBKGRD1C",
    "EBKGRD2",  "EBKGRD2A", "EBKGRD2B", "EBKGRD2C", "EBKGRD2A",
    "EBKGRD2B", "EBKGRD3",  "EBKGRD3A", "EBKGRD3B", "EBKGRD3C",
    "EBKGRD3C", "EDEMENT",  "EDEMFLUT", "EGIRLREL", "EGOODEND",
    "EINTRO",   "EKNIGHT",  "EMEDIT",   "STNG51_8", "STNG52_9",
    "STNG03_3", "STNG0410", "STNG0515", "STNG06_3", "STNG0715",
    "STNG0810", "STNG53_4", "STNG10_7", "STNG1110", "STNG54_9",
    "STNG13_2", "STNG14_7", "STNG1512", "STNG16_9", "STNG17_6",
    "STNG18_6", "STNG19_9", "STNG5513", "STNG2117", "STNG22",
    "STNG2310", "STNG24_4", "STNG25_8", "STNG2613", "STNG2711",
    "STNG28_6", "STNG29_5", "STNG30_8", "STNG56_8", "STNG32_6",
    "STNG3316", "STNG3415", "STNG35_3", "STNG36_3", "STNG37_6",
    "STNG38_9", "STNG39_7", "STNG40_7", "STNG41_9", "STNG4219",
    "STNG4316", "STNG4412", "STNG4510", "STNG46_5", "STNG47_8",
    "STNG48_8", "STNG4912", "STNG5015", "EDEMFLU2", "EDEMENTQ",
    "CASTLAMB", "CASTLE_I", "CRYPT_ID", "CRYPTAMB", "GRAVE_ID",
    "GRAVEAMB", "TOWER_I",  "TOWERAMB", "VILL_AMB", "VILL_ID",
    "WILD_AMB", "WILD_ID",  "CASTLE2",  "CASTLE3",  "CASTLE4",
    "GENERIC1", "GENERIC2", "GENERIC3", "GENERIC4", "THEME",
    "OUTRO",
};

static int16_t tune_fade_target;
static int16_t tune_fade_speed;
static int16_t sound_volume = 230;
static int16_t music_volume = 204;
static int32_t find_distance(vector_t *a, vector_t *b);

void load_a_sound(int sound_index);
void stop_speech(void);

/* ── Sound Images MIDI Driver tune → Standard MIDI conversion ──
 * Tunes ship as .SCC/.LAP/.GUS/.AWE/.SBL (Sound Images MIDI Driver, Tony
 * Williams 1992-94); format from ValleyBell's Lem3DMid converter. */

static void smf_write_u32(uint8_t *buf, uint32_t v) {
    buf[0] = v >> 24; buf[1] = v >> 16; buf[2] = v >> 8; buf[3] = v;
}
static void smf_write_u16(uint8_t *buf, uint16_t v) {
    buf[0] = v >> 8; buf[1] = v;
}

/* Variable-length quantity, big-endian 7-bit groups. Returns bytes written. */
static int smf_write_vlq(uint8_t *buf, uint32_t v) {
    uint8_t tmp[5];
    int n = 0;
    tmp[n++] = v & 0x7F;
    while ((v >>= 7) != 0) tmp[n++] = (v & 0x7F) | 0x80;
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

/* SIMD tune → SMF format 1. Format confirmed against Ecstatica, Lemmings 3D
 * and MK2 files.
 *
 *   Header:
 *     [0]      0x01 magic
 *     [1..2]   LE16 pointer to footer-pointer
 *   Footer (at double-indirected offset):
 *     [0]      ticks per quarter note
 *     [1]      tempo in BPM
 *     [2]      track count
 *     [3..]    track-start offsets (LE16 or LE32, matches header width)
 *
 *   Track events: [VLQ delta] [event]
 *     byte < 0x80          note-on: note(1) velocity(1)
 *     0x8n                 select MIDI channel n
 *     0x90 note            note-off (specific note)
 *     0x91                 track end
 *     0x92 prog            program change
 *     0x95 bend            pitch bend (high byte)
 *     0x96 vol             volume (CC 7)
 *     0x97 val             drum select (SBL only)
 *     0x98                 loop back
 *     0x99                 note-off (last played note)
 *     0x9B pan             pan (CC 10)
 *     0x9C                 loop start
 *     0x9D ctrl val        generic controller
 */

static uint8_t *si_to_smf(const uint8_t *src, int src_len, int *out_len) {
    if (src_len < 4 || src[0] != 0x01) return NULL;

    /* 16-bit pointers (E1): [01] [LE16], tracks at 3. 32-bit (E2 Win95):
     * [01 00 00 00] [LE32], tracks at 8. */
    int wide = (src[1] == 0 && src[2] == 0 && src[3] == 0);
    int fp_ptr, footer;
    uint32_t trk_off[64];

    if (wide) {
        if (src_len < 12) return NULL;
        fp_ptr = src[4] | (src[5] << 8) | (src[6] << 16) | (src[7] << 24);
        if (fp_ptr < 0 || fp_ptr + 4 > src_len) return NULL;
        footer = src[fp_ptr] | (src[fp_ptr+1] << 8) |
                 (src[fp_ptr+2] << 16) | (src[fp_ptr+3] << 24);
    } else {
        fp_ptr = src[1] | (src[2] << 8);
        if (fp_ptr + 2 > src_len) return NULL;
        footer = src[fp_ptr] | (src[fp_ptr + 1] << 8);
    }
    if (footer < 0 || footer + 3 > src_len) return NULL;

    uint8_t tpq      = src[footer + 0];
    uint8_t tempo_bpm = src[footer + 1];
    uint8_t trk_count = src[footer + 2];
    if (!tpq) tpq = 24;
    if (!tempo_bpm) tempo_bpm = 120;
    if (!trk_count) return NULL;

    int max_trk = trk_count < 64 ? trk_count : 64;
    if (wide) {
        if (footer + 3 + max_trk * 4 > src_len) return NULL;
        for (int t = 0; t < max_trk; t++)
            trk_off[t] = src[footer+3+t*4] | (src[footer+3+t*4+1] << 8) |
                         (src[footer+3+t*4+2] << 16) | (src[footer+3+t*4+3] << 24);
    } else {
        if (footer + 3 + max_trk * 2 > src_len) return NULL;
        for (int t = 0; t < max_trk; t++)
            trk_off[t] = src[footer+3+t*2] | (src[footer+3+t*2+1] << 8);
    }

    int cap = 64 + src_len * 6 * max_trk;
    /* Zeroed: the buffer is over-allocated and only partly written. */
    uint8_t *out = (uint8_t *)calloc(1, (size_t)cap);
    if (!out) return NULL;

    memcpy(out, "MThd", 4);
    smf_write_u32(out + 4, 6);
    smf_write_u16(out + 8,  1);            /* format 1 */
    smf_write_u16(out + 10, max_trk);
    smf_write_u16(out + 12, tpq);
    int op = 14;

    for (int t = 0; t < max_trk; t++) {
        memcpy(out + op, "MTrk", 4);
        int trk_size_pos = op + 4;
        op += 8;
        int trk_start = op;

        if (t == 0) {
            uint32_t uspq = 60000000 / tempo_bpm;
            out[op++] = 0x00;
            out[op++] = 0xFF; out[op++] = 0x51; out[op++] = 0x03;
            out[op++] = (uspq >> 16) & 0xFF;
            out[op++] = (uspq >> 8)  & 0xFF;
            out[op++] =  uspq        & 0xFF;
        }

        int ip = trk_off[t];
        uint8_t sel_ch = 0;
        uint8_t last_note = 0xFF;
        int trk_end = 0;
        uint32_t pending_delta = 0;

/* Flush accumulated time immediately before an event is written. */
#define EMIT_DELTA() do {                                   \
            op += smf_write_vlq(out + op, pending_delta);    \
            pending_delta = 0;                               \
        } while (0)

        while (!trk_end && ip < src_len && op < cap - 24) {
            /* Events with no MIDI output (channel select, loop markers) carry
             * their time forward to the next real event. */
            uint32_t delta = 0;
            while (ip < src_len && (src[ip] & 0x80)) {
                delta = (delta << 7) | (src[ip++] & 0x7F);
            }
            if (ip >= src_len) break;
            delta = (delta << 7) | src[ip++];
            pending_delta += delta;

            if (ip >= src_len) break;

            if (!(src[ip] & 0x80)) {
                if (ip + 1 >= src_len) break;
                uint8_t note = src[ip];
                uint8_t vel  = src[ip + 1];
                ip += 2;

                EMIT_DELTA();
                /* If same note replays, insert note-off first (SBL compat). */
                if (last_note == note && last_note != 0xFF) {
                    out[op++] = 0x90 | sel_ch;
                    out[op++] = note;
                    out[op++] = 0x00;
                    out[op++] = 0x00; /* zero delta before re-trigger */
                }
                last_note = note;
                out[op++] = 0x90 | sel_ch;
                out[op++] = note;
                out[op++] = vel;
            } else if ((src[ip] & 0xF0) == 0x80) {
                /* Applied to the status byte of later events; emits nothing. */
                sel_ch = src[ip] & 0x0F;
                ip++;
            } else {
                uint8_t cmd = src[ip];
                switch (cmd) {
                case 0x90: /* Note-off (specific note) */
                    if (ip + 1 >= src_len) { trk_end = 1; break; }
                    EMIT_DELTA();
                    out[op++] = 0x90 | sel_ch;
                    out[op++] = src[ip + 1];
                    out[op++] = 0x00;
                    ip += 2;
                    break;
                case 0x91: /* Track end */
                    ip++;
                    trk_end = 1;
                    break;
                case 0x92: /* Program change */
                    if (ip + 1 >= src_len) { trk_end = 1; break; }
                    EMIT_DELTA();
                    out[op++] = 0xC0 | sel_ch;
                    out[op++] = src[ip + 1];
                    ip += 2;
                    break;
                case 0x95: /* Pitch bend (high byte only) */
                    if (ip + 1 >= src_len) { trk_end = 1; break; }
                    EMIT_DELTA();
                    out[op++] = 0xE0 | sel_ch;
                    out[op++] = 0x00;
                    out[op++] = src[ip + 1];
                    ip += 2;
                    break;
                case 0x96: /* Volume (CC 7) */
                    if (ip + 1 >= src_len) { trk_end = 1; break; }
                    EMIT_DELTA();
                    out[op++] = 0xB0 | sel_ch;
                    out[op++] = 0x07;
                    out[op++] = src[ip + 1];
                    ip += 2;
                    break;
                case 0x97: /* Drum select (SBL) */
                    if (ip + 1 >= src_len) { trk_end = 1; break; }
                    EMIT_DELTA();
                    out[op++] = 0xB0 | sel_ch;
                    out[op++] = 0x67;
                    out[op++] = src[ip + 1];
                    ip += 2;
                    break;
                case 0x98: /* Loop back — end track; platform handles looping */
                    trk_end = 1;
                    break;
                case 0x99: /* Note-off (last note) */
                    if (last_note != 0xFF) {
                        EMIT_DELTA();
                        out[op++] = 0x90 | sel_ch;
                        out[op++] = last_note;
                        out[op++] = 0x00;
                        last_note = 0xFF;
                    }
                    ip++;
                    break;
                case 0x9B: /* Pan (CC 10) */
                    if (ip + 1 >= src_len) { trk_end = 1; break; }
                    EMIT_DELTA();
                    out[op++] = 0xB0 | sel_ch;
                    out[op++] = 0x0A;
                    out[op++] = src[ip + 1];
                    ip += 2;
                    break;
                case 0x9C: /* Loop start marker — ignored; platform loops whole file */
                    ip++;
                    break;
                case 0x9D: /* Generic controller */
                    if (ip + 2 >= src_len) { trk_end = 1; break; }
                    EMIT_DELTA();
                    out[op++] = 0xB0 | sel_ch;
                    out[op++] = src[ip + 1];
                    out[op++] = src[ip + 2];
                    ip += 3;
                    break;
                default:
                    DBG_LOG(1, "[TUNE] unknown SI cmd 0x%02x at offset %d\n",
                            cmd, ip);
                    ip++;
                    trk_end = 1;
                    break;
                }
            }
        }

        EMIT_DELTA();
        out[op++] = 0xFF; out[op++] = 0x2F; out[op++] = 0x00;
#undef EMIT_DELTA

        smf_write_u32(out + trk_size_pos, op - trk_start);
    }

    *out_len = op;
    return out;
}

void play_tune(int tune_index) {
    if (!music_on) return;
    if (tune_index < 0) return;
    if (tune_index == current_tune && tune_playing) return;

    current_tune = tune_index;
    load_tune(tune_index);
    start_tune();

    if (!tune_buffer || tune_buffer_length <= 4) return;

    uint8_t *buf = (uint8_t *)tune_buffer;

    /* Standard MIDI file — pass directly to platform player. */
    if (buf[0] == 'M' && buf[1] == 'T' && buf[2] == 'h' && buf[3] == 'd') {
        platform_midi_play(buf, tune_buffer_length, true);
        return;
    }

    /* Sound Images MIDI Driver format — convert to SMF. */
    int smf_len = 0;
    uint8_t *smf = si_to_smf(buf, tune_buffer_length, &smf_len);
    if (smf) {
        platform_midi_play(smf, smf_len, true);
        free(smf);
    } else {
        DBG_LOG(1, "[TUNE] idx=%d si→smf failed (len=%d)\n",
                tune_index, (int)tune_buffer_length);
    }
}

/* music_start_tune  E1: ? | E2P: 0x430490 */
void start_tune(void) {
    if (!tune_buffer) return;
    tune_playing = 1;
    tune_position = 0;
}

/* music_stop_tune  E1: ? | E2P: 0x430560 */
void stop_tune(void) {
    tune_playing = 0;
    tune_position = 0;
    platform_midi_stop();
}

/* music_pause_tune  E1: ? | E2P: 0x430630 */
void pause_tune(void) {
    tune_playing = 0;
}

/* music_resume_tune  E1: ? | E2P: 0x430700 */
void resume_tune(void) {
    if (tune_buffer && current_tune >= 0) {
        tune_playing = 1;
    }
}

/* music_process_tune_4307D0 — ticked the Sound Images sequencer; the platform
 * MIDI player replaces it. */
void process_tune(void) {
    if (!tune_playing || !tune_buffer) return;
}

/* music_fade_tune_4308A0 — target=0 stops (AVMIDIPlayer has no gain ramp). */
void fade_tune(int target_volume, int speed) {
    tune_fade_target = target_volume;
    tune_fade_speed = speed;
    if (target_volume <= 0)
        stop_tune();
}

/* music_play_sound_effect  E1: ? | E2P: 0x430970 */
void play_sound_effect(int sound_index, int volume) {
    if (!sound_fx_on) return;
    if (sound_index < 0 || sound_index >= SOUND_TAB_SIZE) return;

    sound_t *sound = sound_tab[sound_index];
    if (!sound) {
        load_a_sound(sound_index);
        sound = sound_tab[sound_index];
    }
    if (sound) {
        start_playing_sample(sound, 0, volume);
    }
}

/* music_play_ambient  E1: ? | E2P: 0x430A40 */
void play_ambient(int sound_index) {
    if (!sound_fx_on) return;
    play_sound_effect(sound_index, 128);  /* Medium volume for ambients */
}

/* music_stop_all_sounds  E1: ? | E2P: 0x430B10 */
void stop_all_sounds(void) {
    stop_samples();
    stop_tune();
}

/* music_set_sound_volume  E1: ? | E2P: 0x430BE0 */
void set_sound_volume(int volume) {
    sound_volume = volume;
    if (sound_volume < 0) sound_volume = 0;
    if (sound_volume > 255) sound_volume = 255;
    platform_set_sfx_volume(sound_volume);
}

/* music_set_music_volume  E1: ? | E2P: 0x430CB0 */
void set_music_volume(int volume) {
    music_volume = volume;
    if (music_volume < 0) music_volume = 0;
    if (music_volume > 255) music_volume = 255;
    platform_set_music_volume(music_volume);
}

/* music_play_sound_3d  E1: ? | E2P: 0x430D80 */
void play_sound_3d(int sound_index, vector_t *position) {
    if (!sound_fx_on) return;
    if (!position) return;

    int32_t dist = find_distance(&actor_position[0], position);
    int volume = 255;
    if (dist > 0) {
        volume = 255 - (int)(dist / 1000);
        if (volume < 0) volume = 0;
    }

    play_sound_effect(sound_index, volume);
}

/* music_play_actor_sound  E1: ? | E2P: 0x430E50 */
void play_actor_sound(int actor_index, int sound_index) {
    if (actor_index < 0 || actor_index >= ACTOR_POOL_SIZE) return;
    play_sound_3d(sound_index, &actor_position[actor_index]);
}

static int s_speech_voice = -1;

/* music_play_speech  E1: ? | E2P: 0x430F20 */
void play_speech(int speech_index) {
    if (speech_index < 0) return;
    if (!sound_is_on || !sound_fx_on) return;

    stop_speech();

    check_sound_loaded(speech_index);
    sound_t *sound = sound_tab[speech_index];
    if (!sound || !sound->audio_ptr || sound->sound_length <= 0) return;

    int rate = sound->sample_rate > 0 ? sound->sample_rate : 22050;
    s_speech_voice = platform_audio_play_pcm(sound->audio_ptr, sound->sound_length,
                                              rate, 127, 0, false);
}

/* music_stop_speech  E1: ? | E2P: 0x430FF0 */
void stop_speech(void) {
    if (s_speech_voice >= 0) {
        platform_audio_stop_voice(s_speech_voice);
        s_speech_voice = -1;
    }
}

/* Approximate, no sqrt. */
static int32_t find_distance(vector_t *a, vector_t *b) {
    int32_t dx = a->X - b->X;
    int32_t dy = a->Y - b->Y;
    int32_t dz = a->Z - b->Z;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dz < 0) dz = -dz;
    return dx + dy + dz;
}
