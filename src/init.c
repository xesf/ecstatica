/**
 * init.c
 *
 * Initialization, display primitives, input handling, timing.
 */

#include "init.h"
#include "asm_f.h"
#include "display.h"
#include "edit.h"
#include "ellipse.h"
#include "file.h"
#include "game.h"
#include "icon.h"
#include "map.h"
#include "menu.h"
#include "display.h"
#include "file.h"
#include "music.h"
#include "anim.h"
#include "game.h"
#include "edit.h"
#include "prefetch.h"
#include "render.h"
#include "req.h"
#include "topo.h"
#include "tools/pointcloud.h"
#include "tools/viewer.h"
#include "win.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
#include <math.h>

char keys_pressed[256];
char keys_were_pressed[256];
char extra_keys_pressed[256];
char extra_keys_were_pressed[256];
char keys_were_pressed_codes[256];
int16_t mouse = 0;
int16_t mouse_x = 0;
int16_t mouse_y = 0;
bool space_pressed = false;
bool space_was_pressed = false;
bool enter_was_pressed = false;
bool enter_pressed = false;
bool ctrl_pressed = false;
bool alt_pressed = false;
bool key1_pressed = false, key2_pressed = false, key3_pressed = false, key4_pressed = false;
bool key5_pressed = false, key6_pressed = false, key7_pressed = false, key8_pressed = false, key9_pressed = false;
bool key_esc_was_pressed = false;
bool key_i_was_pressed = false;
bool key_return_was_pressed = false;
int16_t joystick = 0;
bool joystick_control = true;
int16_t joy_button = 0;
int16_t movement_speed_mode = 4;  /* default: walk speed 1 (F5) */

bool mouse_pointer_on = false;

int16_t sine_table[65536];
int16_t cosn_table[65536];
int16_t arcsin_tab[32768];
int8_t  atan_tab0[256];
int8_t  atan_tab1[256];
char shade_tab[17][128][128];
char shade_texture[8][128][128];
char shadow_tab[3][16][256];
char shade_map[128][128];
int16_t profile[128][128];

int16_t material_flags[30];

int16_t event_type_flags[80];
int16_t event_priority[80];

palette_entry_t view_cmap[256];
palette_entry_t colour_map[256];
palette_entry_t all_black_cmap[256];
palette_entry_t spare_cmap[256];
palette_entry_t fade_cmap[256];
palette_entry_t edit_map_cmap[256];

/* init_setup  E1: 0x41007C | E2: 0x41007C */
void setup(void) {
    DBG_LOG(1, "SETUP: entering\n");
    free_all_heaps();
    DBG_LOG(1, "SETUP: after free_all_heaps\n");
    initialise_parts();
    DBG_LOG(1, "SETUP: after initialise_parts\n");
    init();
    DBG_LOG(1, "SETUP: after init\n");

    FILE *f = fopen_ci("e_config", "rb");
    if (f) {
        config_t config;
        fread_ignore(&config, sizeof(config_t), 1, f);
        fclose(f);

        if (memcmp(config.name, "Ecstatica001", 12) == 0) {
            sound_driver = config.sound_driver;
            female = config.femaleF;
            language = config.language;
        }
    }

    /* do_init() ran before the stored preference was loaded. */
    platform_set_scale_mode(win_platform(), display_scale_mode);

    /* Here, not in do_init(): the renderer preference is only known once
     * load_port_settings() has run. A failure leaves the software renderer. */
    render_init();
    DBG_LOG(1, "SETUP: after render_init\n");

    init_gadgets();
    setup_directory_paths();
    setup_long_screen();
    height_shift = 7;

    /* The mixer comes up here, ahead of the archive merges, so it is ready
     * the first time a scene plays a sound. */
    set_up_sound_driver();
    DBG_LOG(1, "SETUP: after set_up_sound_driver\n");
    platform_audio_init();
    DBG_LOG(1, "SETUP: after platform_audio_init\n");
    if (!sound_driver) sound_driver = 1;
    sound_is_on = true;
    sound_fx_on = true;
    music_on = true;
    subtitles_on = true;
    top_of_sound_data = 0;

    /* Load main data — pump events between heavy steps so the title
     * screen stays visible and the window doesn't appear frozen. */
    present_delay(0);
    DBG_LOG(1, "SETUP: before merge CODE\\ECSTATIC.FAN\n");
    merge_a_file_no_message("CODE\\ECSTATIC.FAN", 0);
    DBG_LOG(1, "SETUP: after merge CODE\\ECSTATIC.FAN\n");
    present_delay(0);

    FILE *offsets_check = fopen_ci("OFFSETS", "rb");
    if (!offsets_check) offsets_check = fopen_ci("../OFFSETS", "rb");
    if (offsets_check) {
        fclose(offsets_check);
        set_load_by_offset();
        open_read_file("files\\ECSTATIC");
        if (game_version == GAME_VERSION_E2)
            open_read_file2("files\\ECST2");
        present_delay(0);
        read_offsets_file();
        present_delay(0);
    }

    /* --viewer: same archives, same render pipeline, no world. Takes over
     * here because everything it browses is loaded by the merges above. */
    if (viewer_mode == VIEWER_POINTCLOUD) {
        pointcloud_main();
        return;
    }
    if (viewer_mode) {
        viewer_main();
        return;
    }

    /* setup_41007C clears bitmap[0] to 0 here, not bitmap[2], which holds the
     * background check_view loaded. */
    start_game_medium(0, 0);
    a_pen_colour = 0;
    rect_fill(0, 0, 0, screen_width, screen_height);
    make_thing();
    quit("");
}

static int16_t peek_fan_version(void) {
    FILE *f = fopen_ci("CODE/ECSTATIC.FAN", "rb");
    if (!f) return -1;

    uint8_t magic[4];
    if (fread(magic, 1, 4, f) != 4) { fclose(f); return -1; }
    if (magic[0] != 'F' || magic[1] != 'A' || magic[2] != 'N' || magic[3] != 'T') {
        fclose(f);
        return -1;
    }
    int16_t ver = getw_be(f);
    fclose(f);
    return ver;
}

