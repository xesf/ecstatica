/**
 * render.c
 *
 * Backend-independent half of the hardware renderer: builds the draw lists,
 * does the per-face shading on the CPU so it matches the software path, and
 * derives view/projection from the engine's camera. Backends (render_gl.c)
 * consume only these lists.
 */

#include "render.h"

/* The preferences exist on every target so file.c round-trips the config even
 * where no backend can apply them. */
#ifdef ECS_ENABLE_GL
render_backend_t render_backend = RENDER_SOFTWARE;
#endif

int16_t render_hardware_pref  = 0;
int16_t render_supersample    = 0;   /* 0 = auto, match the drawable */
int16_t render_enhanced_light = 0;
int16_t render_map3d          = 0;   /* 0 = pre-rendered, 1 = 3D map */

#ifdef ECS_ENABLE_GL

#include "render_priv.h"
#include "display.h"
#include "ellipse.h"
#include "game.h"
#include "init.h"
#include "map.h"
#include "topo.h"
#include "win.h"
#include "platform.h"
#include "file.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static bool s_available   = false;   /* a backend came up at least once */
static bool s_frame_begun = false;   /* 3D content was started this frame */

/* ── Draw lists ───────────────────────────────────────────────
 * Sized from the engine's own pools, so they cannot overflow: one ellipsoid
 * per part, and quads split into two triangles hence the doubling.
 */
static render_ellipsoid_t s_ellipsoids[PART_POOL_SIZE];
static int                s_ellipsoid_count;
static render_vertex_t    s_flat_verts[TRI_SIZE * 2 * 3];
static int                s_flat_count;
static render_vertex_t    s_tex_verts[TRI_SIZE * 2 * 3];
static int                s_tex_count;

render_frame_t render_frame;

/* ── Fixed-point helpers ──────────────────────────────────── */

static inline float fx(int v) { return (float)v / (float)FIXED_POINT_ONE; }

/* ── Matrices ─────────────────────────────────────────────────
 * Column-major 4x4, the layout glUniformMatrix4fv wants with transpose=GL_FALSE.
 */

static void mat4_identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* The engine computes out = view_matrix * (world - view_pos) in 14-bit fixed
 * point: a rotation about the camera, with the translation in the last column. */
static void build_view_matrix(float *m) {
    const matrix3x3_t *v = &view_matrix;
    float r[9];
    r[0] = fx(v->_11); r[1] = fx(v->_12); r[2] = fx(v->_13);
    r[3] = fx(v->_21); r[4] = fx(v->_22); r[5] = fx(v->_23);
    r[6] = fx(v->_31); r[7] = fx(v->_32); r[8] = fx(v->_33);

    float px = (float)view_pos.X, py = (float)view_pos.Y, pz = (float)view_pos.Z;

    mat4_identity(m);
    /* column-major: m[col*4 + row] */
    m[0] = r[0]; m[4] = r[1]; m[8]  = r[2];
    m[1] = r[3]; m[5] = r[4]; m[9]  = r[5];
    m[2] = r[6]; m[6] = r[7]; m[10] = r[8];

    m[12] = -(r[0] * px + r[1] * py + r[2] * pz);
    m[13] = -(r[3] * px + r[4] * py + r[5] * pz);
    m[14] = -(r[6] * px + r[7] * py + r[8] * pz);
    m[15] = 1.0f;
}

/**
 * Projection, derived from perspective_transform (display.c):
 *
 *   coeff_x    = zoom_factor / z,  scaled by screen_width/320
 *   coeff_y    = coeff_x * 7/8,    scaled by screen_height/200
 *   pixel_x    = centre_x + (coeff_x * X >> 10) >> 4
 *
 * so  pixel_x - centre_x = kx * X / z  with  kx = zoom * (sw/320) / 16384.
 *
 * Depth is the ordinary perspective mapping: a linear z_ndc would need z² with
 * w = z. The background shader converts its linear int16 depth per pixel, so
 * everything else keeps interpolated depth and early-Z. Engine +Y is down and
 * +Z into the screen; both are baked in here.
 */
static void build_proj_matrix(float *m) {
    const float near_z = (float)RENDER_NEAR_Z;
    const float far_z  = (float)RENDER_FAR_Z;

    float sw = (float)screen_width;
    float sh = (float)screen_height;

    float kx = (float)zoom_factor * (sw / 320.0f) / 16384.0f;
    float ky = (float)zoom_factor * 0.875f * (sh / 200.0f) / 16384.0f;

    memset(m, 0, 16 * sizeof(float));
    m[0]  =  2.0f * kx / sw;
    m[5]  = -2.0f * ky / sh;               /* engine +Y is down */
    m[10] =  (far_z + near_z) / (far_z - near_z);
    m[14] = -2.0f * far_z * near_z / (far_z - near_z);
    m[11] =  1.0f;                         /* engine +Z is forward */
}

