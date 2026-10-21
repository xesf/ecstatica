/**
 * menu.c
 *
 * Game menu system: main menu, pause menu, settings, drawn in the original
 * requester style.
 */

#include "menu.h"
#include "display.h"
#include "edit.h"
#include "file.h"
#include "game.h"
#include "init.h"
#include "music.h"
#include "render.h"
#include "req.h"
#include "platform.h"
#include "win.h"
#include "asm_f.h"
#include "icon.h"
#include <string.h>
#include <stdio.h>

static void handle_main_menu_selection(void);
void do_load_menu(void);
void do_save_menu(void);
void do_settings_menu(void);
void do_enhanced_menu(void);
int  do_slot_select(const char *title);

static int menu_selection = 0;
static int menu_active = 0;
static int menu_result = -1;
bool menu_no_continue = false;

/* PSP/Vita's Circle binding needs to tell gameplay from a menu: see the
 * btn_east handling in window_proc(). */
bool menu_is_active(void) {
    return menu_active != 0;
}

static void menu_frame_start(void) {
    window_proc();
}

static void menu_frame_end(void) {
    show_parts();
    platform_blit(NULL, (const uint8_t *)bitmap[1 - db], (const uint8_t *)colour_map);
    platform_delay(16);
}

static bool menu_nav_up(void) {
    return key8_pressed || extra_keys_pressed[72];
}

static bool menu_nav_down(void) {
    return key2_pressed || extra_keys_pressed[80];
}

static bool menu_nav_left(void) {
    return key4_pressed || extra_keys_pressed[75];
}

static bool menu_nav_right(void) {
    return key6_pressed || extra_keys_pressed[77];
}

static bool menu_confirm(void) {
    return space_pressed || enter_pressed || joy_button;
}

static bool menu_back(void) {
    if (key_esc_was_pressed) {
        key_esc_was_pressed = false;
        return true;
    }
    return false;
}

/* Palette indices per version. E1 (req_request_431034 / show_gadget_43166C)
 * uses the reds 0xC3–0xDD; E2 (req_request_43B304 / show_gadget_43B93C) uses
 * greys and gold at 8/10/14/15. */
static int col_fill(void)       { return (game_version == GAME_VERSION_E1) ? 0xC9 : 15; }
static int col_bevel_light(void) { return (game_version == GAME_VERSION_E1) ? 0xCF :  8; }
static int col_bevel_dark(void)  { return (game_version == GAME_VERSION_E1) ? 0xC3 : 14; }
static int col_text(void)       { return (game_version == GAME_VERSION_E1) ? 0xD8 : 10; }
static int col_text_sel(void)   { return (game_version == GAME_VERSION_E1) ? 0xC3 : 14; }
static int col_title(void)      { return (game_version == GAME_VERSION_E1) ? 0xDD :  8; }

/* 3D beveled box — matches original req_request / show_gadget rendering. */
static void draw_bevel(int x, int y, int w, int h, bool pressed) {
    int tl = pressed ? col_bevel_dark()  : col_bevel_light();
    int br = pressed ? col_bevel_light() : col_bevel_dark();

    a_pen_colour = tl;
    move_pen(db, (int16_t)x, (int16_t)(y + h - 1));
    draw(db, x, y);
    draw(db, x + w - 1, y);

    a_pen_colour = br;
    move_pen(db, (int16_t)(x + w - 1), (int16_t)(y + 1));
    draw(db, x + w - 1, y + h - 1);
    draw(db, x + 1, y + h - 1);
}

static void draw_panel(int px, int py, int pw, int ph) {
    draw_mode[db] = 1;
    a_pen_colour = col_fill();
    rect_fill(db, px, py, pw, ph);
    draw_bevel(px, py, pw, ph, false);
}

static void draw_panel_title(const char *title, int px, int py, int pw) {
    int len = (int)strlen(title);
    int tx = px + (pw - len * tx_w) / 2;
    a_pen_colour = col_title();
    b_pen_colour = col_fill();
    move_pen(db, (int16_t)tx, (int16_t)(py + tx_h + 2));
    text(db, title, len);
}

static void draw_menu_item(const char *label, int x, int y, int item_w, int item_h, bool selected) {
    a_pen_colour = col_fill();
    rect_fill(db, x, y, item_w, item_h);
    draw_bevel(x, y, item_w, item_h, selected);

    a_pen_colour = selected ? col_text_sel() : col_text();
    b_pen_colour = col_fill();
    int len = (int)strlen(label);
    int tx = x + (item_w - len * tx_w) / 2;
    int ty = y + (item_h - tx_h) / 2;
    move_pen(db, (int16_t)tx, (int16_t)ty);
    text(db, label, len);
}