/* init_init  E1: 0x410A4C | E2: 0x410A48 */
void init(void) {
    make_code_writable();

    detect_game_version();
    init_data_roots();

    /* Before the boot resolution is picked below: the aspect ratio sets its width. */
    load_port_settings();

    /* E1 data predating the Win95 release ships only 320x200 backgrounds, so
     * it boots in VGA. A nested W/ folder (or a HIRES set in the launch dir)
     * supplies the hi-res assets the graphics toggle switches to. */
    int16_t fan_ver = peek_fan_version();
    int vga_data = (game_version == GAME_VERSION_E1 && fan_ver >= 0 && fan_ver <= 30);

    /* Fixed for the session: it describes the loaded database, which the
     * graphics toggle never changes. Drives the SFX sample rate. */
    e1_dos_data = vga_data;

    /* Hi-res needs both halves: the assets, and a display mode to put them in.
     * On DOS the second one is not a given — a card with no VESA 2.0 linear
     * framebuffer has nothing above mode 13h — so the data check alone would
     * leave the engine rendering 640x480 into a 320x200 screen. */
    int display_hires = platform_hires_supported(win_platform());
    hires_available = hires_data_available() && display_hires;
    low_res_only = (vga_data || !display_hires) && !hires_available;

    if (!display_hires)
        DBG_LOG(1, "[INIT] display has no 640x480 8-bit mode; staying in VGA\n");

    /* Enhanced whenever the hi-res set is there: E2, Win95 E1, and DOS E1
     * with a W/ folder beside it. Only bare DOS E1 data boots in VGA, unless
     * the platform asks for it.
     *
     * E2 has a complete 320x200 mode: VIEWS/ is a genuine low-res set, the
     * HUD switches to LOWGRAPH/'s L-prefixed art (llife*, larmour*, lmbar...)
     * when mode_svga is clear, and show_icon_page() goes to SVGA for the
     * inventory page, which has no low-res twin. That last part is why a
     * platform preference must not set low_res_only. */
    int boot_vga = !display_hires || (vga_data && !hires_available) ||
                   platform_boot_low_res(win_platform());

    /* Must agree with the boot resolution before the first asset load, or the
     * swappable lookups prefer the wrong root. */
    enhanced_graphics = boot_vga ? 0 : 1;

    if (boot_vga) {
        chosen_svga = 0;
        set_vga_constants();
    } else {
        set_svga_constants();
    }

    set_up_bitmaps();
    flush_backgrounds();
    go_vga();
    if (!boot_vga)
        go_svga();

    /* Skip the publisher logos and title in the viewer. Skipping the waits is
     * not enough: every present_delay() in setup() would flip the title back
     * up during the load, so the planes are cleared instead. */
    if (game_version != GAME_VERSION_E1 && !viewer_mode) {
        load_logo("psyglogo.raw");
        present_delay(2000);
        load_logo("aasglogo.raw");
        present_delay(2000);
    }

    load_def_palette();
    if (viewer_mode) {
        int16_t save_pen = a_pen_colour;
        a_pen_colour = 0;
        rect_fill(0, 0, 0, screen_width, screen_height);
        rect_fill(1, 0, 0, screen_width, screen_height);
        rect_fill(3, 0, 0, screen_width, screen_height);
        a_pen_colour = save_pen;
    } else {
        load_background_title();
        present_delay(2000);
    }
    load_anti_alias();
    init_gadgets();
    init_event_type_flags();
    /* Bit 4 makes a material impassable in find_height_now_material. Left
     * zeroed, the player walks through the scenery that fences off camera-0
     * ground, and check_camera then plays scene 7 with its first actor as
     * the new player. */
    init_material_flags();

    memset(all_black_cmap, 0, sizeof(all_black_cmap));

    part_names = (name_text_t *)calloc(PART_POOL_SIZE, sizeof(name_text_t));
    thing_names = (name_text_t *)calloc(THING_TAB_SIZE, sizeof(name_text_t));
    action_names = (name_text_t *)calloc(ACTION_TAB_SIZE, sizeof(name_text_t));
    scene_names = (name_text_t *)calloc(SCENE_TAB_SIZE, sizeof(name_text_t));
    point_names = (name_text_t *)calloc(POINT_POOL_SIZE, sizeof(name_text_t));
    triangle_names = (name_text_t *)calloc(TRI_SIZE, sizeof(name_text_t));
    code_names = (name_text_t *)calloc(CODE_TAB_SIZE, sizeof(name_text_t));
    repertoire_names = (name_text_t *)calloc(REPERTOIRE_TAB_SIZE, sizeof(name_text_t));
    sound_names = (name_text_t *)calloc(SOUND_TAB_SIZE, sizeof(name_text_t));
    map_area_names = (name_text_t *)calloc(MAP_AREA_TAB_SIZE, sizeof(name_text_t));
    texture_names = (name_text_t *)calloc(TEXTURE_TAB_SIZE, sizeof(name_text_t));

    /* Populate default part names (must match FAN file names exactly) */
    static const char *default_part_names[] = {
        "Body", "Chest", "Head", "Left upper arm", "Right upper arm",
        "Left forearm", "Right forearm", "Left hand", "h_hold",
        "Left thigh", "Right thigh", "Left shin", "Right shin",
        "Left foot", "Right foot", "Left heel", "Right heel",
        "Left ear", "Right ear", "Left calf", "Right calf",
        "L finger tips", "R finger tips", "L index finger",
        "R index finger", "L index tip", "R index tip"
    };
    for (int i = 0; i < 27 && i < PART_POOL_SIZE; i++) {
        strncpy(part_names[i].field_0, default_part_names[i], 25);
    }

    token_store = (int16_t *)calloc(20000, sizeof(int16_t));
    top_of_tokens = 1;

    fill_in_sin_tables();
    load_shadow_tab();
    init_map();
    init_profile_heights();
    init_graphics();
    clear_keys_pressed();
    load_shade_map();

    extern void load_character_set_ref(void);
    load_character_set_ref();

    game_time = 0;
}

/* init_setup_directory_paths_413DA4 — the original picked CD or disk path
 * prefixes; all data here is relative to the data root. */
void setup_directory_paths(void) {
}

/* init_stop_samples_413E88 — stop all playing sound buffers */
void stop_samples(void) {
    platform_audio_stop_all();
}

/* init_start_playing_sample  E1: 0x41104C | E2: 0x413E78 */
void start_playing_sample(sound_t *sound, int loop, int volume) {
    if (!sound) return;
    if (sound->audio_ptr && sound->sound_length > 0) {
        sound->_time = game_time;

        /* Track when the audio in flight will finish, so a subtitle can be
         * held for as long as its speech lasts. A running maximum: footsteps
         * over a long spoken sample must not pull it back. Recorded even with
         * SFX muted so the hold still scales with the line. */
        int rate = sound->sample_rate > 0 ? sound->sample_rate : 22050;
        int32_t now_rt = my_time();
        int32_t dur = (int32_t)(((int64_t)sound->sound_length * MY_TIME_PER_SEC) / rate);
        if (now_rt + dur > sample_end_rt)
            sample_end_rt = now_rt + dur;
        DBG_LOG(2, "[SUBT] sample len=%d rate=%d -> %.2fs, rt=%d audio ends rt=%d (game_time %d)\n",
                (int)sound->sound_length, rate,
                (double)dur / MY_TIME_PER_SEC,
                (int)now_rt, (int)sample_end_rt, (int)game_time);

        if (sound_driver) {
            play_sound_win95(sound, volume);
        }
    }
}

/* init_set_up_sound_driver  E1: 0x411460 | E2: 0x41428C */
void set_up_sound_driver(void) {
    if (!tune_buffer) {
        tune_buffer = (char *)calloc(50000, 1);
    }
}