/* ── Per-face shading ─────────────────────────────────────────
 * From draw_triangle_ell (ellipse.c), so the hardware path picks the same
 * palette entry as software, backface colour swap included. Returns 0 when the
 * face is culled.
 */
static int face_palette_index(tri_t *tri, tri_t *shade, uint8_t *out_index) {
    point_t *p1 = tri->point1, *p2 = tri->point2, *p3 = tri->point3;
    if (!p1 || !p2 || !p3) return 0;

    int x1 = p1->screen_coord.X, y1 = p1->screen_coord.Y, z1 = p1->screen_coord.Z;
    int x2 = p2->screen_coord.X, y2 = p2->screen_coord.Y, z2 = p2->screen_coord.Z;
    int x3 = p3->screen_coord.X, y3 = p3->screen_coord.Y, z3 = p3->screen_coord.Z;

    /* Z 0 marks a near-clipped vertex. GL clips the geometry, so use the
     * unshaded face colour rather than dividing by zero below. */
    int near_clipped = (!z1 || !z2 || !z3);

    int cross_x = (z2 - z3) * (y1 - y3) - (z1 - z3) * (y2 - y3);
    int cross_z = (y2 - y3) * (x1 - x3) - (y1 - y3) * (x2 - x3);
    int cross_y = (x2 - x3) * (z1 - z3) - (z2 - z3) * (x1 - x3);
    int tri_color;
    int ref_z = z1;

    if (cross_z <= 0) {
        if (!(tri->tri_use_flag & 1)) return 0;      /* single-sided: culled */
        cross_x = -cross_x;
        cross_y = (z2 - z3) * (x1 - x3) - (x2 - x3) * (z1 - z3);
        tri_color = tri->tri_color_4;
        cross_z = -cross_z;
    } else {
        tri_color = tri->tri_color_3;
    }

    if (shade && shade->point1 && shade->point2 && shade->point3) {
        int sx1 = shade->point1->screen_coord.X, sy1 = shade->point1->screen_coord.Y, sz1 = shade->point1->screen_coord.Z;
        int sx2 = shade->point2->screen_coord.X, sy2 = shade->point2->screen_coord.Y, sz2 = shade->point2->screen_coord.Z;
        int sx3 = shade->point3->screen_coord.X, sy3 = shade->point3->screen_coord.Y, sz3 = shade->point3->screen_coord.Z;

        cross_x = (sz2 - sz3) * (sy1 - sy3) - (sz1 - sz3) * (sy2 - sy3);
        cross_z = (sy2 - sy3) * (sx1 - sx3) - (sy1 - sy3) * (sx2 - sx3);
        cross_y = (sx2 - sx3) * (sz1 - sz3) - (sz2 - sz3) * (sx1 - sx3);
        ref_z = sz1;
    }

    if (tri_color < 0) tri_color = 0;
    if (tri_color > 16) tri_color = 16;

    int shade_idx = 64;
    if (!near_clipped) {
        while (cross_x < -0x4000 || cross_x > 0x4000 || cross_y < -0x4000 || cross_y > 0x4000 ||
               cross_z < -0x4000 || cross_z > 0x4000) {
            cross_x >>= 1; cross_y >>= 1; cross_z >>= 1;
        }

        vector_t normal;
        normal.X = (int16_t)cross_x; normal.Y = (int16_t)cross_y; normal.Z = (int16_t)cross_z;

        int16_t rot_y = arctan(normal.X, normal.Z);
        if (rot_y) rotate_vector_about_y(&normal, -rot_y);
        int16_t rot_x = arctan(-normal.Y, normal.Z);
        if (rot_x) rotate_vector_about_x(&normal, -rot_x);

        int shade_col, shade_row;
        if (normal.Z) {
            shade_col = -64 * cross_x / normal.Z + 64;
            if (shade_col < 0) shade_col = 0;
            if (shade_col > 127) shade_col = 127;
            shade_row = -64 * cross_y / normal.Z + 64;
            if (shade_row < 0) shade_row = 0;
            if (shade_row > 127) shade_row = 127;
        } else {
            shade_col = 64; shade_row = 64;
        }
        shade_idx = shade_map[shade_row][shade_col] & 0x7F;
    }

    int depth_shade = moving_camera ? (159 - (ref_z >> 5)) : (191 - (ref_z >> 7));
    if (depth_shade < 0) depth_shade = 0;
    if (depth_shade > 127) depth_shade = 127;

    int band = (tri->tri_use_flag & 0x10)
             ? 127
             : (tri->shade_multiplier * depth_shade >> 14);
    if (band < 0) band = 0;
    if (band > 127) band = 127;

    *out_index = (uint8_t)shade_tab[tri_color][band][shade_idx];
    return 1;
}