/* Main menu. E1 (0x43CDD8): panel 210×180, gadgets 170×10 at x=20. E2 is
 * scaled for SVGA: panel 420×270, gadgets 340×15 at x=40. */

/* E1 menu items. "Enhanced" is a port addition, sat next to Settings so the
 * original panel keeps only what the release shipped. */
#define E1_MENU_ITEMS 7
static const char *e1_menu_items[E1_MENU_ITEMS] = {
    "Start game (Male)",
    "Start game (Female)",
    "Save game...",
    "Load game...",
    "Settings...",
    "Enhanced...",
    "Quit"
};
static const int e1_menu_y[E1_MENU_ITEMS] = { 20, 35, 50, 65, 80, 95, 130 };

#define E2_MENU_ITEMS 7
static const char *e2_menu_items[E2_MENU_ITEMS] = {
    "Continue",
    "New Game",
    "Load Game",
    "Save Game",
    "Settings",
    "Enhanced",
    "Quit"
};
static const int e2_menu_y[E2_MENU_ITEMS] = { 20, 35, 50, 65, 80, 95, 130 };

static int menu_item_count(void) {
    return (game_version == GAME_VERSION_E1) ? E1_MENU_ITEMS : E2_MENU_ITEMS;
}

static const char **menu_items(void) {
    return (game_version == GAME_VERSION_E1) ? e1_menu_items : e2_menu_items;
}

static const int *menu_y_offsets(void) {
    return (game_version == GAME_VERSION_E1) ? e1_menu_y : e2_menu_y;
}

/* menu_do_main_menu  E1: ? | E2P: 0x42B210 */
void do_main_menu(void) {
    capture_save_thumbnail();
    menu_active = 1;
    menu_result = -1;
    int num_items = menu_item_count();
    int first_item = (menu_no_continue && game_version != GAME_VERSION_E1) ? 1 : 0;
    menu_selection = first_item;

    while (menu_active) {
        menu_frame_start();

        draw_main_menu();

        if (menu_nav_up()) {
            if (menu_selection > first_item) menu_selection--;
            platform_delay(120);
        }
        if (menu_nav_down()) {
            if (menu_selection < num_items - 1) menu_selection++;
            platform_delay(120);
        }

        if (menu_confirm()) {
            platform_delay(150);
            handle_main_menu_selection();
        }

        if (menu_back() && !menu_no_continue) {
            menu_active = 0;
        }

        menu_frame_end();
    }

    menu_no_continue = false;

    /* Menu frames pump window_proc, which latches the Enter/Space that picked
     * the item; left set, the first game frame took that Enter as the
     * inventory key (get_joystick). */
    clear_keys_pressed();
    space_was_pressed = false;
    enter_was_pressed = false;

    /* Actions run after the menu loop, as the original ran them outside req_request. */
    if (menu_result >= 0) {
        if (game_version == GAME_VERSION_E1) {
            switch (menu_result) {
            case 0: female = 0; start_game_medium(0, 0); break;
            case 1: female = 1; start_game_medium(0, 0); break;
            }
        } else {
            if (menu_result == 1)
                start_game_medium(0, 0);
        }
    }
}

/* menu_draw_main_menu  E1: ? | E2P: 0x42B2E0 */
void draw_main_menu(void) {
    int num_items = menu_item_count();
    const char **items = menu_items();
    const int *y_offsets = menu_y_offsets();

    int panel_w, panel_h, gadget_w, gadget_h, gadget_x;
    if (mode_svga) {
        panel_w = 420; panel_h = 270;
        gadget_w = 340; gadget_h = 15; gadget_x = 40;
    } else {
        panel_w = 210; panel_h = 180;
        gadget_w = 170; gadget_h = 10; gadget_x = 20;
    }

    int px = (screen_width - panel_w) / 2;
    int py = (screen_height - panel_h) / 2;

    draw_panel(px, py, panel_w, panel_h);

    const char *title = (game_version == GAME_VERSION_E1) ? "Ecstatica" : "Ecstatica II";
    draw_panel_title(title, px, py, panel_w);

    int first_item = (menu_no_continue && game_version != GAME_VERSION_E1) ? 1 : 0;
    for (int i = first_item; i < num_items; i++) {
        int y_off = y_offsets[i];
        if (mode_svga)
            y_off = (3 * y_off + (y_off < 0 ? 1 : 0)) / 2;
        draw_menu_item(items[i], px + gadget_x, py + y_off,
                       gadget_w, gadget_h, i == menu_selection);
    }
}