/* init_load_tune  E1: 0x41148C | E2: 0x4142B8 */
void load_tune(int sound_idx) {
    tune_buffer_length = 0;
    if (!tune_buffer) return;
    if (sound_driver < 1 || sound_idx < 1) return;

    /* The re-release ships every tune as a standard MIDI file, which
     * play_tune hands to the platform synth unconverted. */
    if (remaster_layout) {
        if (sound_idx > NUM_TUNE_NAMES) return;
        char path[128];
        snprintf(path, sizeof(path), "Audio/Music/Tunes/MIDI/%s.mid",
                 tune_names[sound_idx - 1]);
        FILE *f = fopen_ci(path, "rb");
        if (!f) {
            DBG_LOG(1, "[TUNE] missing %s\n", path);
            return;
        }
        size_t n = fread(tune_buffer, 1, 48000, f);
        if (n > 0 && n < 48000)
            tune_buffer_length = (int32_t)n;
        fclose(f);
        DBG_LOG(2, "[TUNE] %d %s, %d bytes\n", sound_idx, path, (int)tune_buffer_length);
        return;
    }

    if (load_by_offset) {
        /* Monolithic archive path — indexed by driver/tune.
         * Original binary uses 1-based sound_idx directly into the
         * tune_offset array (entry [0] is unused padding per driver row). */
        int max_tune = (game_version == GAME_VERSION_E1) ? 74 : 95;
        if (file_pointer && sound_driver <= 10 && sound_idx <= max_tune) {
            long offset = tune_offset[sound_driver - 1][sound_idx];
            if (offset > 0) {
                fseek(file_pointer, offset, SEEK_SET);
                int32_t length = getl(file_pointer);
                if (length > 0 && length <= 48000) {
                    if (fread(tune_buffer, 1, length, file_pointer) == (size_t)length)
                        tune_buffer_length = length;
                }
            }
        }
        /* Fall back to per-file if offset-based loading produced nothing. */
        if (tune_buffer_length <= 0 && game_version == GAME_VERSION_E1)
            goto try_per_file;
    } else {
try_per_file:
        /* E1 per-file path — MUSIC/<NAME>.<ext>. Try all drivers. */
        if (sound_idx > 75) return;
        static const char *exts[] = { "scc", "sbl", "gus", "awe", "lap" };
        int drv_start = (sound_driver >= 1 && sound_driver <= 5) ? sound_driver - 1 : 0;
        for (int d = 0; d < 5 && tune_buffer_length <= 0; d++) {
            int di = (drv_start + d) % 5;
            char path[128];
            snprintf(path, sizeof(path), "MUSIC/%s.%s",
                     tune_names[sound_idx - 1], exts[di]);
            FILE *f = fopen_ci(path, "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long length = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (length > 0 && length <= 48000) {
                if (fread(tune_buffer, 1, length, f) == (size_t)length)
                    tune_buffer_length = (int32_t)length;
            }
            fclose(f);
        }
    }
}

/* init_set_up_bitmaps  E1: 0x411990 | E2: 0x4147B0 */
void set_up_bitmaps(void) {
    /* Size the planes to the largest mode this session can reach. go_svga()
     * is the only widener and bails out when low_res_only, so a low-res-only
     * session saves 2.44 MB. */
#ifdef ECS_WIDESCREEN
    int32_t plane_pixels = (low_res_only ? (320 * 200) : (WIDE_SCREEN_MAX_WIDTH * 480))
                         + BITMAP_SLACK;
#else
    int32_t plane_pixels = (low_res_only ? (320 * 200) : (640 * 480))
                         + BITMAP_SLACK;
#endif

    bitmap[0] = (char *)calloc(plane_pixels, 1);
    bitmap[1] = (char *)calloc(plane_pixels, 1);
    bitmap[2] = (char *)calloc(plane_pixels, 1);
    bitmap[3] = (char *)calloc(plane_pixels, 1);
    bitmap[4] = NULL;
    bitmap[5] = NULL;

    mask_map[0] = (int16_t *)calloc(plane_pixels, 2);
    mask_map[1] = (int16_t *)calloc(plane_pixels, 2);
    mask_map[2] = (int16_t *)calloc(plane_pixels, 2);

    if (!bitmap[0] || !bitmap[1] || !bitmap[2] || !bitmap[3] ||
        !mask_map[0] || !mask_map[1] || !mask_map[2])
        quit("not enough memory for screen planes");
}

/* init_setup_long_screen  E1: 0x411A60 | E2: 0x414880 */
void setup_long_screen(void) {
    hires_bitmap[3] = bitmap[3];
    hires_bitmap[4] = bitmap[4];
    hires_bitmap[5] = bitmap[5];
}

/* init_load_logo  E1: 0x411B4C | E2: 0x41496C */
void load_logo(const char *file_name) {
    FILE *f = fopen_ci(file_name, "rb");
    if (!f) return;

    char header[32];
    fread_ignore(header, 1, 32, f);

    uint8_t pal[768];
    fread_ignore(pal, 1, 768, f);
    for (int i = 0; i < 256; i++) {
        spare_cmap[i].R = pal[i * 3 + 0] >> 2;
        spare_cmap[i].G = pal[i * 3 + 1] >> 2;
        spare_cmap[i].B = pal[i * 3 + 2] >> 2;
    }

    read_screen_image(f, bitmap[3]);
    fclose(f);

    clip_blit(3, 0, 0, 0, 0, 0, screen_width, screen_height, 0xC0);
    clip_blit(3, 0, 0, 1, 0, 0, screen_width, screen_height, 0xC0);
    set_palette(spare_cmap);
    /* Update fade target so any subsequent CT_FADE_IN fades to this palette */
    memcpy(fade_cmap, spare_cmap, 256 * sizeof(palette_entry_t));
}

/* init_load_def_palette  E1: 0x411C48 | E2: 0x414A68 */
void load_def_palette(void) {
    FILE *f = fopen_ci("PALLETTE.RAW", "rb");
    if (!f) f = fopen_ci("TITLE_S.RAW", "rb");
    if (!f) return;

    char header[32];
    fread_ignore(header, 1, 32, f);

    uint8_t pal[768];
    fread_ignore(pal, 1, 768, f);
    fclose(f);

    for (int i = 0; i < 256; i++) {
        colour_map[i].R = pal[i * 3 + 0] >> 2;
        colour_map[i].G = pal[i * 3 + 1] >> 2;
        colour_map[i].B = pal[i * 3 + 2] >> 2;
    }

    /* Red reads as pink in E1; darken the red channel of the red entries. */
    if (game_version == GAME_VERSION_E1) {
        for (int i = 207; i <= 223; i++) {
            colour_map[i].G = colour_map[i].G / 2;
            colour_map[i].B = colour_map[i].B / 2;
        }
    }

    /* Initialize view_cmap and fade_cmap from colour_map so rendering and
     * fade effects use the correct base palette. */
    memcpy(view_cmap, colour_map, sizeof(view_cmap));
    memcpy(fade_cmap, colour_map, sizeof(fade_cmap));
}

/* init_load_background  E1: 0x411CD8 | E2: 0x414AF8 */
void load_background_title(void) {
    FILE *f = NULL;
    if (screen_width <= 320)
        f = fopen_ci("TITLE_S.RAW", "rb");
    if (!f) f = fopen_ci("TSCREEN.RAW", "rb");
    if (!f) f = fopen_ci("TITLE_S.RAW", "rb");
    if (!f) return;

    char header[32];
    fread_ignore(header, 1, 32, f);

    uint8_t pal[768];
    fread_ignore(pal, 1, 768, f);
    for (int i = 0; i < 256; i++) {
        spare_cmap[i].R = pal[i * 3 + 0] >> 2;
        spare_cmap[i].G = pal[i * 3 + 1] >> 2;
        spare_cmap[i].B = pal[i * 3 + 2] >> 2;
    }

    read_screen_image(f, bitmap[3]);
    fclose(f);

    clip_blit(3, 0, 0, 0, 0, 0, screen_width, screen_height, 0xC0);
    clip_blit(3, 0, 0, 1, 0, 0, screen_width, screen_height, 0xC0);
    set_palette(spare_cmap);
    /* Update fade target so any subsequent CT_FADE_IN fades to this palette */
    memcpy(fade_cmap, spare_cmap, 256 * sizeof(palette_entry_t));
}

/* init_quit2  E1: 0x412028 | E2: 0x414E30 */
void quit2(const char *msg1, const char *msg2) {
    printf("%s\n%s\n", msg1, msg2);
    quit("");
}

