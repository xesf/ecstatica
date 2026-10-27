#ifndef ICON_H
#define ICON_H

#include "types.h"

extern int16_t rate_box_left;
extern int16_t rate_box_top;
extern int16_t vector_box_left;
extern int16_t vector_box_top;
extern int16_t max_message_len;

/* Port setting: the shape of the 640x480 mode, an index into the aspect
 * ratios (4:3, 16:10, 16:9, 21:9, 32:9), 16:9 by default. A wider one only
 * takes effect where the data (the re-release's wide views) reaches that far
 * and the build allows it; otherwise the next narrower one does, so the
 * original releases stay 4:3. */
#define ASPECT_4_3  0
#define ASPECT_16_9 2
extern int aspect_ratio;
int aspect_ratio_count(void);
const char *aspect_ratio_name(int index);
/* Index of "16:9" style names, or -1. */
int aspect_ratio_from_name(const char *name);
int aspect_ratio_available(int index);
/* The aspect ratio the setting gives with this data and build. */
int aspect_ratio_in_use(void);

/* Columns either side of the original 640 in the widescreen mode, 0 otherwise.
 * 640-wide art and coordinates are shifted right by this to stay centred. */
int wide_margin(void);

/* Reads a full-screen 640x480 image into `plane`, centred on a wide screen
 * with index 0 either side. */
void read_screen_image(FILE *f, char *plane);

void set_vga_constants(void);
void set_svga_constants(void);

#endif /* ICON_H */