/* menu_handle_main_menu_selection  E1: ? | E2P: 0x42B3B0 */
static void handle_main_menu_selection(void) {
    if (game_version == GAME_VERSION_E1) {
        switch (menu_selection) {
        case 0: /* Start game (Male) */
        case 1: /* Start game (Female) */
            menu_result = menu_selection;
            menu_active = 0;
            break;
        case 2: /* Save game */
            do_save_menu();
            break;
        case 3: /* Load game */
            do_load_menu();
            break;
        case 4: /* Settings */
            do_settings_menu();
            break;
        case 5: /* Enhanced */
            do_enhanced_menu();
            break;
        case 6: /* Quit */
            program_up_and_running = false;
            menu_active = 0;
            break;
        }
    } else {
        switch (menu_selection) {
        case 0: /* Continue */
            if (!menu_no_continue)
                menu_active = 0;
            break;
        case 1: /* New Game */
            menu_result = 1;
            menu_active = 0;
            break;
        case 2: /* Load Game */
            do_load_menu();
            break;
        case 3: /* Save Game */
            do_save_menu();
            break;
        case 4: /* Settings */
            do_settings_menu();
            break;
        case 5: /* Enhanced */
            do_enhanced_menu();
            break;
        case 6: /* Quit */
            program_up_and_running = false;
            menu_active = 0;
            break;
        }
    }
}

/* menu_do_load_menu  E1: ? | E2P: 0x42B480 */
void do_load_menu(void) {
    int slot = do_slot_select("Load Game");
    if (slot >= 0) {
        if (!check_saved_game(slot)) {
            beep_message("No saved game");
            return;
        }
        load_game(slot);
        menu_active = 0;
    }
}

/* menu_do_save_menu  E1: ? | E2P: 0x42B550 */
void do_save_menu(void) {
    /* req_handle_save_game (0x43D3FC) refuses during the intro. */
    if (intro_flag) {
        beep_message("Not during the intro");
        return;
    }

    int slot = do_slot_select("Save Game");
    if (slot >= 0) {
        save_game(slot);
        beep_message("Game Saved");
    }
}

#define NUM_SLOTS 11
#define THUMB_W 80
#define THUMB_H 60

static void draw_thumbnail(int dx, int dy, const uint8_t *thumb) {
    int scale = mode_svga ? 2 : 1;
    int tw = THUMB_W * scale;
    int th = THUMB_H * scale;
    uint8_t *dst = (uint8_t *)bitmap[db];
    for (int sy = 0; sy < THUMB_H; sy++) {
        for (int sx = 0; sx < THUMB_W; sx++) {
            uint8_t pixel = thumb[sy * THUMB_W + sx];
            for (int ry = 0; ry < scale; ry++) {
                int py2 = dy + sy * scale + ry;
                if (py2 < 0 || py2 >= screen_height) continue;
                for (int rx = 0; rx < scale; rx++) {
                    int px2 = dx + sx * scale + rx;
                    if (px2 < 0 || px2 >= screen_width) continue;
                    dst[py2 * screen_width + px2] = pixel;
                }
            }
        }
    }
    draw_bevel(dx - 1, dy - 1, tw + 2, th + 2, true);
}

