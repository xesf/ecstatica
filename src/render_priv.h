/**
 * render_priv.h
 *
 * Shared by render.c and the backend only; not engine-facing.
 */

#ifndef RENDER_PRIV_H
#define RENDER_PRIV_H

#ifdef ECS_ENABLE_GL

#include "types.h"
#include "platform.h"

/* view_transform's near-clip test; depth fits the engine's int16 mask. */
#define RENDER_NEAR_Z   128
#define RENDER_FAR_Z    32767

enum {
    RENDER_ELL_SOLID = 0,
    RENDER_ELL_SHADOW,
    RENDER_ELL_SMOKE,
    RENDER_ELL_BEAM
};

/* One ellipsoid in view space: p = centre + rot * diag(axes) * u. `rot` is
 * matrix_2, which already includes the view rotation. */
typedef struct {
    float   centre[3];
    float   rot[9];          /* row-major 3x3 */
    float   axes[3];         /* semi-axes, world units */
    float   radius;          /* max(axes), for the bounding quad */
    uint8_t colour;          /* shade_tab first index, 0..16 */
    uint8_t mode;            /* RENDER_ELL_*  */
    int16_t colour_shade;    /* 14-bit fixed multiplier for the fog band */
} render_ellipsoid_t;

/* One triangle corner in world space. `pal` is the CPU-shaded palette index;
 * `layer` the texture array slot, or -1 for flat. */
typedef struct {
    float   pos[3];
    float   uv[2];           /* texel units; wrapped at 0x7F like asm_f.c:456 */
    uint8_t pal;
    int16_t layer;
} render_vertex_t;

/* 3D map vertex. Untextured faces carry an RGB colour; textured ones (layer
 * >= 0, E2's face textures) a grey shade the palette colour is scaled by. */
typedef struct {
    float   pos[3];
    float   uv[2];           /* texel units, wrapped at 128 */
    uint8_t rgb[3];
    uint8_t pad;
    int32_t layer;
} render_map_vertex_t;

typedef struct {
    float view[16];
    float proj[16];
    int   moving_camera;

    const render_ellipsoid_t *ellipsoids;
    int                       ellipsoid_count;
    const render_vertex_t    *flat_verts;
    int                       flat_count;
    const render_vertex_t    *tex_verts;
    int                       tex_count;
    bool                      have_3d;

    const render_map_vertex_t *map_verts;
    int                        map_count;
} render_frame_t;

extern render_frame_t render_frame;

/* ── Backend interface ────────────────────────────────────── */

bool render_gl_init(platform_t *p);      /* create context, load GL, compile */
void render_gl_shutdown(void);
bool render_gl_start(void);              /* become the active renderer */
void render_gl_stop(void);

void render_gl_frame_begin(void);
void render_gl_frame_end(void);

void render_gl_invalidate_background(void);
void render_gl_invalidate_palette(void);

/* Texture array slot, uploaded on first use. -1 when the texture is not loaded
 * (the caller draws flat). */
int  render_gl_texture_layer(int16_t texture_name_index);

#endif /* ECS_ENABLE_GL */
#endif /* RENDER_PRIV_H */