/* init_quit  E1: 0x412034 | E2: 0x414E3C */
void quit(const char *info) {
    /* stdout goes nowhere on the handhelds; the log is the only record. */
    DBG_LOG(1, "[QUIT] %s\n", info && info[0] ? info : "(no message)");
    /* Every thread that touches the heap stops before exit() frees it: the
     * prefetch reader allocates, and the mixer reads sample buffers. */
    prefetch_shutdown();
    platform_audio_shutdown();

    sound_t *s = sound_list;
    while (s) {
        release_sound_buffer_win95(s);
        s = s->next;
    }
    remove_sound_driver_win95();

    if (file_pointer) { fclose(file_pointer); file_pointer = NULL; }
    if (file2_pointer) { fclose(file2_pointer); file2_pointer = NULL; }

    if (info && info[0]) {
        printf("%s\n", info);
    }

    dbg_log_flush();
    exit(0);
}

/* init_fill_in_sin_tables  E1: 0x41210C | E2: 0x414F14 */
void fill_in_sin_tables(void) {
    /* atan_tab0/1 are byte-scaled: arctan() recovers the angle with << 8
     * (0x4000 = 90°). From init_fill_in_sin_tables_414F40:
     *   tab0[i] = trunc(128 * atan(i / 64) / π + 0.5)
     *   tab1[i] = trunc(128 * atan(i /  4) / π + 0.5) */
    for (int i = 0; i < 256; i++) {
        atan_tab0[i] = (int8_t)(128.0 * atan((double)i / 64.0) / M_PI + 0.5);
        atan_tab1[i] = (int8_t)(128.0 * atan((double)i /  4.0) / M_PI + 0.5);
    }

    /* Fill shade_tab[17][128][128]
     * Each material maps to a color group (0..7) and a modulation type (0..2).
     * Material 0 always outputs 0.
     * Material 16 is a special "invisible" marker (0x80).
     *
     * E1 (asm 0x41244B): shade_val = intensity/2 + 63, then type 1 subtracts 48,
     *   type 2 subtracts 80.  No depth_shade dimension — all 128 j-rows identical.
     * E2: shade_val = (j * intensity) >> 7, type 1 darkens by 3/8, type 2 by 5/8.
     *   j-row selected by depth_shade in shade_ellipse. */
    static const int mat_colorgroup[16] = {0,1,1,1,2,2,3,3,4,4,5,5,6,6,7,7};
    static const int mat_modtype[16]    = {0,2,1,0,1,0,1,0,1,0,1,0,1,0,1,0};
    for (int i = 0; i < 16; i++) {
        int color_group = mat_colorgroup[i];
        int mod_type = mat_modtype[i];
        if (game_version == GAME_VERSION_E1) {
            for (int intensity = 0; intensity < 128; intensity++) {
                int shade_val = intensity / 2 + 63;
                if (mod_type == 1) shade_val -= 48;
                if (mod_type == 2) shade_val -= 80;
                char entry = 0;
                if (shade_val >= 0 && shade_val <= 127 && i != 0)
                    entry = (char)(32 * color_group + (shade_val >> 2));
                for (int j = 0; j < 128; j++)
                    shade_tab[i][j][intensity] = entry;
            }
        } else {
            for (int j = 0; j < 128; j++) {
                for (int intensity = 0; intensity < 128; intensity++) {
                    int shade_val = (j * intensity) >> 7;
                    if (mod_type == 1) shade_val = shade_val - j * 3 / 8;
                    if (mod_type == 2) shade_val = shade_val - j * 5 / 8;
                    if (shade_val > 0 && i != 0) {
                        if (shade_val > 127) shade_val = 127;
                        shade_tab[i][j][intensity] = (char)(32 * color_group + (shade_val >> 2));
                    } else {
                        shade_tab[i][j][intensity] = 0;
                    }
                }
            }
        }
    }
    for (int j = 0; j < 128; j++)
        for (int intensity = 0; intensity < 128; intensity++)
            shade_tab[16][j][intensity] = (char)0x80;

    /* Fill shade_texture[8][128][128] — specular/highlight shading */
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 128; j++) {
            for (int k = 0; k < 128; k++) {
                int value = i;
                int specular = k * (k / 2 + j / 2) >> 7;
                if (!i) { value = 3; specular -= 80; }
                if (specular > 0) {
                    if (specular > 127) specular = 127;
                    shade_texture[i][j][k] = (char)(32 * value + (specular >> 2));
                } else {
                    shade_texture[i][j][k] = 0;
                }
            }
        }
    }

    /* Fill sine_table and cosn_table (14-bit fixed-point) */
    for (int i = 0; i < 65536; i++) {
        double angle = (double)i * 2.0 * M_PI / 65536.0;
        sine_table[i] = (int16_t)(sin(angle) * 16384.0);
        cosn_table[i] = (int16_t)(cos(angle) * 16384.0);
    }

    /* Fill arcsin_tab */
    for (int i = 0; i < 32768; i++) {
        double x = (double)i / 16384.0 - 1.0;
        if (x > 1.0) x = 1.0;
        if (x < -1.0) x = -1.0;
        arcsin_tab[i] = (int16_t)(asin(x) * 32768.0 / M_PI);
    }
}

/* init_fill_in_shadow_tab_415560 — load_shadow_tab() reads the precomputed
 * SHADOW.DAT instead. */
void fill_in_shadow_tab(void) {
}

/* init_load_shadow_tab  E1: ? | E2: 0x415B4C */
void load_shadow_tab(void) {
    FILE *f = fopen_ci("SHADOW.DAT", "rb");
    if (!f) return;
    fread_ignore(shadow_tab, 1, sizeof(shadow_tab), f);
    fclose(f);
}

/* init_load_anti_alias  E1: ? | E2: 0x415C3C */
int load_anti_alias(void) {
    return 1;  /* Anti-aliasing not in final game */
}

/* init_wait_for_interrupt  E1: 0x41262C | E2: 0x415CF4 — not needed. */

/* init_my_time  E1: 0x4199B4 | E2: 0x41CF30 */
int32_t my_time(void) {
#ifdef MY_TIME_DETERMINISTIC
    static int32_t deterministic_time = 0;
    return deterministic_time++;
#else
    /* Original: E1 = 60 * clock / 100, E2 = 70 * clock / 100 */
    int32_t rate = (game_version == GAME_VERSION_E1) ? 60 : 70;
    return (int32_t)(platform_ticks(NULL) * rate / 1000);
#endif
}

/* Not in the original, which spun on my_time. Sleeping frees the core for the
 * other threads and keeps the handhelds cool; the last millisecond is still
 * polled because a sleep may overshoot by its timer granularity. */
int32_t wait_for_tick_after(int32_t t) {
#ifndef MY_TIME_DETERMINISTIC
    uint32_t rate = (game_version == GAME_VERSION_E1) ? 60 : 70;
    uint32_t next_ms = ((uint32_t)(t + 1) * 1000u + rate - 1) / rate;
    int32_t left = (int32_t)(next_ms - platform_ticks(NULL));
    if (left > 1)
        platform_delay((uint32_t)(left - 1));
#endif
    int32_t now;
    while ((now = my_time()) == t)
        ;
    return now;
}