/* ── Triangle submission ──────────────────────────────────── */

static void push_vertex(render_vertex_t *dst, const point_t *p,
                        int u, int v, uint8_t pal, int layer) {
    dst->pos[0] = (float)p->world_position.X;
    dst->pos[1] = (float)p->world_position.Y;
    dst->pos[2] = (float)p->world_position.Z;
    dst->uv[0]  = (float)u;
    dst->uv[1]  = (float)v;
    dst->pal    = pal;
    dst->layer  = (int16_t)layer;
}

/* `uv` holds the six texel coordinates in draw_new_tex_tri's field order. */
static void emit_triangle(tri_t *tri, tri_t *shade, const int *uv) {
    uint8_t pal;
    if (!face_palette_index(tri, shade, &pal)) return;

    int layer = -1;
    if (tri->texture_name_index >= 0)
        layer = render_gl_texture_layer(tri->texture_name_index);

    if (layer >= 0) {
        if (s_tex_count + 3 > (int)(sizeof(s_tex_verts) / sizeof(s_tex_verts[0]))) return;
        render_vertex_t *d = &s_tex_verts[s_tex_count];
        push_vertex(&d[0], tri->point1, uv[0], uv[1], pal, layer);
        push_vertex(&d[1], tri->point2, uv[2], uv[3], pal, layer);
        push_vertex(&d[2], tri->point3, uv[4], uv[5], pal, layer);
        s_tex_count += 3;
    } else {
        if (s_flat_count + 3 > (int)(sizeof(s_flat_verts) / sizeof(s_flat_verts[0]))) return;
        render_vertex_t *d = &s_flat_verts[s_flat_count];
        push_vertex(&d[0], tri->point1, 0, 0, pal, -1);
        push_vertex(&d[1], tri->point2, 0, 0, pal, -1);
        push_vertex(&d[2], tri->point3, 0, 0, pal, -1);
        s_flat_count += 3;
    }
}

/* As draw_polygon (tri.c): a quad is two triangles, the second from point3 +
 * quad_point4 with the same texture field shuffle. GL does the near clipping. */
void render_triangle(tri_t *tri, int plane, tri_t *shade) {
    (void)plane;
    if (!tri) return;

    int uv[6];
    uv[0] = tri->u1; uv[1] = tri->v1;
    uv[2] = tri->u2; uv[3] = tri->v2;
    uv[4] = tri->u3; uv[5] = tri->v3;

    if (!tri->quad_point4) {
        emit_triangle(tri, shade, uv);
        return;
    }

    emit_triangle(tri, shade, uv);

    tri_t second = *tri;
    second.point1      = tri->point3;
    second.point3      = tri->quad_point4;
    second.quad_point4 = NULL;
    second.tri_use_flag &= 0xFEBF;
    if (tri->tri_use_flag & 0x0100) second.tri_use_flag |= 0x40;
    if (tri->tri_use_flag & 0x0200) second.tri_use_flag |= 0x0100;

    int uv2[6];
    uv2[0] = tri->u3; uv2[1] = tri->v3;   /* old p3 becomes p1 */
    uv2[2] = tri->u2; uv2[3] = tri->v2;   /* p2 unchanged      */
    uv2[4] = tri->u4; uv2[5] = tri->v4;   /* p4                */
    emit_triangle(&second, shade, uv2);
}

/* ── Ellipsoid submission ─────────────────────────────────── */

/* By put_an_ellipse's leaf, matrix_2 is view_matrix * matrix_1, VECTOR_Squash
 * holds the semi-axes and persp_origin the view-space centre X/Y. The ellipsoid
 * p = centre + R * diag(squash) * u, |u| = 1, is ray-traced per pixel. */