/* menu_do_slot_select  E1: ? | E2P: 0x42B620 */
int do_slot_select(const char *title) {
    int selected = 0;
    int scroll_top = 0;
    int visible = NUM_SLOTS < 8 ? NUM_SLOTS : 8;

    int item_h = tx_h + 8;
    int item_w = 22 * tx_w;
    int thumb_scale = mode_svga ? 2 : 1;
    int thumb_disp_w = THUMB_W * thumb_scale;
    int thumb_disp_h = THUMB_H * thumb_scale;
    int thumb_area_w = thumb_disp_w + 16;
    int panel_w = item_w + thumb_area_w + 16;
    int list_h = visible * item_h;
    int min_h = thumb_disp_h + 8;
    int content_h = list_h > min_h ? list_h : min_h;
    int panel_h = content_h + 28;
    int px = (screen_width - panel_w) / 2;
    int py = (screen_height - panel_h) / 2;

    uint8_t thumb_buf[THUMB_W * THUMB_H];
    int thumb_loaded = -1;

    for (;;) {
        menu_frame_start();

        draw_panel(px, py, panel_w, panel_h);
        draw_panel_title(title, px, py, panel_w);

        int item_x = px + 8;
        int start_y = py + 22;

        for (int i = 0; i < visible && (scroll_top + i) < NUM_SLOTS; i++) {
            int slot = scroll_top + i;
            int y = start_y + i * item_h;
            char label[48];
            char name[28];
            if (get_save_name(slot, name, sizeof(name)))
                snprintf(label, sizeof(label), "%d: %s", slot + 1, name);
            else
                snprintf(label, sizeof(label), "%d: ---", slot + 1);
            draw_menu_item(label, item_x, y, item_w, item_h, slot == selected);
        }

        if (thumb_loaded != selected) {
            thumb_loaded = selected;
            if (!load_saved_thumbnail(selected, thumb_buf))
                memset(thumb_buf, 0, sizeof(thumb_buf));
        }
        int thumb_x = px + item_w + 16 + 4;
        int thumb_y = start_y + (content_h - thumb_disp_h) / 2;
        draw_thumbnail(thumb_x, thumb_y, thumb_buf);

        if (menu_nav_up()) {
            if (selected > 0) {
                selected--;
                if (selected < scroll_top) scroll_top = selected;
            }
            platform_delay(120);
        }
        if (menu_nav_down()) {
            if (selected < NUM_SLOTS - 1) {
                selected++;
                if (selected >= scroll_top + visible) scroll_top = selected - visible + 1;
            }
            platform_delay(120);
        }

        if (menu_confirm()) {
            platform_delay(150);
            return selected;
        }
        if (menu_back()) {
            return -1;
        }

        menu_frame_end();
    }
}

static const char *difficulty_names[] = {"Easy", "Medium", "Hard"};

/* Indices match the script tokens. E1 has English/German/French only. */
static const char *language_names[] = {
    "English", "German", "French", "", "Italian", "Spanish", "Polish"
};
#define NUM_LANGUAGES_E1 3
#define NUM_LANGUAGES_E2 7
#define SETTINGS_ITEMS 5

/* Settings row: gadget-style with label left, value right */
static void draw_setting_row(const char *label, const char *value,
                             int x, int y, int w, bool selected, bool has_arrows) {
    int item_h = tx_h + 4;
    int ty = y + (item_h - tx_h) / 2;

    a_pen_colour = col_fill();
    rect_fill(db, x, y, w, item_h);
    draw_bevel(x, y, w, item_h, selected);

    a_pen_colour = selected ? col_text_sel() : col_text();
    move_pen(db, (int16_t)(x + 4), (int16_t)ty);
    text(db, label, (int)strlen(label));

    if (has_arrows) {
        char buf[48];
        snprintf(buf, sizeof(buf), "< %s >", value);
        int buf_len = (int)strlen(buf);
        int vx = x + w - buf_len * tx_w - 4;
        a_pen_colour = selected ? col_title() : col_text();
        move_pen(db, (int16_t)vx, (int16_t)ty);
        text(db, buf, buf_len);
    } else {
        int val_len = (int)strlen(value);
        int vx = x + w - val_len * tx_w - 4;
        a_pen_colour = selected ? col_title() : col_text();
        move_pen(db, (int16_t)vx, (int16_t)ty);
        text(db, value, val_len);
    }
}

/* Settings item IDs. Everything the original shipped with lives in Settings;
 * the port additions live in the E1 Enhanced panel. */
enum {
    SETT_DIFFICULTY = 0,
    SETT_LANGUAGE,
    SETT_MUSIC,
    SETT_SOUNDFX,
    SETT_SUBTITLES,
    SETT_SUBTITLE_SIZE,
    SETT_SUBTITLE_HOLD,
    SETT_GRAPHICS,
    SETT_SUPERSAMPLE,
    SETT_LIGHTING,
    SETT_MAP3D,
    SETT_SCALEMODE,
    SETT_SPEED_MODE,
    SETT_WIDESCREEN,
    SETT_MAX
};

/* One row covers both the resolution and the renderer, for both games:
 *
 *   Original (VGA, software)  Enhanced (SVGA, software)  Hardware
 *
 * Hardware implies the enhanced mode.
 */