/* init_get_joystick  E1: 0x41264C | E2: 0x415D14 */
void get_joystick(void) {
    joystick = 1000;  /* No direction */

    if (key8_pressed) joystick = 0;       /* Up */
    else if (key2_pressed) joystick = 4;   /* Down */
    else if (key4_pressed) joystick = 6;   /* Left */
    else if (key6_pressed) joystick = 2;   /* Right */
    if (game_version == GAME_VERSION_E1) {
        /* E1: NUM7/9/1/3 are action keys, not movement.
         * Only arrow diagonals contribute to joystick direction. */
        if (key7_pressed && !extra_keys_pressed[71]) joystick = 7;
        else if (key9_pressed && !extra_keys_pressed[73]) joystick = 1;
        else if (key1_pressed && !extra_keys_pressed[79]) joystick = 5;
        else if (key3_pressed && !extra_keys_pressed[81]) joystick = 3;
    } else {
        if (key7_pressed) joystick = 7;   /* Up-Left */
        else if (key9_pressed) joystick = 1;   /* Up-Right */
        else if (key1_pressed) joystick = 5;   /* Down-Left */
        else if (key3_pressed) joystick = 3;   /* Down-Right */
    }

    joy_button = space_pressed ? 1 : 0;

    /* E1 F-key speed modes (original at 0x4128DF):
     * F1-F4 → execute "Key_F1_4" code, F5-F8 → "Key_F5_8", F9-F12 → "Key_F9_12".
     * Script codes modify actor's action_slots to swap sneak/walk/run animations. */
    if (game_version == GAME_VERSION_E1) {
        static int16_t key_f_code_idx[3] = { -2, -2, -2 };
        if (key_f_code_idx[0] == -2) {
            key_f_code_idx[0] = find_code_name_index("Key_F1_4");
            key_f_code_idx[1] = find_code_name_index("Key_F5_8");
            key_f_code_idx[2] = find_code_name_index("Key_F9_12");
        }

        for (int g = 0; g < 3; g++) {
            bool pressed = false;
            for (int i = 0; i < 4; i++) {
                if (extra_keys_were_pressed[0x70 + g * 4 + i]) {
                    extra_keys_were_pressed[0x70 + g * 4 + i] = 0;
                    pressed = true;
                }
            }
            if (pressed && selected_thing) {
                int16_t ci = key_f_code_idx[g];
                if (ci >= 0 && code_tab[ci]) {
                    execute_thing_code(selected_thing, ci);
                } else {
                    movement_speed_mode = (int16_t)(g * 4);
                }
            }
        }

        for (int g = 0; g < 3; g++) {
            if (keys_were_pressed_codes[0x31 + g] && selected_thing) {
                keys_were_pressed_codes[0x31 + g] = 0;
                int16_t ci = key_f_code_idx[g];
                if (ci >= 0 && code_tab[ci])
                    execute_thing_code(selected_thing, ci);
                else
                    movement_speed_mode = (int16_t)(g * 4);
            }
        }
    }

    /* Return: inventory page (byte_4C39AC). E2 only: E1 has no ICONPAGE,
     * and show_icon_page() switches to SVGA before it finds that out. */
    if (key_return_was_pressed) {
        key_return_was_pressed = false;
        if (game_version != GAME_VERSION_E1) {
            stop_the_clock = true;
            show_icon_page();
        }
    }

    /* I: toggle HUD icons (byte_4C39A7). */
    if (key_i_was_pressed) {
        key_i_was_pressed = false;
        no_icons = !no_icons;
        if (!no_icons)
            update_game_icons();
        else
            clear_game_icons();
    }

    /* 0x4129B4 gates on game_up_and_running alone, so the menu is reachable
     * during the intro too. */
    if ((key_esc_was_pressed || key_esc_was_forced) && game_up_and_running) {
        stop_the_clock = true;
        if (key_esc_was_forced) {
            menu_no_continue = true;
            key_esc_was_forced = 0;
        }
        key_esc_was_pressed = false;
        do_request();
    }
}

/* init_get_mouse  E1: 0x4184D8 | E2: 0x41BA50 */
void get_mouse(void) {
    /* init_get_mouse_41BA7C ran the Win32 message pump here. */
    window_proc();

    if (!app_active) {
        platform_delay(100);
        return;
    }
}

/* init_clear_keys_pressed  E1: 0x419A14 | E2: 0x41CF94 */
void clear_keys_pressed(void) {
    memset(keys_were_pressed_codes, 0, 256);
    memset(keys_were_pressed, 0, 256);
    memset(keys_pressed, 0, 256);
    memset(extra_keys_were_pressed, 0, 256);
    memset(extra_keys_pressed, 0, 256);

    space_pressed = false;
    ctrl_pressed = false;
    alt_pressed = false;
    key1_pressed = key2_pressed = key3_pressed = key4_pressed = false;
    key5_pressed = key6_pressed = key7_pressed = key8_pressed = key9_pressed = false;
    key_esc_was_pressed = false;
    key_i_was_pressed = false;
    key_return_was_pressed = false;
}

/* init_clear_ptr_tabs  E1: 0x413564 | E2: 0x416ACC */
void clear_ptr_tabs(void) {
    memset(thing_tab, 0, sizeof(thing_tab));
    memset(action_tab, 0, sizeof(action_tab));
    memset(scene_tab, 0, sizeof(scene_tab));
    memset(code_tab, 0, sizeof(code_tab));
    memset(repertoire_tab, 0, sizeof(repertoire_tab));
    memset(sound_tab, 0, sizeof(sound_tab));
    memset(map_area_tab, 0, sizeof(map_area_tab));
    memset(texture_tab, 0, sizeof(texture_tab));
}

/* init_init_material_flags  E1: 0x413604 | E2: 0x416B7C */
void init_material_flags(void) {
    /* Hardcoded, from init_init_material_flags_416BA8. */
    material_flags[0]  = 0;
    material_flags[1]  = 14;
    material_flags[2]  = 7;
    material_flags[3]  = 8;
    material_flags[4]  = 0;
    material_flags[5]  = 0;
    material_flags[6]  = 0;
    material_flags[7]  = 0;
    material_flags[8]  = 0;
    material_flags[9]  = 6;
    material_flags[10] = 14;
    material_flags[11] = 0;
    material_flags[12] = 14;
    material_flags[13] = 0;
    material_flags[14] = 15;
    material_flags[15] = 15;
    material_flags[16] = 0;
    material_flags[17] = 0;
    material_flags[18] = 16;
    material_flags[19] = 16;
    material_flags[20] = 7;
    material_flags[21] = 16;
    material_flags[22] = 7;
    material_flags[23] = 8;
    material_flags[24] = 0;
    material_flags[25] = 0;
    material_flags[26] = 0;
    material_flags[27] = 0;
    material_flags[28] = 0;
    material_flags[29] = 0;
}