void render_ellipsoid(part_t *part, int plane) {
    (void)plane;
    if (!part) return;
    if (s_ellipsoid_count >= PART_POOL_SIZE) return;

    int z = part->vector_persp.Z;
    if (z <= 0) return;                     /* near-clipped by the engine */

    int sx = part->VECTOR_Squash.X;
    int sy = part->VECTOR_Squash.Y;
    int sz = part->VECTOR_Squash.Z;
    if (!sx || !sy || !sz) return;          /* degenerate: pivot helper geometry */

    render_ellipsoid_t *e = &s_ellipsoids[s_ellipsoid_count];

    e->centre[0] = (float)part->persp_origin.X;
    e->centre[1] = (float)part->persp_origin.Y;
    e->centre[2] = (float)z;

    const matrix3x3_t *m = &part->matrix_2;
    e->rot[0] = fx(m->_11); e->rot[1] = fx(m->_12); e->rot[2] = fx(m->_13);
    e->rot[3] = fx(m->_21); e->rot[4] = fx(m->_22); e->rot[5] = fx(m->_23);
    e->rot[6] = fx(m->_31); e->rot[7] = fx(m->_32); e->rot[8] = fx(m->_33);

    e->axes[0] = (float)abs(sx);
    e->axes[1] = (float)abs(sy);
    e->axes[2] = (float)abs(sz);

    float r = e->axes[0];
    if (e->axes[1] > r) r = e->axes[1];
    if (e->axes[2] > r) r = e->axes[2];
    e->radius = r;

    int colour = part->color;
    if (colour < 0) colour = 0;
    if (colour > 16) colour = 16;
    e->colour = (uint8_t)colour;

    int cshade = part->color_shade;
    if (cshade < 0) cshade = 0;
    if (cshade > FIXED_POINT_ONE) cshade = FIXED_POINT_ONE;
    e->colour_shade = (int16_t)cshade;

    /* ellipse.c:119 — the 0x42 pair selects the three destination-modulating
     * modes; everything else is an ordinary lit surface. */
    int f = part->flags & 0x42;
    if (f == 0x42)      e->mode = RENDER_ELL_BEAM;
    else if (f == 0x02) e->mode = RENDER_ELL_SHADOW;
    else if (f == 0x40) e->mode = RENDER_ELL_SMOKE;
    else                e->mode = RENDER_ELL_SOLID;

    s_ellipsoid_count++;
}

/* ── Debug map geometry ───────────────────────────────────────
 * The collision map drawn as geometry from the game camera, in place of the
 * pre-rendered background. Grid to world, the inverse of find_map_element:
 *   world X = (col - 64) << 9,  world Z = (row - 64) << 9,  cell = 512 units
 *   world Y = (128 - def_height) << height_shift
 * Rebuilt when top_of_map_elements moves (a new area was merged in).
 */
#define MAP_GRID   128
#define MAP_CELL   512

/* Only around the camera: distant cells pile into a wall that hides the view. */
#define MAP_VIEW_RADIUS 26

static render_map_vertex_t *s_map_verts;
static int  s_map_count;
static int  s_map_cap;
static int  s_map_built_for = -1;
static int  s_map_centre_row = -9999, s_map_centre_col = -9999;

static void map_push_tex(float x, float y, float z, const uint8_t rgb[3],
                         float tu, float tv, int layer) {
    if (s_map_count >= s_map_cap) {
        int cap = s_map_cap ? s_map_cap * 2 : 65536;
        render_map_vertex_t *g = (render_map_vertex_t *)realloc(s_map_verts,
                                     (size_t)cap * sizeof(*g));
        if (!g) return;
        s_map_verts = g;
        s_map_cap   = cap;
    }
    render_map_vertex_t *v = &s_map_verts[s_map_count++];
    v->pos[0] = x; v->pos[1] = y; v->pos[2] = z;
    v->uv[0] = tu; v->uv[1] = tv;
    v->rgb[0] = rgb[0]; v->rgb[1] = rgb[1]; v->rgb[2] = rgb[2];
    v->pad = 0;
    v->layer = layer;
}

static void map_push(float x, float y, float z, const uint8_t rgb[3]) {
    map_push_tex(x, y, z, rgb, 0.0f, 0.0f, -1);
}

/* Average top-face colour of each material, measured from the pre-rendered
 * views: the --pointcloud reconstruction puts every background pixel back in
 * the world, and the ones resting on a block's top are averaged per material.
 * Unmeasured materials stay grey. */