enum { GFX_ORIGINAL = 0, GFX_ENHANCED, GFX_HARDWARE };

static void gfx_apply(int mode);

/* Fills `out` with the modes this build and this game can actually reach, and
 * returns how many. One mode means there is no choice and the row is hidden. */
static int gfx_mode_list(int *out) {
    int n = 0;
    bool can_switch = enhanced_graphics_available();
    if (can_switch || !mode_svga) out[n++] = GFX_ORIGINAL;
    if (can_switch || mode_svga)  out[n++] = GFX_ENHANCED;
    if (render_available()) out[n++] = GFX_HARDWARE;
    return n;
}

static int gfx_current_mode(void) {
    if (render_backend == RENDER_HARDWARE) return GFX_HARDWARE;
    return mode_svga ? GFX_ENHANCED : GFX_ORIGINAL;
}

static const char *gfx_mode_name(int mode) {
    switch (mode) {
    case GFX_ORIGINAL: return "Original";
    case GFX_HARDWARE: return "Hardware";
    default:           return "Enhanced";
    }
}

/* Step through the available modes. Shared with the G key (win.c) so the
 * hotkey and the menu row cannot drift apart about what a graphics mode is. */
void graphics_mode_cycle(int dir) {
    int modes[3];
    int n = gfx_mode_list(modes);
    if (n < 2) return;
    int cur = gfx_current_mode();
    int at = 0;
    for (int i = 0; i < n; i++) if (modes[i] == cur) at = i;
    at = (at + (dir > 0 ? 1 : n - 1)) % n;
    gfx_apply(modes[at]);
}

static void gfx_apply(int mode) {
    /* Drop to software first: set_enhanced_graphics re-enters the frame loop,
     * which must not happen under a live GL context. */
    if (mode != GFX_HARDWARE && render_backend == RENDER_HARDWARE)
        render_select(RENDER_SOFTWARE);

    int want_svga = (mode != GFX_ORIGINAL);
    if (mode_svga != want_svga)
        set_enhanced_graphics(want_svga);

    if (mode == GFX_HARDWARE && render_backend != RENDER_HARDWARE)
        render_select(RENDER_HARDWARE);

    render_hardware_pref = (render_backend == RENDER_HARDWARE);
    save_port_settings();
}

/* Options pinned to their original values in VGA mode; shown locked there. */
static bool setting_is_locked(int id) {
    if (mode_svga) return false;
    return id == SETT_SUBTITLE_SIZE || id == SETT_SUBTITLE_HOLD;
}

static const char *subtitle_hold_names[] = { "Original", "Long", "Match voice" };
static const char *scale_mode_names[] = { "Pillarbox", "Crop", "Stretch" };
static const char *speed_mode_names[] = { "Sneak", "Walk", "Run" };

static void settings_get_value(int id, char *buf, int bufsz) {
    if (setting_is_locked(id)) {
        snprintf(buf, bufsz, "Original");
        return;
    }
    switch (id) {
    case SETT_DIFFICULTY: {
        int d = difficulty;
        if (d < 0) d = 0;
        if (d > 2) d = 2;
        snprintf(buf, bufsz, "%s", difficulty_names[d]);
        break;
    }
    case SETT_LANGUAGE: {
        int l = language;
        int mx = (game_version == GAME_VERSION_E1) ? NUM_LANGUAGES_E1 : NUM_LANGUAGES_E2;
        if (l < 0) l = 0;
        if (l >= mx) l = mx - 1;
        if (l == 3) l = 2;
        snprintf(buf, bufsz, "%s", language_names[l]);
        break;
    }
    case SETT_MUSIC:     snprintf(buf, bufsz, "%s", music_on ? "On" : "Off"); break;
    case SETT_SOUNDFX:   snprintf(buf, bufsz, "%s", sound_fx_on ? "On" : "Off"); break;
    case SETT_SUBTITLES: snprintf(buf, bufsz, "%s", subtitles_on ? "On" : "Off"); break;
    case SETT_SUBTITLE_SIZE:
        snprintf(buf, bufsz, "%s", subtitle_scale >= 2 ? "Large" : "Normal");
        break;
    case SETT_SUBTITLE_HOLD: {
        int h = subtitle_hold;
        if (h < 0) h = 0;
        if (h > 2) h = 2;
        snprintf(buf, bufsz, "%s", subtitle_hold_names[h]);
        break;
    }
    case SETT_GRAPHICS:
        snprintf(buf, bufsz, "%s", gfx_mode_name(gfx_current_mode()));
        break;
    case SETT_SUPERSAMPLE:
        if (render_supersample <= 0)      snprintf(buf, bufsz, "Auto");
        else if (render_supersample == 1) snprintf(buf, bufsz, "Off");
        else snprintf(buf, bufsz, "%dx", (int)render_supersample);
        break;
    case SETT_LIGHTING:
        snprintf(buf, bufsz, "%s", render_enhanced_light ? "Enhanced" : "Original");
        break;
    case SETT_MAP3D:
        snprintf(buf, bufsz, "%s", render_map3d ? "3D Map" : "Pre-rendered");
        break;
    case SETT_SCALEMODE:
        snprintf(buf, bufsz, "%s", scale_mode_names[display_scale_mode]);
        break;
    case SETT_SPEED_MODE:
        snprintf(buf, bufsz, "%s", speed_mode_names[e1_speed_mode_step()]);
        break;
    case SETT_WIDESCREEN:
        snprintf(buf, bufsz, "%s", widescreen ? "16:9" : "4:3 (Original)");
        break;
    }
}