/* init_init_event_type_flags  E1: 0x413710 | E2: 0x416C88 */
void init_event_type_flags(void) {
    memset(event_type_flags, 0, sizeof(event_type_flags));
    memset(event_priority, 0, sizeof(event_priority));

    /* Part-targeted events */
    event_type_flags[ROTATE]          = EVENT_FLAGS_NO_SUPRESS | EVENT_FLAGS_PART;
    event_type_flags[OFFSET]          = EVENT_FLAGS_NO_SUPRESS | EVENT_FLAGS_PART;
    event_type_flags[VECTOR1]         = EVENT_FLAGS_NO_SUPRESS | EVENT_FLAGS_PART;
    event_type_flags[VECTOR2]         = EVENT_FLAGS_PART;
    event_type_flags[VECTOR3]         = EVENT_FLAGS_PART;
    event_type_flags[COLOUR]          = EVENT_FLAGS_PART | 4;
    event_type_flags[ADD_PART]        = EVENT_FLAGS_PART | 4 | 2;
    event_type_flags[ADD_THING]       = 4;
    event_type_flags[TYPE]            = EVENT_FLAGS_PART | 4;
    event_type_flags[ADD_PART_TO_THING] = 0xC;
    event_type_flags[FLAGS]           = EVENT_FLAGS_PART | 4 | 2;
    event_type_flags[DISP_PNT]        = EVENT_FLAGS_PART;
    event_type_flags[ANCHOR_PART]     = EVENT_FLAGS_PART | 4 | 2;
    event_type_flags[LOOSEN_JOINT]    = EVENT_FLAGS_PART | 4 | 2;
    event_type_flags[UNLOOSEN_JOINT]  = EVENT_FLAGS_PART | 4 | 2;
    event_type_flags[POSITION]        = EVENT_FLAGS_NO_SUPRESS | EVENT_FLAGS_PART;
    event_type_flags[TWO_PART_LIMB]   = EVENT_FLAGS_PART | 4 | 2;
    event_type_flags[FIX_PART]        = EVENT_FLAGS_PART | 4 | 2;
    event_type_flags[UNFIX_PART]      = EVENT_FLAGS_PART | 4 | 2;
    event_type_flags[UNMAKE_LIMB]     = EVENT_FLAGS_PART | 4 | 2;
    event_type_flags[ABSOLUTE_POS]    = EVENT_FLAGS_PART;
    event_type_flags[ABSOLUTE_ROT]    = EVENT_FLAGS_PART;
    event_type_flags[DEF_ROTATE]      = EVENT_FLAGS_PART | 4 | 2;
    event_type_flags[DEF_OFFSET]      = EVENT_FLAGS_PART | 4 | 2;
    event_type_flags[DEF_VECTOR1]     = EVENT_FLAGS_PART | 4 | 2;
    event_type_flags[DEF_VECTOR2]     = EVENT_FLAGS_PART | 4 | 2;
    event_type_flags[DEF_COLOUR]      = EVENT_FLAGS_PART | 4 | 2;
    event_type_flags[DEF_FLAGS]       = EVENT_FLAGS_PART | 4 | 2;
    event_type_flags[DEF_POSITION]    = EVENT_FLAGS_PART | 4 | 2;
    event_type_flags[CUT_PART]        = EVENT_FLAGS_PART | 4 | 2;
    event_type_flags[SHADE]           = EVENT_FLAGS_PART;
    event_type_flags[POINT_TO_POINT]  = EVENT_FLAGS_PART | 4;
    event_type_flags[PART_TEXTURE]    = EVENT_FLAGS_PART | 4;
    event_type_flags[N68]             = 0x200 | EVENT_FLAGS_PART;
    event_type_flags[N69]             = 0x200 | EVENT_FLAGS_PART;

    /* Triangle-targeted events */
    event_type_flags[ADD_TRIANGLE]    = EVENT_FLAGS_TRIANGLE | 4;
    event_type_flags[COLOUR_TRIANGLE] = EVENT_FLAGS_TRIANGLE | 4;
    event_type_flags[TRIANGLE_FLAGS]  = EVENT_FLAGS_TRIANGLE | 4;
    event_type_flags[TRI_SHADE_NAME]  = EVENT_FLAGS_TRIANGLE | 4;
    event_type_flags[MAKE_QUAD]       = EVENT_FLAGS_TRIANGLE | 4;
    event_type_flags[TRI_TEX_NAME]    = EVENT_FLAGS_TRIANGLE | 4;
    event_type_flags[TRI_TEXTURE1]    = EVENT_FLAGS_TRIANGLE | 4;
    event_type_flags[TRI_TEXTURE2]    = EVENT_FLAGS_TRIANGLE | 4;
    event_type_flags[TRI_TEXTURE3]    = EVENT_FLAGS_TRIANGLE | 4;

    /* Point events: ADD_POINT is part-targeted (resolves the part to add
     * the point to; original flag 0x14 = PART|4), OFFSET_POINT is
     * point-targeted (modifies an existing point). */
    event_type_flags[ADD_POINT]       = EVENT_FLAGS_PART | 4;
    event_type_flags[OFFSET_POINT]    = EVENT_FLAGS_POINT;

    /* Actor-level events (no part/tri/point lookup) */
    event_type_flags[MOVE_ACT]         = 8;
    event_type_flags[RAND_ACT]         = 8;
    event_type_flags[RAND_INFO]        = 8;
    event_type_flags[START_POSITION]   = 8;
    event_type_flags[ROTATE_THING]     = 0x88;
    event_type_flags[MOVE_THING]       = 0x88;
    event_type_flags[SCRIPT_MOVE]      = EVENT_FLAGS_NO_SUPRESS | 0xE;
    event_type_flags[SCRIPT_TURN]      = EVENT_FLAGS_NO_SUPRESS | 0xE;
    event_type_flags[THING_FLAGS]      = 0xE;
    event_type_flags[SPAWN_ACTION]     = 6;
    event_type_flags[REORIENT_THING]   = 0xE;
    event_type_flags[HELD_OFFSET]      = 8;
    event_type_flags[HELD_ROTATE]      = 8;
    event_type_flags[HELD_OFF_LEFT]    = 8;
    event_type_flags[HELD_ROT_LEFT]    = 8;
    event_type_flags[BACKGROUND]       = 0xE;
    event_type_flags[ACTOR_REP]        = 0xE;
    event_type_flags[THING_CODE]       = 8;
    event_type_flags[THING_CODE_2]     = 8;
    event_type_flags[END_ACTION]       = 0xE;
    event_type_flags[INTERACT]         = EVENT_FLAGS_PART;

    /* Priorities for event ordering */
    event_priority[ROTATE] = 0;
    event_priority[OFFSET] = 0;
    event_priority[ADD_PART] = -2;
    event_priority[ADD_THING] = -2;
    event_priority[SPAWN_ACTION] = 2;
    event_priority[INTERACT] = 1;
}

/* init_getw_4171B8 — read 16-bit big-endian */
int16_t getw_be(FILE *f) {
    unsigned char buf[2];
    if (fread(buf, 1, 2, f) != 2) return 0;
    return (int16_t)((buf[0] << 8) | buf[1]);
}

/* init_putw_417248 — write 16-bit big-endian */
void putw_be(int16_t val, FILE *f) {
    fputc((val >> 8) & 0xFF, f);
    fputc(val & 0xFF, f);
}

/* init_putw_lo_hi_417308 — write 16-bit little-endian */
int16_t putwLoHi(int16_t val, FILE *f) {
    fputc(val & 0xFF, f);
    fputc((val >> 8) & 0xFF, f);
    return val;
}

/* init_getw_lo_hi_4173C8 — read 16-bit little-endian */
int16_t getwLoHi(FILE *f) {
    unsigned char buf[2];
    if (fread(buf, 1, 2, f) != 2) return 0;
    return (int16_t)((buf[1] << 8) | buf[0]);
}

/* init_clip_blit  E1: 0x414068 | E2: 0x4175E0 */
void clip_blit(int src_plane, int src_x, int src_y, int dst_plane,
              int dst_x, int dst_y, int width, int height, int minterm) {
    clip_blit_win95(src_plane, src_x, src_y, dst_plane, dst_x, dst_y, width, height, minterm);
}