static const uint8_t e1_material_rgb[][3] = {
    {  79, 51, 23 }, {  71, 76, 61 }, {  39, 30, 14 }, {  68, 62, 25 },
    {  36, 34, 32 }, {  82, 51, 23 }, {  99, 78, 60 }, {  17, 17, 18 },
    {  60, 60, 60 }, {  36, 25,  8 }, {  42, 46, 17 },
};
static const uint8_t e2_material_rgb[][3] = {
    {  94, 37, 14 }, {  60, 60, 60 }, {  98, 38,  9 }, {  90, 47, 19 },
    {  54, 39, 52 }, {  60, 60, 60 }, {  69, 27, 15 }, {  60, 60, 60 },
    {  23, 17, 20 }, {  71, 31, 18 }, {  72, 40,  9 }, {  60, 60, 60 },
    { 111, 59, 21 }, {  77, 37, 27 }, {  87, 34,  6 }, {  93, 42,  0 },
    {  98, 56, 53 }, {  88, 30, 11 }, {  90, 41, 24 }, {  60, 29, 24 },
    {  60, 60, 60 }, {  81, 24,  3 }, { 131, 52, 10 }, {  60, 60, 60 },
    {  60, 60, 60 }, {  60, 60, 60 }, {  60, 60, 60 }, {  10,  0,  0 },
    {   5,  4,  6 }, {  66, 27, 15 },
};

/* Measured colours are averages over lit and shaded texels; lift them. */
static void material_colour(int material, float shade, uint8_t out[3]) {
    const uint8_t (*tab)[3] = (game_version == GAME_VERSION_E1) ? e1_material_rgb : e2_material_rgb;
    int n = (game_version == GAME_VERSION_E1)
          ? (int)(sizeof(e1_material_rgb) / sizeof(e1_material_rgb[0]))
          : (int)(sizeof(e2_material_rgb) / sizeof(e2_material_rgb[0]));
    const uint8_t grey[3] = { 60, 60, 60 };
    const uint8_t *c = (material >= 0 && material < n) ? tab[material] : grey;
    for (int i = 0; i < 3; i++) {
        float v = c[i] * 1.8f * shade;
        out[i] = (uint8_t)(v > 255.0f ? 255.0f : v);
    }
}

/* A footprint piece in cell-local units (0..512 on x and z), as a convex
 * polygon. block_config 1 is the whole cell, 2-5 the four diagonal halves, and
 * from 6 up a mask of quarters (bit 1 +x-z, 2 -x-z, 4 +x+z, 8 -x+z), as
 * find_map_element reads them. */
typedef struct { int n; float x[4], z[4]; } map_poly_t;

static int map_footprint(int cfg, map_poly_t out[4]) {
    static const float sq[4][2] = { {0,0}, {512,0}, {512,512}, {0,512} };
    int n = 0;
    if (cfg == 1) {
        out[0].n = 4;
        for (int i = 0; i < 4; i++) { out[0].x[i] = sq[i][0]; out[0].z[i] = sq[i][1]; }
        return 1;
    }
    if (cfg >= 2 && cfg <= 5) {
        /* The corner each half leaves out. */
        static const int skip[6] = { 0, 0, 3, 2, 0, 1 };
        out[0].n = 3;
        for (int i = 0, k = 0; i < 4; i++) {
            if (i == skip[cfg]) continue;
            out[0].x[k] = sq[i][0]; out[0].z[k] = sq[i][1]; k++;
        }
        return 1;
    }
    static const struct { int bit; float x0, z0; } quarters[4] = {
        { 1, 256, 0 }, { 2, 0, 0 }, { 4, 256, 256 }, { 8, 0, 256 },
    };
    for (int q = 0; q < 4; q++) {
        if (!(cfg & quarters[q].bit)) continue;
        map_poly_t *p = &out[n++];
        p->n = 4;
        p->x[0] = quarters[q].x0;       p->z[0] = quarters[q].z0;
        p->x[1] = quarters[q].x0 + 256; p->z[1] = quarters[q].z0;
        p->x[2] = quarters[q].x0 + 256; p->z[2] = quarters[q].z0 + 256;
        p->x[3] = quarters[q].x0;       p->z[3] = quarters[q].z0 + 256;
    }
    return n;
}

/* E2 face textures: slot by face direction, as sub_140146430 picks it. The
 * texture layer, or -1 when the face is drawn from its material. */
enum { SLOT_NEG_Z, SLOT_POS_Z, SLOT_NEG_X, SLOT_POS_X, SLOT_TOP, SLOT_BOTTOM, SLOT_DIAG };