static void settings_adjust(int id, int dir) {
    if (setting_is_locked(id)) return;
    switch (id) {
    case SETT_DIFFICULTY:
        difficulty += dir;
        if (difficulty < 0) difficulty = 0;
        if (difficulty > 2) difficulty = 2;
        break;
    case SETT_LANGUAGE: {
        int mx = (game_version == GAME_VERSION_E1) ? NUM_LANGUAGES_E1 : NUM_LANGUAGES_E2;
        language += dir;
        if (language == 3) language += dir;
        if (language < 0) language = 0;
        if (language >= mx) language = mx - 1;
        break;
    }
    case SETT_MUSIC:
        music_on = !music_on;
        if (music_on) start_tune(); else stop_tune();
        break;
    case SETT_SOUNDFX:
        sound_fx_on = !sound_fx_on;
        if (!sound_fx_on) stop_samples();
        break;
    case SETT_SUBTITLES:
        subtitles_on = !subtitles_on;
        break;
    case SETT_SUBTITLE_SIZE:
        /* Retire live subtitles so the next one uses the new metrics. */
        subtitle_scale = (subtitle_scale >= 2) ? 1 : 2;
        clear_subtitles = 1;
        save_port_settings();
        break;
    case SETT_SUBTITLE_HOLD:
        subtitle_hold += dir;
        if (subtitle_hold < 0) subtitle_hold = 0;
        if (subtitle_hold > 2) subtitle_hold = 2;
        save_port_settings();
        break;
    case SETT_GRAPHICS:
        graphics_mode_cycle(dir);
        break;
    case SETT_SUPERSAMPLE:
        /* Auto, then powers of two, so the downsample stays a clean average. */
        if (dir > 0) render_supersample = (render_supersample <= 0) ? 1
                                        : (render_supersample >= 4) ? 0
                                        : (int16_t)(render_supersample * 2);
        else         render_supersample = (render_supersample <= 0) ? 4
                                        : (render_supersample == 1) ? 0
                                        : (int16_t)(render_supersample / 2);
        save_port_settings();
        break;
    case SETT_LIGHTING:
        render_enhanced_light = !render_enhanced_light;
        save_port_settings();
        break;
    case SETT_MAP3D:
        render_map3d = !render_map3d;
        save_port_settings();
        break;
    case SETT_SCALEMODE:
        win_set_scale_mode(((display_scale_mode + dir) % 3 + 3) % 3);
        save_port_settings();
        break;
    case SETT_SPEED_MODE:
        e1_cycle_speed_mode(dir);
        break;
    case SETT_WIDESCREEN:
        set_widescreen(!widescreen);
        save_port_settings();
        break;
    }
}

static const char *settings_labels[] = {
    "Difficulty", "Language", "Music", "Sound FX", "Subtitles",
    "Subtitle Size", "Subtitle Hold", "Graphics",
    "Supersampling", "3D Lighting", "Background", "Scale Mode",
    "Speed Mode", "Aspect Ratio"
};

/* Shared driver for Settings and its Enhanced sub-panel: same rows, same
 * navigation, only the item list and the title differ. */
