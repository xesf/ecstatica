#ifndef ICON_H
#define ICON_H

#include "types.h"

extern int16_t rate_box_left;
extern int16_t rate_box_top;
extern int16_t vector_box_left;
extern int16_t vector_box_top;
extern int16_t max_message_len;

/* Port setting, on by default: widen the 640x480 mode to WIDE_SCREEN_WIDTH.
 * Only takes effect where widescreen_active() says the data (the re-release's
 * wide views) and the build support it, so the original releases stay 4:3. */
extern int widescreen;
int widescreen_active(void);

/* Columns either side of the original 640 in the widescreen mode, 0 otherwise.
 * 640-wide art and coordinates are shifted right by this to stay centred. */
int wide_margin(void);

/* Reads a full-screen 640x480 image into `plane`, centred on a wide screen
 * with index 0 either side. */
void read_screen_image(FILE *f, char *plane);

void set_vga_constants(void);
void set_svga_constants(void);

#endif /* ICON_H */
