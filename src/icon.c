/**
 * icon.c
 *
 * Resolution constants: set_vga_constants (320×200), set_svga_constants (640×480).
 */

#include "icon.h"
#include "display.h"
#include "win.h"
#include "asm_f.h"
#include "compat.h"
#include <stdio.h>
#include <string.h>

int16_t rate_box_left = 10;
int16_t rate_box_top = 20;
int16_t vector_box_left = 242;
int16_t vector_box_top = 276;
int16_t max_message_len = 52;
int aspect_ratio = ASPECT_16_9;

static const struct {
    const char *name;
    int16_t width;
} aspect_ratios[] = {
    { "4:3", 640 }, { "16:10", 768 }, { "16:9", 854 }, { "21:9", 1120 }, { "32:9", 1706 },
};

#define NUM_ASPECT_RATIOS ((int)(sizeof(aspect_ratios) / sizeof(aspect_ratios[0])))

int aspect_ratio_count(void) {
    return NUM_ASPECT_RATIOS;
}

const char *aspect_ratio_name(int index) {
    return (index >= 0 && index < NUM_ASPECT_RATIOS) ? aspect_ratios[index].name : "";
}

int aspect_ratio_from_name(const char *name) {
    for (int i = 0; i < NUM_ASPECT_RATIOS; i++)
        if (strcmp(name, aspect_ratios[i].name) == 0) return i;
    return -1;
}

int aspect_ratio_available(int index) {
    if (index == ASPECT_4_3) return 1;
    if (index < 0 || index >= NUM_ASPECT_RATIOS) return 0;
#ifdef ECS_WIDESCREEN
    int width = aspect_ratios[index].width;
    return width <= WIDE_SCREEN_MAX_WIDTH && width <= file_wide_view_reach();
#else
    return 0;
#endif
}

int aspect_ratio_in_use(void) {
    int index = aspect_ratio < NUM_ASPECT_RATIOS ? aspect_ratio : NUM_ASPECT_RATIOS - 1;
    while (index > ASPECT_4_3 && !aspect_ratio_available(index)) index--;
    return index;
}

int wide_margin(void) {
    return (screen_height == 480 && screen_width > 640) ? (screen_width - 640) / 2 : 0;
}

void read_screen_image(FILE *f, char *plane) {
    int margin = wide_margin();
    if (!margin) {
        fread_ignore(plane, 1, screen_width * screen_height, f);
        return;
    }
    memset(plane, 0, (size_t)screen_width * screen_height);
    for (int y = 0; y < screen_height; y++)
        fread_ignore(plane + y * screen_width + margin, 1, 640, f);
}

/* icon_set_vga_constants  E1: 0x42F050 | E2: 0x4391A0 */
void set_vga_constants(void) {
    screen_width    = 320;
    screen_height   = 200;
    hires_width     = 320;
    hires_height    = 200;
    right_edge      = 320;
    bottom_edge     = 200;
    screen_centre_x = 160;
    screen_centre_y = 100;
    rate_box_left   = 10;
    rate_box_top    = 20;
    vector_box_top  = 114;
    max_message_len = 26;
    vector_box_left = 121;
    win_set_render_size(screen_width, screen_height);
}

/* icon_set_svga_constants  E1: 0x42F238 | E2: 0x439388 */
void set_svga_constants(void) {
    int16_t width = aspect_ratios[aspect_ratio_in_use()].width;
    screen_width    = width;
    screen_height   = 480;
    hires_width     = width;
    hires_height    = 480;
    right_edge      = width;
    bottom_edge     = 480;
    screen_centre_x = width / 2;
    screen_centre_y = 240;
    rate_box_left   = 10;
    rate_box_top    = 20;
    max_message_len = 52;
    vector_box_top  = 276;
    vector_box_left = 242;
    win_set_render_size(screen_width, screen_height);
}