static int map_face_layer(int idx, int slot) {
#ifdef ECS_ENABLE_GL
    if (game_version != GAME_VERSION_E2) return -1;
    int16_t tex = map_element_tex[idx][slot];
    if (tex < 0 || tex >= TEXTURE_TAB_SIZE) return -1;
    check_texture_loaded(tex);
    return render_gl_texture_layer(tex);
#else
    (void)idx; (void)slot;
    return -1;
#endif
}

/* Texture scale: 128 texels to a 512-unit cell. */
#define MAP_TEXEL 4.0f

/* One map element as a solid prism: its footprint from height2 up to
 * def_height. Ground blocks (height2 0) only get a short skirt; the rest of
 * their column is never seen. */
static void map_prism(float cx, float cz, int idx, const map_area_element_t *e) {
    map_poly_t polys[4];
    int np = map_footprint(e->block_config, polys);
    if (!np) return;

    float ytop = (float)((128 - (int)e->def_height) << height_shift);
    int bottom = e->height2 ? (int)e->height2 : (int)e->def_height - 24;
    float ybot = (float)((128 - bottom) << height_shift);

    uint8_t top[3], side_x[3], side_z[3], under[3];
    material_colour(e->material, 1.0f, top);
    material_colour(e->material, 0.75f, side_x);
    material_colour(e->material, 0.6f, side_z);
    material_colour(e->material, 0.4f, under);
    static const uint8_t lit[3] = { 255, 255, 255 }, lit_x[3] = { 200, 200, 200 },
                         lit_z[3] = { 165, 165, 165 }, lit_under[3] = { 110, 110, 110 };

    int top_layer = map_face_layer(idx, SLOT_TOP);
    int bot_layer = e->height2 ? map_face_layer(idx, SLOT_BOTTOM) : -1;

    for (int i = 0; i < np; i++) {
        const map_poly_t *p = &polys[i];
        for (int k = 1; k + 1 < p->n; k++) {
            const int t3[3] = { 0, k, k + 1 };
            const uint8_t *tc = top_layer >= 0 ? lit : top;
            for (int m = 0; m < 3; m++) {
                float x = cx + p->x[t3[m]], z = cz + p->z[t3[m]];
                map_push_tex(x, ytop, z, tc, x / MAP_TEXEL, z / MAP_TEXEL, top_layer);
            }
            if (e->height2) {
                const int b3[3] = { 0, k + 1, k };
                const uint8_t *bc = bot_layer >= 0 ? lit_under : under;
                for (int m = 0; m < 3; m++) {
                    float x = cx + p->x[b3[m]], z = cz + p->z[b3[m]];
                    map_push_tex(x, ybot, z, bc, x / MAP_TEXEL, z / MAP_TEXEL, bot_layer);
                }
            }
        }
        for (int k = 0; k < p->n; k++) {
            int j = (k + 1) % p->n;
            float x0 = cx + p->x[k], z0 = cz + p->z[k];
            float x1 = cx + p->x[j], z1 = cz + p->z[j];
            /* Outward normal of a counter-clockwise footprint edge. */
            float nx = z1 - z0, nz = -(x1 - x0);
            int slot = (nx == 0.0f) ? (nz > 0 ? SLOT_POS_Z : SLOT_NEG_Z)
                     : (nz == 0.0f) ? (nx > 0 ? SLOT_POS_X : SLOT_NEG_X) : SLOT_DIAG;
            int layer = map_face_layer(idx, slot);
            int along_x = fabsf(x1 - x0) > fabsf(z1 - z0);
            const uint8_t *c = layer >= 0 ? (along_x ? lit_z : lit_x) : (along_x ? side_z : side_x);
            float u0 = (along_x ? x0 : z0) / MAP_TEXEL, u1 = (along_x ? x1 : z1) / MAP_TEXEL;
            float vt = ytop / MAP_TEXEL, vb = ybot / MAP_TEXEL;
            map_push_tex(x0, ytop, z0, c, u0, vt, layer);
            map_push_tex(x1, ytop, z1, c, u1, vt, layer);
            map_push_tex(x1, ybot, z1, c, u1, vb, layer);
            map_push_tex(x0, ytop, z0, c, u0, vt, layer);
            map_push_tex(x1, ybot, z1, c, u1, vb, layer);
            map_push_tex(x0, ybot, z0, c, u0, vb, layer);
        }
    }
}

/* Thatch, measured from E1's roofs in the pre-rendered views. */
static const uint8_t roof_rgb[3] = { 190, 120, 55 };