/* init_clip_blit_win95  E1: 0x41457C | E2: 0x417AF4 */
void clip_blit_win95(int src_plane, int src_x, int src_y, int dst_plane,
                   int dst_x, int dst_y, int width, int height, int minterm) {
    if (!bitmap[src_plane] || !bitmap[dst_plane]) return;
    if (width <= 0 || height <= 0) return;

    if (dst_x < 0) { src_x -= dst_x; width += dst_x; dst_x = 0; }
    if (dst_y < 0) { src_y -= dst_y; height += dst_y; dst_y = 0; }
    if (dst_x + width > screen_width) width = screen_width - dst_x;
    if (dst_y + height > screen_height) height = screen_height - dst_y;
    if (width <= 0 || height <= 0) return;

    for (int y = 0; y < height; y++) {
        int src_off = (src_y + y) * hires_width + src_x;
        int dst_off = (dst_y + y) * hires_width + dst_x;
        memcpy(bitmap[dst_plane] + dst_off, bitmap[src_plane] + src_off, width);
    }
}

/* init_put_graphic  E1: 0x414B90 | E2: 0x418108 */
void put_graphic(char *data, int plane, int x, int y, int sx, int sy) {
    put_graphic_win95(data, plane, x, y, sx, sy);
}

/* init_put_graphic_win95_41831C — .RAW graphics are row-major. Mask plane is
 * mask_map[0] for planes 0-1 and mask_map[1] for planes 2-3. */
void put_graphic_win95(char *data, int plane, int x, int y, int sx, int sy) {
    if (!data || !bitmap[plane]) return;

    int16_t *const m   = (plane < 2) ? mask_map[0] : mask_map[1];
    char *const    dst = bitmap[plane];
    const int      w   = screen_width;
    const int      h   = screen_height;
    const int      hw  = hires_width;

    /* Clip the rectangle once instead of testing every pixel. The transparency
     * test stays in the loop — it is per-pixel data, not geometry. */
    int col0 = (x < 0) ? -x : 0;
    int row0 = (y < 0) ? -y : 0;
    int col1 = (x + sx > w) ? w - x : sx;
    int row1 = (y + sy > h) ? h - y : sy;

    for (int row = row0; row < row1; row++) {
        const char *src = data + row * sx;
        int off = (y + row) * hw + x;
        for (int col = col0; col < col1; col++) {
            char pixel = src[col];
            /* 0xFF is the transparency key; Open Watcom's char is unsigned,
             * so comparing with -1 never matches there. */
            if ((unsigned char)pixel == 0xFFu) continue;
            dst[off + col] = pixel;
            if (m) m[off + col] = 0;
        }
    }
}

/* init_clear_background  E1: 0x414EE0 | E2: 0x418458 */
void clear_background(int plane, int x, int y, int sx, int sy) {
    clear_background_win95(plane, x, y, sx, sy);
}

/* init_clear_background_win95  E1: 0x414F70 | E2: 0x4184E8 */
void clear_background_win95(int plane, int x, int y, int sx, int sy) {
    if (!bitmap[2] || !bitmap[plane]) return;

    for (int row = 0; row < sy; row++) {
        int py = y + row;
        if (py < 0 || py >= screen_height) continue;
        int off = py * hires_width + x;
        int len = sx;
        if (x < 0) { off -= x; len += x; }
        if (x + sx > screen_width) len = screen_width - x;
        if (len > 0) {
            memcpy(bitmap[plane] + off, bitmap[2] + off, len);
        }
    }
}

/* init_clip_mask  E1: 0x4150E8 | E2: 0x418660 */
void clip_mask(int src, int dst, int x, int y, int sx, int sy) {
    if (!mask_map[src] || !mask_map[dst]) return;

    for (int row = 0; row < sy; row++) {
        int py = y + row;
        if (py < 0 || py >= screen_height) continue;
        int off = py * hires_width + x;
        memcpy(mask_map[dst] + off, mask_map[src] + off, sx * sizeof(int16_t));
    }
}

/* init_convert_ascii  E1: 0x415CF8 | E2: 0x419270 */
int convert_ascii(int ascii_code) {
    if (ascii_code >= 32 && ascii_code <= 126) {
        return ascii_code - 32;
    }
    return 102;  /* fallback character */
}

/* init_text_418770 — SmallTextWIN95. Glyph cells hold '#' for foreground and
 * ' ' for background. The pen advances 6 per glyph; length 0 means up to the
 * NUL terminator. */
void text(int plane, const char *text, int length) {
    if (!text || !bitmap[plane]) return;
    int limit = length ? length : 10000;

    char a_color = (char)a_pen_colour;
    char b_color = (char)b_pen_colour;

    char *const dst        = bitmap[plane];
    const int   w          = screen_width;
    const int   h          = screen_height;
    const int   hw         = hires_width;
    const int   fill_bg    = (draw_mode[plane] == 2);
    const int   glyph_w    = tx_w;
    const int   glyph_h    = tx_h;

    for (int i = 0; i < limit && text[i]; i++) {
        int char_idx = convert_ascii((unsigned char)text[i]);
        int px = pen_position_x[plane];
        int py = pen_position_y[plane];

        int cx0 = (px < 0) ? -px : 0;
        int cy0 = (py < 0) ? -py : 0;
        int cx1 = (px + glyph_w > w) ? w - px : glyph_w;
        int cy1 = (py + glyph_h > h) ? h - py : glyph_h;

        const char *glyph = character_set[char_idx];
        for (int cy = cy0; cy < cy1; cy++) {
            const char *grow = glyph + cy * glyph_w;
            int off = (py + cy) * hw + px;
            for (int cx = cx0; cx < cx1; cx++) {
                if (grow[cx] == '#') {
                    dst[off + cx] = a_color;
                } else if (fill_bg) {
                    dst[off + cx] = b_color;
                }
            }
        }
        pen_position_x[plane] += glyph_w;
    }
}

/* init_small_text_win95  E1: 0x4155F4 | E2: 0x418B6C */
void small_text_win95(int plane, const char *str, int length) {
    text(plane, str, length);
}

/* init_anti_aliased_text  E1: 0x415A44 | E2: 0x418FBC */
void anti_aliased_text(int plane, const char *str, int length) {
    text(plane, str, length);
}

/* init_text_with_mask  E1: 0x415F04 | E2: 0x41947C */
void text_with_mask(int plane, const char *str, int length) {
    text_with_mask_win95(plane, str, length);
}

/* init_text_with_mask_win95_41987C — fills the background colour and zeroes
 * the mask at every glyph cell, so text draws over any masked layer. */
void text_with_mask_win95(int plane, const char *str, int length) {
    text_with_mask_scaled(plane, str, length, 1);
}

/* Port addition: text_with_mask_win95 with each glyph pixel scaled up. */
void text_with_mask_scaled(int plane, const char *str, int length, int scale) {
    if (!str || !bitmap[plane]) return;
    if (scale < 1) scale = 1;
    int limit = length ? length : 10000;

    char a_color = (char)a_pen_colour;
    char b_color = (char)b_pen_colour;
    int16_t *mask_plane = (plane < 2) ? mask_map[0] : mask_map[1];

    for (int i = 0; i < limit && str[i]; i++) {
        int char_idx = convert_ascii((unsigned char)str[i]);
        int px = pen_position_x[plane];
        int py = pen_position_y[plane];

        for (int cy = 0; cy < tx_h; cy++) {
            for (int cx = 0; cx < tx_w; cx++) {
                char col = (character_set[char_idx][cy * tx_w + cx] == '#')
                         ? a_color : b_color;
                for (int sy = py + cy * scale; sy < py + (cy + 1) * scale; sy++) {
                    if (sy < 0 || sy >= screen_height) continue;
                    for (int sx = px + cx * scale; sx < px + (cx + 1) * scale; sx++) {
                        if (sx < 0 || sx >= screen_width) continue;
                        int off = sy * hires_width + sx;
                        bitmap[plane][off] = col;
                        if (mask_plane) mask_plane[off] = 0;
                    }
                }
            }
        }
        pen_position_x[plane] += tx_w * scale;
    }
}