static void run_settings_panel(const char *title, const int *items, int num_items) {
    int sel = 0;

    for (;;) {
        /* screen_width/height change under the graphics toggle. */
        int item_h = tx_h + 8;
        int item_w = 30 * tx_w;
        int panel_w = item_w + 16;
        int panel_h = num_items * item_h + 28;
        int px = (screen_width - panel_w) / 2;
        int py = (screen_height - panel_h) / 2;

        menu_frame_start();

        draw_panel(px, py, panel_w, panel_h);
        draw_panel_title(title, px, py, panel_w);

        int item_x = px + 8;
        int start_y = py + 22;

        for (int i = 0; i < num_items; i++) {
            char val[32];
            int id = items[i];
            settings_get_value(id, val, sizeof(val));
            bool arrows = !setting_is_locked(id);
            draw_setting_row(settings_labels[id], val,
                             item_x, start_y + i * item_h, item_w,
                             i == sel, arrows);
        }

        if (menu_nav_up()) {
            if (sel > 0) sel--;
            platform_delay(120);
        }
        if (menu_nav_down()) {
            if (sel < num_items - 1) sel++;
            platform_delay(120);
        }

        if (menu_nav_left() || menu_nav_right()) {
            int dir = menu_nav_right() ? 1 : -1;
            settings_adjust(items[sel], dir);
            platform_delay(150);
        }

        if (menu_confirm()) {
            int id = items[sel];
            if (id == SETT_MUSIC || id == SETT_SOUNDFX || id == SETT_SUBTITLES ||
                id == SETT_GRAPHICS || id == SETT_LIGHTING ||
                id == SETT_MAP3D || id == SETT_WIDESCREEN)
                settings_adjust(id, 1);
            platform_delay(150);
        }

        if (menu_back()) return;

        menu_frame_end();
    }
}

/* Port-only options, kept off the original Settings panel. */
void do_enhanced_menu(void) {
    int items[SETT_MAX];
    int num_items = 0;

    {
        int modes[3];
        if (gfx_mode_list(modes) > 1) items[num_items++] = SETT_GRAPHICS;
    }
    /* Only with the re-release's wide views, in a build that can show them. */
#ifdef ECS_WIDESCREEN
    if (file_wide_views_available())
        items[num_items++] = SETT_WIDESCREEN;
#endif
    items[num_items++] = SETT_SUBTITLE_SIZE;
    items[num_items++] = SETT_SUBTITLE_HOLD;
    /* Only where a hardware backend actually came up. */
    if (render_available()) {
        items[num_items++] = SETT_SUPERSAMPLE;
        items[num_items++] = SETT_LIGHTING;
        items[num_items++] = SETT_MAP3D;
    }
    if (platform_scale_mode_supported(win_platform()))
        items[num_items++] = SETT_SCALEMODE;
    /* Fallback for platforms with no L3: PSP/Vita have B/Circle for this
     * during gameplay (see window_proc()'s btn_east handling), but this row
     * still works from the menu on any platform or input device. */
    if (game_version == GAME_VERSION_E1)
        items[num_items++] = SETT_SPEED_MODE;

    run_settings_panel("Enhanced", items, num_items);
}

/* menu_do_settings_menu  E1: ? | E2P: 0x42B6F0 */
void do_settings_menu(void) {
    /* E1 has no difficulty. */
    int items[SETT_MAX];
    int num_items = 0;
    if (game_version != GAME_VERSION_E1)
        items[num_items++] = SETT_DIFFICULTY;
    items[num_items++] = SETT_LANGUAGE;
    items[num_items++] = SETT_MUSIC;
    items[num_items++] = SETT_SOUNDFX;
    items[num_items++] = SETT_SUBTITLES;

    run_settings_panel("Settings", items, num_items);
}

/* menu_do_difficulty  E1: ? | E2P: 0x42B7C0 */
void do_difficulty(void) {
    do_settings_menu();
}

/* menu_do_language  E1: ? | E2P: 0x42B890 */
void do_language(void) {
    do_settings_menu();
}

/* menu_do_sound_settings  E1: ? | E2P: 0x42B960 */
void do_sound_settings(void) {
    do_settings_menu();
}

/* menu_delete_triangle  E1: 0x430650 | E2: 0x43A928 */
void delete_triangle(tri_t *tri) {
    if (!tri) return;
    actor_t *actor = tri->parent_actor;
    if (!actor) { free_event((event_t *)tri); return; }

    if (tri == actor->polygone_tri_list) {
        actor->polygone_tri_list = tri->next;
    } else {
        for (tri_t *t = actor->polygone_tri_list; t && t->next; t = t->next) {
            if (tri == t->next) {
                t->next = t->next->next;
                break;
            }
        }
    }

    if (actor->_TriangleTab && tri->tri_index >= 0 && tri->tri_index < 500)
        actor->_TriangleTab->field_0[tri->tri_index] = NULL;

    free_event((event_t *)tri);
}