static void roof_colour(float shade, uint8_t out[3]) {
    for (int i = 0; i < 3; i++) {
        float v = roof_rgb[i] * shade;
        out[i] = (uint8_t)(v > 255.0f ? 255.0f : v);
    }
}

static void map_tri(const float a[3], const float b[3], const float c[3], const uint8_t rgb[3]) {
    map_push(a[0], a[1], a[2], rgb);
    map_push(b[0], b[1], b[2], rgb);
    map_push(c[0], c[1], c[2], rgb);
}

/* DrawRoof dir dx dz rows width: a pitched roof standing on the element, as
 * the re-release builds it (Ecstatica II.exe, sub_14013A1E0). The ridge runs
 * along dir (45 degree steps) for rows x 256 units; the roof is width x 256
 * across, eaves on both sides, ridge in the middle, 45 degrees (checked
 * against the thatch in the pre-rendered views). The eaves stand on the
 * element's top; dx and dz move the roof in 256-unit steps. The re-release covers the slopes in rows of
 * thatch ellipsoids; here they are two flat slopes and two gable ends. */
static void map_roof(float bx, float by, float bz, const int p[5]) {
    double a = (double)(p[0] & 7) * (3.14159265358979 / 4.0);
    float dir[2]  = { (float)cos(a), (float)sin(a) };
    float perp[2] = { -dir[1], dir[0] };
    float len = (float)(p[3] * 256);
    float wid = (float)(p[4] * 256);
    float rise = (float)(p[4] * 128);

    float ox = bx + (float)((int8_t)p[1] * 256);
    float oz = bz + (float)((int8_t)p[2] * 256);
    float e0[2][3], ridge[2][3], e1[2][3];
    for (int k = 0; k < 2; k++) {
        float t = k ? len : 0.0f;
        float x = ox + dir[0] * t, z = oz + dir[1] * t;
        e0[k][0] = x;                        e0[k][1] = by;        e0[k][2] = z;
        ridge[k][0] = x + perp[0] * wid / 2; ridge[k][1] = by - rise;
        ridge[k][2] = z + perp[1] * wid / 2;
        e1[k][0] = x + perp[0] * wid;        e1[k][1] = by;        e1[k][2] = z + perp[1] * wid;
    }
    uint8_t slope_a[3], slope_b[3], gable[3];
    roof_colour(1.0f, slope_a);
    roof_colour(0.8f, slope_b);
    roof_colour(0.6f, gable);
    map_tri(e0[0], e0[1], ridge[1], slope_a);  map_tri(e0[0], ridge[1], ridge[0], slope_a);
    map_tri(e1[0], ridge[0], ridge[1], slope_b); map_tri(e1[0], ridge[1], e1[1], slope_b);
    map_tri(e0[0], ridge[0], e1[0], gable);
    map_tri(e0[1], e1[1], ridge[1], gable);
}

/* Roofs come from the element's second code: the original one-line
 * DrawRoof scripts (token CT_DRAW_ROOF and five values). */
static void map_element_roofs(int row, int col, int idx, const map_area_element_t *e) {
    int code = map_element_code2[idx];
    if (code <= 0 || code - 1 >= CODE_TAB_SIZE) return;
    const code_t *c = code_tab[code - 1];
    if (!c || !token_store) return;
    const int16_t *t = &token_store[c->token_store_index];
    for (int i = 0; i < 64 && t[i]; i++) {
        if (t[i] != CT_DRAW_ROOF) continue;
        int p[5];
        for (int k = 0; k < 5; k++) {
            int v = t[i + 1 + k] & 0x0FFF;
            p[k] = (v & 0x800) ? v - 0x1000 : v;
        }
        float bx = (float)((col - 64) * MAP_CELL);
        float bz = (float)((row - 64) * MAP_CELL);
        float by = (float)((128 - (int)e->def_height) << height_shift);
        DBG_LOG(2, "[ROOF] elem=%d cell=%d,%d h=%d params=%d %d %d %d %d\n",
                idx, col, row, e->def_height, p[0], p[1], p[2], p[3], p[4]);
        map_roof(bx, by, bz, p);
        i += 5;
    }
}