/* init_rect_fill  E1: 0x4175A8 | E2: 0x41AB20 */
void rect_fill(int plane, int x, int y, int w, int h) {
    rect_fill_win95(plane, x, y, w, h);
}

/* init_rect_fill_win95  E1: 0x4177B0 | E2: 0x41AD28 */
void rect_fill_win95(int plane, int x, int y, int w, int h) {
    if (!bitmap[plane]) return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > screen_width) w = screen_width - x;
    if (y + h > screen_height) h = screen_height - y;

    for (int row = 0; row < h; row++) {
        memset(bitmap[plane] + (y + row) * hires_width + x, a_pen_colour, w);
    }
}

/* init_set_palette  E1: 0x4179E4 | E2: 0x41AF5C */
void set_palette(palette_entry_t *new_palette) {
    memcpy(view_cmap, new_palette, 256 * sizeof(palette_entry_t));
}

/* init_move  E1: 0x417AD4 | E2: 0x41B04C */
void move_pen(int indx, int16_t x, int16_t y) {
    pen_position_x[indx] = x;
    pen_position_y[indx] = y;
}

/* init_draw_41B34C — Bresenham line drawing */
void draw(int plane, int x, int y) {
    draw_win95(plane, x, y);
}

/* init_draw_win95  E1: 0x417F9C | E2: 0x41B514 */
void draw_win95(int plane, int x, int y) {
    if (!bitmap[plane]) return;

    int x0 = pen_position_x[plane];
    int y0 = pen_position_y[plane];
    int x1 = x, y1 = y;

    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    while (1) {
        if (x0 >= 0 && x0 < screen_width && y0 >= 0 && y0 < screen_height) {
            bitmap[plane][y0 * hires_width + x0] = (char)a_pen_colour;
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }

    pen_position_x[plane] = (int16_t)x;
    pen_position_y[plane] = (int16_t)y;
}

/* init_write_pixel  E1: 0x41837C | E2: 0x41B8F4 */
void write_pixel(int plane, int x, int y, int unused) {
    (void)unused;
    if (!bitmap[plane]) return;
    if (x >= 0 && x < screen_width && y >= 0 && y < screen_height) {
        bitmap[plane][y * hires_width + x] = (char)a_pen_colour;
    }
}

/* init_read_pixel  E1: 0x4183F0 | E2: 0x41B968 */
int read_pixel(int plane, int unused, int x, int y) {
    (void)unused;
    if (!bitmap[plane]) return 0;
    if (x >= 0 && x < screen_width && y >= 0 && y < screen_height) {
        return (unsigned char)bitmap[plane][y * hires_width + x];
    }
    return 0;
}

/* init_xor_pixel  E1: 0x418434 | E2: 0x41B9AC */
void xor_pixel(int plane, int x, int y) {
    if (!bitmap[plane]) return;
    if (x >= 0 && x < screen_width && y >= 0 && y < screen_height) {
        bitmap[plane][y * hires_width + x] ^= (char)a_pen_colour;
    }
}

/* init_turn_mouse_pointer_on  E1: 0x418724 | E2: 0x41BC9C */
void turn_mouse_pointer_on(void) {
    mouse_pointer_on = true;
}

/* init_turn_mouse_pointer_off  E1: 0x418758 | E2: 0x41BCD0 */
void turn_mouse_pointer_off(void) {
    mouse_pointer_on = false;
}

/* init_clear_db_mouse_cursor_win95  E1: 0x418CD8 | E2: 0x41C254 */
void clear_db_mouse_cursor_win95(void) {
}

/* init_init_colours0to8  E1: 0x419748 | E2: 0x41CCC4 */
void init_colours0to8(palette_entry_t *palette) {
    palette[0] = pal_rgb(0, 0, 0);       /* Black */
    palette[1] = pal_rgb(63, 63, 63);     /* White */
    palette[2] = pal_rgb(63, 0, 0);       /* Red */
    palette[3] = pal_rgb(63, 63, 0);      /* Yellow */
    palette[4] = pal_rgb(20, 20, 20);     /* Dark gray */
    palette[5] = pal_rgb(40, 40, 40);     /* Gray */
    palette[6] = pal_rgb(0, 0, 32);       /* Dark blue */
    palette[7] = pal_rgb(32, 48, 63);     /* Light cyan */
}

/* init_load_shade_map  E1: 0x4198E0 | E2: 0x41CE5C */
void load_shade_map(void) {
    FILE *f = fopen_ci("shademap.dat", "rb");
    if (!f) return;

    /* Static because 48 KB overflows DOS/4GW's default 64 KB stack silently. */
    static uint8_t buf[128 * 128 * 3];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n < sizeof(buf)) return;

    const uint8_t *p = buf;
    for (int y = 0; y < 128; y++) {
        for (int x = 0; x < 128; x++) {
            shade_map[y][x] = (char)*p++;
            int16_t hi = *p++;
            int16_t lo = *p++;
            profile[y][x] = (int16_t)((hi << 8) | lo);
        }
    }

}

/* init_clear_mask_rect  E1: 0x4199F8 | E2: 0x41CF78 */
void clear_mask_rect(int mask, int x, int y, int w, int h) {
    if (!mask_map[mask]) return;
    clip_mask(0, mask, x, y, w, h);
}

/* init_set_load_by_offset  E1: 0x41A59C | E2: 0x41DB1C */
void set_load_by_offset(void) {
    load_by_offset = true;
}

/* init_reverse_char_word  E1: 0x41A5EC | E2: 0x41DB6C */
void reverse_char_word(void) {
}

/* init_shift_char_word  E1: 0x41A674 | E2: 0x41DBF4 */
void shift_char_word(void) {
}

/* init_change_character_set_41DC8C — language font remap; not called. */
void change_character_set(void) {
}

/* init_change_subtitles_41E3C4 — not called. */
void change_subtitles(void) {
}

/* init_change_text_jap_41E85C — not called. */
void change_text_jap(void) {
}

/* init_dd_lock  E1: 0x415460 | E2: 0x4189D8 */
char *dd_lock(int plane, int *pitch) {
    if (pitch) *pitch = screen_width;
    return bitmap[plane];
}

/* init_dd_unlock  E1: 0x41555C | E2: 0x418AD4 */
void dd_unlock(int plane, char *data) {
    (void)plane;
    (void)data;
}

void show_timer(int timer_val, char reset, int unused) {
    (void)unused;
    if (reset) game_timer = -1;
    char buf[20];
    snprintf(buf, sizeof(buf), "Timer: %10d", timer_val);
    a_pen_colour = 1;
    b_pen_colour = 0;
    draw_mode[1 - db] = 2;
    move_pen((int16_t)(1 - db), 10, 10);   /* RATE_BOX_LEFT, RATE_BOX_TOP */
    small_text_win95((int16_t)(1 - db), buf, 0);
}