/* menu_delete_parts  E1: 0x4305A0 | E2: 0x43A878 */
void delete_parts(part_t *part) {
    if (!part) return;

    if (part->parent_actor && part->parent_actor->_PartTab &&
        part->name_index >= 0 && part->name_index < 500)
        part->parent_actor->_PartTab->field_0[part->name_index] = NULL;

    if (part->parent_actor) {
        part_t *dp = (part_t *)part->parent_actor->actor_parts_list;
        if (dp) {
            for (; dp->next_in_display_list; dp = dp->next_in_display_list) {
                if (part == dp->next_in_display_list) {
                    dp->next_in_display_list = dp->next_in_display_list->next_in_display_list;
                    break;
                }
            }
        }
    }

    part_t *child = (part_t *)part->actor_parts_list;
    while (child) {
        part_t *next_child = child->next;
        delete_parts(child);
        child = next_child;
    }

    /* Delete all triangles referencing this part's points */
    if (part->parent_actor) {
        for (point_t *pt = part->points_list; pt; pt = pt->next) {
            tri_t *tri = part->parent_actor->polygone_tri_list;
            while (tri) {
                tri_t *next_tri = tri->next;
                if (pt == tri->point1 || pt == tri->point2 ||
                    pt == tri->point3 || pt == tri->quad_point4)
                    delete_triangle(tri);
                tri = next_tri;
            }
            free_point(pt);
        }
    }

    free_part(part);
}

/* menu_remove_part  E1: 0x43081C | E2: 0x43AAF4 */
void remove_part(part_t *part) {
    if (!part) {
        do_info_req("No part to delete!");
        return;
    }

    actor_t *actor = part->holding_actor;

    /* Clear TwoPartsLimb flag if this part is the limb root */
    if (actor && actor->type != 7 && (actor->flags & 0x20) &&
        part == (part_t *)actor->actor_parts_list)
        actor->flags &= ~0x20;

    /* Also check grandparent */
    if (actor && actor->type != 7) {
        actor_t *grandparent = (actor_t *)actor->holding_actor;
        if (grandparent && grandparent->type != 7 &&
            (grandparent->flags & 0x20) &&
            grandparent->actor_parts_list &&
            part == ((part_t *)grandparent->actor_parts_list)->actor_parts_list)
            grandparent->flags &= ~0x20;
    }

    if (!actor || actor->type == 7) {
        do_info_req("Can't delete root part!");
        return;
    }

    /* 0x43AB6A: the predecessor search starts at the owning actor's root part,
     * which heads the whole display list. Starting at the holding part's first
     * child misses later siblings (add_part puts each new child right after
     * its parent in display order), leaving a dangling `next` into the freed
     * slot; a save then walked into whatever actor reused it. */
    actor_t *owner = part->parent_actor;
    part_t *first = owner->actor_parts_list;
    if (part == first) {
        owner->actor_parts_list = first->next;
    } else {
        for (part_t *fp = first; fp; fp = fp->next_in_display_list) {
            if (part == fp->next) {
                fp->next = fp->next->next;
                break;
            }
            if (part == (part_t *)fp->actor_parts_list) {
                fp->actor_parts_list = ((part_t *)fp->actor_parts_list)->next;
                break;
            }
        }
    }

    delete_parts(part);
}

/* menu_play_dead_scene  E1: 0x430388 | E2: 0x43A660 */
void play_dead_scene(int sceneIndex) {
    stop_the_clock = true;
    break_do_movement = 1;
    stop_samples();
    initialise_game();

    check_scene_loaded((int16_t)sceneIndex);
    scene_t *scene = scene_tab[sceneIndex];
    if (!scene) return;

    active_camera = NULL;
    check_actors_in_scene_loaded(scene);

    script_t *first_script = scene->scene_script_list;
    if (first_script) {
        int16_t actorIndex = first_script->script_actor_index;
        if (actorIndex >= 0 && actorIndex < THING_TAB_SIZE) {
            selected_thing = thing_tab[actorIndex];
        }
    }

    start_scene(scene);
}