static void build_map_mesh(void) {
    int ccol = (view_pos.X >> 9) + 64;
    int crow = (view_pos.Z >> 9) + 64;

    if (s_map_built_for == top_of_map_elements &&
        s_map_centre_row == crow && s_map_centre_col == ccol &&
        s_map_count > 0)
        return;

    s_map_built_for  = top_of_map_elements;
    s_map_centre_row = crow;
    s_map_centre_col = ccol;
    s_map_count = 0;

    int r0 = crow - MAP_VIEW_RADIUS, r1 = crow + MAP_VIEW_RADIUS;
    int c0 = ccol - MAP_VIEW_RADIUS, c1 = ccol + MAP_VIEW_RADIUS;
    if (r0 < 0) r0 = 0;
    if (r1 >= MAP_GRID) r1 = MAP_GRID - 1;
    if (c0 < 0) c0 = 0;
    if (c1 >= MAP_GRID) c1 = MAP_GRID - 1;

    for (int row = r0; row <= r1; row++) {
        for (int col = c0; col <= c1; col++) {
            uint16_t idx = new_map[row][col];
            if (idx == 0xFFFF) continue;
            float cx = (float)((col - 64) * MAP_CELL);
            float cz = (float)((row - 64) * MAP_CELL);
            /* Every element of the cell's stack, up to the one marked last. */
            for (; (int)idx < top_of_map_elements; idx++) {
                const map_area_element_t *e = &map_elements[idx];
                map_prism(cx, cz, idx, e);
                map_element_roofs(row, col, idx, e);
                if ((uint16_t)e->code_index_p1 & 0x8000) break;
            }
        }
    }
}

/* ── Frame ────────────────────────────────────────────────── */

void render_frame_begin(void) {
    if (render_backend != RENDER_HARDWARE) return;

    s_ellipsoid_count = 0;
    s_flat_count      = 0;
    s_tex_count       = 0;
    s_frame_begun     = true;

    build_view_matrix(render_frame.view);
    build_proj_matrix(render_frame.proj);
    render_frame.moving_camera = moving_camera;

    if (render_map3d) build_map_mesh();
    render_frame.map_verts = render_map3d ? s_map_verts : NULL;
    render_frame.map_count = render_map3d ? s_map_count : 0;

    render_gl_frame_begin();
}

void render_frame_end(void) {
    if (render_backend != RENDER_HARDWARE) return;

    render_frame.ellipsoids      = s_ellipsoids;
    render_frame.ellipsoid_count = s_ellipsoid_count;
    render_frame.flat_verts      = s_flat_verts;
    render_frame.flat_count      = s_flat_count;
    render_frame.tex_verts       = s_tex_verts;
    render_frame.tex_count       = s_tex_count;
    render_frame.have_3d         = s_frame_begun;

    render_gl_frame_end();

    s_frame_begun = false;
}

/* ── Lifecycle ────────────────────────────────────────────── */

bool render_available(void) { return s_available; }

void render_invalidate_background(void) { render_gl_invalidate_background(); }
void render_invalidate_palette(void)    { render_gl_invalidate_palette(); }

render_backend_t render_select(render_backend_t want) {
    if (want == render_backend) return render_backend;

    if (want == RENDER_HARDWARE) {
        if (!s_available) return RENDER_SOFTWARE;
        if (!render_gl_start()) {
            DBG_LOG(1, "[RENDER] hardware start failed, staying on software\n");
            return RENDER_SOFTWARE;
        }
        render_backend = RENDER_HARDWARE;
    } else {
        render_gl_stop();
        render_backend = RENDER_SOFTWARE;
    }

    /* The baked background store belongs to neither renderer across a switch. */
    unbake_stuck_actors();
    render_gl_invalidate_background();
    return render_backend;
}

void render_init(void) {
    platform_t *p = win_platform();
    if (!p) return;

    s_available = render_gl_init(p);
    if (!s_available) {
        DBG_LOG(1, "[RENDER] no hardware backend; software only\n");
        return;
    }

    /* The ECSTATICA_RENDERER env override beats the stored preference. */
    bool want_hw = (render_hardware_pref != 0);
    const char *env = getenv("ECSTATICA_RENDERER");
    if (env && *env) {
        if (env[0] == 'g' || env[0] == 'G' || env[0] == 'h' || env[0] == 'H')
            want_hw = true;
        else if (env[0] == 's' || env[0] == 'S')
            want_hw = false;
    }

    if (want_hw) render_select(RENDER_HARDWARE);
}

void render_shutdown(void) {
    if (render_backend == RENDER_HARDWARE) render_gl_stop();
    render_gl_shutdown();
    render_backend = RENDER_SOFTWARE;
    s_available = false;
}

#endif /* ECS_ENABLE_GL */
