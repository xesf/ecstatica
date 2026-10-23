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
#include <time.h>
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

/* Grass and plants are ellipsoid lists built on a background thread: `work`
 * is the thread's, `ready` a finished list waiting for the next frame, and
 * `front` the one drawn. They change hands under s_veg_lock. */
typedef struct {
    render_map_ellipsoid_t *v;
    int n, cap;
} ell_list_t;

static ell_list_t s_veg[3];
static ell_list_t *s_veg_front = &s_veg[0], *s_veg_ready = &s_veg[1], *s_veg_work = &s_veg[2];
static ell_list_t *s_ell_target;
static unsigned s_map_gen;

/* An ellipsoid of the 3D map: centre and its three scaled axis vectors. */
static void map_push_ellipsoid(const float c[3], const float ax[3], const float ay[3],
                               const float az[3], const uint8_t rgb[3]) {
    ell_list_t *l = s_ell_target;
    if (l->n >= l->cap) {
        int cap = l->cap ? l->cap * 2 : 65536;
        render_map_ellipsoid_t *g = (render_map_ellipsoid_t *)realloc(l->v,
                                        (size_t)cap * sizeof(*g));
        if (!g) return;
        l->v = g;
        l->cap = cap;
    }
    render_map_ellipsoid_t *e = &l->v[l->n++];
    memcpy(e->centre, c, sizeof(e->centre));
    memcpy(e->ax, ax, sizeof(e->ax));
    memcpy(e->ay, ay, sizeof(e->ay));
    memcpy(e->az, az, sizeof(e->az));
    e->rgb[0] = rgb[0]; e->rgb[1] = rgb[1]; e->rgb[2] = rgb[2];
    e->pad = 0;
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
/* Vertex layer for an untextured face patterned with bumpmap.raw. */
#define MAP_LAYER_STONE (-2)

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

    /* E1's terraces (3) and rock (4) show dry-stone walls on their sides in
     * the re-release's renders. */
    int stone = game_version == GAME_VERSION_E1 && (e->material == 3 || e->material == 4);
    static const uint8_t stone_x[3] = { 100, 100, 104 }, stone_z[3] = { 78, 78, 84 };

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
            if (layer < 0 && stone) {
                layer = MAP_LAYER_STONE;
                c = along_x ? stone_z : stone_x;
            }
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

/* ── Grass ──
 * The re-release's grass (Ecstatica II.exe, case 5/13 of sub_140146430 and
 * the tuft emitter sub_14014F070): each top face is filled on a 16-step grid
 * in its own triangle coordinates, jittered +-16 units; every point carries a
 * tuft of 8 thin ellipsoid blades (8 x 8 x 80..175), each turned 0x5000
 * (112.5 degrees) from the last and leaning 8.4 degrees more, from upright to
 * about 59 degrees. Here a blade is a thin triangle, and the grid thins out
 * with distance from the camera. */
/* Both re-release executables share the material switch: 5 and 13 are the
 * needle shrubs in E1 and E2 alike. */
static int is_grass_material(int material) {
    return material == 5 || material == 13;
}

/* Needle greens from the shrubs in E1's pre-rendered views: dark at the
 * root, yellow-lit at the tip. */
static const uint8_t grass_root_rgb[3] = { 20, 70, 20 };
static const uint8_t grass_tip_rgb[3]  = { 150, 160, 40 };

static uint32_t grass_rand(uint32_t *s) {
    *s = *s * 0x41C64E6Du + 0x3039u;
    return *s >> 16;
}

/* Blades are expensive: only the cells near the camera get them, and fewer
 * further out. */
#define GRASS_RADIUS 9

static void blade_colour(uint32_t *seed, uint8_t out[3]) {
    float shade = 0.8f + (float)(grass_rand(seed) % 40) / 100.0f;
    float mix = (float)(grass_rand(seed) % 100) / 100.0f;
    for (int k = 0; k < 3; k++) {
        float v = (grass_root_rgb[k] + (grass_tip_rgb[k] - grass_root_rgb[k]) * mix) * shade;
        out[k] = (uint8_t)(v > 255 ? 255 : v);
    }
}

static void map_grass_tuft(float x, float y, float z, const uint8_t base[3], uint32_t *seed) {
    (void)base;
    float yaw0 = (float)(grass_rand(seed) & 0xFFFF) * (6.2831853f / 65536.0f);
    for (int b = 0; b < 8; b++) {
        float yaw = yaw0 + (float)b * (6.2831853f * 0x5000 / 0x10000);
        float tilt = (float)b * (6.2831853f * 0x180 / 0x4000);
        float len = 80.0f + (float)(grass_rand(seed) % 96);
        float dx = cosf(yaw), dz = sinf(yaw);
        /* Up is -Y in the engine; the blade grows from (x, y, z). */
        float az[3] = { dx * sinf(tilt), -cosf(tilt), dz * sinf(tilt) };
        float ax[3] = { -dz * 8.0f, 0.0f, dx * 8.0f };
        float ay[3] = { az[1] * dx * 8.0f, (az[2] * -dz - az[0] * dx) * 8.0f, az[1] * dz * 8.0f };
        float c[3] = { x + az[0] * len, y + az[1] * len, z + az[2] * len };
        for (int k = 0; k < 3; k++) az[k] *= len;
        uint8_t rgb[3];
        blade_colour(seed, rgb);
        map_push_ellipsoid(c, ax, ay, az, rgb);
    }
}

static void map_grass(float cx, float cz, int idx, const map_area_element_t *e,
                      int ring) {
    map_poly_t polys[4];
    int np = map_footprint(e->block_config, polys);
    float y = (float)((128 - (int)e->def_height) << height_shift);
    /* The re-release's 16-step grid, halved per ring of distance. */
    int steps = ring <= 2 ? 16 : ring <= 5 ? 8 : 4;
    uint8_t base[3];
    material_colour(e->material, 1.0f, base);
    uint32_t seed = (uint32_t)idx * 2654435761u + 12345u;

    for (int i = 0; i < np; i++) {
        const map_poly_t *p = &polys[i];
        for (int t = 1; t + 1 < p->n; t++) {
            float ax = cx + p->x[0], az = cz + p->z[0];
            float bx = cx + p->x[t] - ax, bz = cz + p->z[t] - az;
            float qx = cx + p->x[t + 1] - ax, qz = cz + p->z[t + 1] - az;
            for (int gi = 0; gi < steps; gi++) {
                for (int gj = 0; gi + gj < steps; gj++) {
                    float u = (float)gi / steps, v = (float)gj / steps;
                    float jx = (float)((int)(grass_rand(&seed) & 31) - 16);
                    float jz = (float)((int)(grass_rand(&seed) & 31) - 16);
                    map_grass_tuft(ax + bx * u + qx * v + jx, y,
                                   az + bz * u + qz * v + jz, base, &seed);
                }
            }
        }
        /* The face renderer runs on every face, so a raised shrub block is
         * covered on its sides too. */
        if (!e->height2) continue;
        float ybot = (float)((128 - (int)e->height2) << height_shift);
        for (int k = 0; k < p->n; k++) {
            int j = (k + 1) % p->n;
            float x0 = cx + p->x[k], z0 = cz + p->z[k];
            float ex = cx + p->x[j] - x0, ez = cz + p->z[j] - z0;
            for (int gi = 0; gi < steps; gi++) {
                for (int gh = 0; gh < steps / 2; gh++) {
                    float u = ((float)gi + 0.5f) / steps;
                    float h = ((float)gh + 0.5f) / (steps / 2);
                    map_grass_tuft(x0 + ex * u, y + (ybot - y) * h, z0 + ez * u, base, &seed);
                }
            }
        }
    }
}

/* ── Plants ──
 * Materials 2, 9, 10 and 20 grow a plant on every top-face triangle
 * (Ecstatica.exe: decoration switch of sub_14013A460, generator
 * sub_140143D40 and the recursive branch sub_1401457C0). A plant is a chain
 * of ellipsoid segments, each 1/32 smaller than the last and starting at the
 * previous one's centre; every fifth segment splits off three branches 120
 * degrees apart, tilted 67.5 degrees; twigs under 0xB0 carry three needles.
 * Sizes 0x600+ are trees, under 0x500 bushes that split at the root. Here an
 * ellipsoid is a GPU impostor, like the actors'. */
typedef struct { float m[3][3]; float p[3]; } plant_frame_t;

#define ANGLE_RAD(a) ((float)((a) & 0xFFFF) * (6.2831853f / 65536.0f))

/* A big tree is tens of thousands of ellipsoids; plants are built nearest
 * first until the budget runs out. */
#define PLANT_RADIUS 12
#define PLANT_ELL_BUDGET (256 * 1024)

static const uint8_t plant_stem_rgb[3] = { 60, 48, 22 };

static int plant_ell_end;
/* The original's level of detail, 64 = full: out of 64 segments this many
 * are drawn (the rest lengthen the next one), and needles grow with it. */
static int plant_lod;

/* Columns of m are the local axes; local -Z grows up (world -Y). */
static void plant_rotate(plant_frame_t *f, int axis, int angle) {
    if (!(angle & 0xFFFF)) return;
    float c = cosf(ANGLE_RAD(angle)), s = sinf(ANGLE_RAD(angle));
    int a = axis == 0 ? 1 : 0, b = axis == 2 ? 1 : 2;
    for (int r = 0; r < 3; r++) {
        float u = f->m[r][a], v = f->m[r][b];
        f->m[r][a] = u * c + v * s;
        f->m[r][b] = v * c - u * s;
    }
}

static void plant_point(const plant_frame_t *f, float lx, float ly, float lz, float out[3]) {
    for (int r = 0; r < 3; r++)
        out[r] = f->p[r] + f->m[r][0] * lx + f->m[r][1] * ly + f->m[r][2] * lz;
}

static int plant_full(void) {
    return s_ell_target->n >= plant_ell_end;
}

static void plant_ellipsoid(const plant_frame_t *f, float off, float rxy, float rz,
                            const uint8_t rgb[3]) {
    if (rxy <= 0.0f || plant_full()) return;
    float c[3], ax[3], ay[3], az[3];
    plant_point(f, 0, 0, off, c);
    for (int r = 0; r < 3; r++) {
        ax[r] = f->m[r][0] * rxy;
        ay[r] = f->m[r][1] * rxy;
        az[r] = f->m[r][2] * rz;
    }
    map_push_ellipsoid(c, ax, ay, az, rgb);
}

static void plant_needle(const plant_frame_t *f, float w, float len, uint32_t *seed) {
    uint8_t rgb[3];
    blade_colour(seed, rgb);
    plant_ellipsoid(f, -len, w, len, rgb);
}

/* The child's frame: at the parent's ellipsoid centre, turned by the child's
 * own angles (X then Z, as sub_140133760 applies them). */
static plant_frame_t plant_child(const plant_frame_t *parent, float parent_off,
                                 int pitch, int spin) {
    plant_frame_t f = *parent;
    plant_point(parent, 0, 0, parent_off, f.p);
    plant_rotate(&f, 0, pitch);
    plant_rotate(&f, 2, spin);
    return f;
}

static void plant_branch(const plant_frame_t *start, int size, int age, int index,
                         uint32_t *seed) {
    plant_frame_t parent = *start;
    float parent_off = 0.0f;
    int first = index, skipped = 0;
    /* Needles: the original's count (c * 59 + 32) / 64 + 5 of 64, and
     * 8 x 8 x 64 close up, up to 24 x 24 x 96 far away. */
    int needles = (plant_lod * 59 + 32) / 64 + 5;
    float needle_w = (float)((64 - plant_lod) / 4 + 8);
    float needle_len = (float)((64 - plant_lod) / 2 + 64);
    for (;; index++) {
        int fifth = index % 5;
        if (index && !fifth) {
            int child = size >= 0x801 ? 0x400
                      : size >= 0x100 ? size / 2 + (size < 0x180 ? (0x180 - size) / 2 : 0)
                      : 0;
            for (int k = 0; k < 3 && child && !plant_full(); k++) {
                plant_frame_t knob = plant_child(&parent, parent_off, 0x3000, k * 0x10000 / 3);
                plant_ellipsoid(&knob, 0.0f, (float)(child >> 4), (float)(child >> 5),
                                plant_stem_rgb);
                plant_branch(&knob, child, age, 0, seed);
            }
            if (size >= 0xC9 && size <= 0x11F) size -= size / 4;
        }
        if (age >= 0x2EF && size <= 0xAF &&
            (needles > 63 || ((unsigned)(needles * index) & 63) < (unsigned)needles)) {
            int pitch = size <= 0x8F ? (size << 9) + 0x1000 : 0x3000;
            for (int k = 0; k < 3; k++) {
                plant_frame_t leaf = plant_child(&parent, parent_off, pitch, k * 0x10000 / 3);
                plant_needle(&leaf, needle_w, needle_len, seed);
            }
        }
        if (age < 0x2EE && size >= 0x1C1 && size < 0x200) size = 0;
        size -= size / 32;
        int spin = size <= 0x9F ? (int)(grass_rand(seed) << 1) : fifth ? 0 : 0x2AAA;
        plant_frame_t seg = plant_child(&parent, parent_off, 0, spin);
        float off = -(float)((size >> 3) / 2);
        skipped++;
        if (plant_lod >= 64 || size < 0x80 ||
            ((unsigned)((index - first) * plant_lod) & 63) < (unsigned)plant_lod) {
            float len = (float)(size >> 3) * (float)(skipped < 4 ? skipped : 4);
            plant_ellipsoid(&seg, off, (float)(size >> 4), len, plant_stem_rgb);
            skipped = 0;
        }
        parent = seg;
        parent_off = off;
        if (size < 0x80 || plant_full()) return;
        age += size >> 8;
    }
}

static void map_plant(float x, float y, float z, int size, uint32_t *seed) {
    float yaw = ANGLE_RAD(grass_rand(seed));
    float c = cosf(yaw), s = sinf(yaw);
    plant_frame_t root = { { { c, -s, 0 }, { 0, 0, 1 }, { -s, -c, 0 } }, { x, y, z } };
    plant_ellipsoid(&root, 0.0f, (float)(size >> 4), (float)(size >> 3), plant_stem_rgb);
    int age = 0;
    for (int s2 = 0x2000; s2 > size; ) {
        s2 -= s2 / 32;
        age += s2 >> 8;
    }
    plant_branch(&root, size, age, size < 0x500 ? 5 : 0, seed);
}

static int is_plant_material(int material) {
    return material == 2 || material == 9 || material == 10 || material == 20;
}

/* Materials that count as open ground next to a plant: the bit set
 * 0x3FAF29F9 the original tests on the neighbouring elements. Water and the
 * plant materials themselves are not in it. */
static int is_open_ground(int material) {
    return material < 30 && ((0x3FAF29F9u >> material) & 1);
}

/* Bushes of 9 and 10 are smaller where the patch meets open ground. */
static int plant_at_edge(int row, int col) {
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            int r = row + dr, c = col + dc;
            if ((!dr && !dc) || r < 0 || c < 0 || r >= MAP_GRID || c >= MAP_GRID) continue;
            uint16_t i = new_map[r][c];
            if (i == 0xFFFF) return 1;
            while ((int)i + 1 < top_of_map_elements &&
                   !((uint16_t)map_elements[i].code_index_p1 & 0x8000))
                i++;
            if (is_open_ground(map_elements[i].material)) return 1;
        }
    }
    return 0;
}

static int plant_size(int material, int edge, uint32_t *seed) {
    uint32_t r = grass_rand(seed);
    switch (material) {
    case 2:  return 0x600 + (int)(r & 0x3FF);
    case 20: return 0x400 + (int)(r & 0x1FF);
    case 9:  return edge ? 0x100 + (int)(r & 0x1FF) : 0x200 + (int)(r & 0x3FF);
    default: return edge ? 0x80 + (int)(r & 0x7F) : 0x100 + (int)(r & 0xFF);
    }
}

static int point_in_poly(const map_poly_t *p, float x, float z) {
    int in = 0;
    for (int i = 0, j = p->n - 1; i < p->n; j = i++) {
        if ((p->z[i] > z) != (p->z[j] > z) &&
            x < (p->x[j] - p->x[i]) * (z - p->z[i]) / (p->z[j] - p->z[i]) + p->x[i])
            in = !in;
    }
    return in;
}

/* One plant per top-face triangle: the cell's eight triangles fan from its
 * centre to each corner and edge midpoint; the plant stands a quarter of the
 * way along both edges from the centre. Nothing grows under an element that
 * is not open ground. */
static void map_plants(int row, int col, int idx, const map_area_element_t *e,
                       const map_area_element_t *above) {
    if (above && !is_open_ground(above->material)) return;
    static const float rim[8][2] = {
        {0,0}, {256,0}, {512,0}, {512,256}, {512,512}, {256,512}, {0,512}, {0,256}
    };
    float cx = (float)((col - 64) * MAP_CELL), cz = (float)((row - 64) * MAP_CELL);
    map_poly_t polys[4];
    int np = map_footprint(e->block_config, polys);
    float y = (float)((128 - (int)e->def_height) << height_shift);
    uint32_t seed = (uint32_t)idx * 0x9E3779B1u;
    seed ^= seed >> 15;
    seed *= 0x85EBCA77u;
    seed ^= seed >> 13;
    int edge = (e->material == 9 || e->material == 10) && plant_at_edge(row, col);
    for (int t = 0; t < 8; t++) {
        const float *a = rim[t], *b = rim[(t + 1) & 7];
        float px = 256.0f + (a[0] - 256.0f) / 4 + (b[0] - 256.0f) / 4;
        float pz = 256.0f + (a[1] - 256.0f) / 4 + (b[1] - 256.0f) / 4;
        float gx = (256.0f + a[0] + b[0]) / 3, gz = (256.0f + a[1] + b[1]) / 3;
        int inside = 0;
        for (int i = 0; i < np && !inside; i++) inside = point_in_poly(&polys[i], gx, gz);
        if (!inside) continue;
        map_plant(cx + px, y, cz + pz, plant_size(e->material, edge, &seed), &seed);
    }
}

/* Every plant cell within PLANT_RADIUS, ring by ring outwards. */
static void map_all_plants(int crow, int ccol) {
    plant_ell_end = s_ell_target->n + PLANT_ELL_BUDGET;
    for (int ring = 0; ring <= PLANT_RADIUS && !plant_full(); ring++) {
        plant_lod = ring <= 1 ? 64 : 64 / ring;
        for (int row = crow - ring; row <= crow + ring; row++) {
            for (int col = ccol - ring; col <= ccol + ring; col++) {
                if (abs(row - crow) != ring && abs(col - ccol) != ring) continue;
                if (row < 0 || col < 0 || row >= MAP_GRID || col >= MAP_GRID) continue;
                uint16_t idx = new_map[row][col];
                if (idx == 0xFFFF) continue;
                for (; (int)idx < top_of_map_elements; idx++) {
                    const map_area_element_t *e = &map_elements[idx];
                    int last = ((uint16_t)e->code_index_p1 & 0x8000) != 0;
                    if (is_plant_material(e->material))
                        map_plants(row, col, idx, e, last ? NULL : &map_elements[idx + 1]);
                    if (last) break;
                }
            }
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

/* Grass and plants around (crow, ccol) into s_ell_target. Reads only the
 * map, so it runs off the main thread. */
static void build_vegetation(int crow, int ccol) {
    for (int row = crow - GRASS_RADIUS; row <= crow + GRASS_RADIUS; row++) {
        for (int col = ccol - GRASS_RADIUS; col <= ccol + GRASS_RADIUS; col++) {
            if (row < 0 || col < 0 || row >= MAP_GRID || col >= MAP_GRID) continue;
            uint16_t idx = new_map[row][col];
            if (idx == 0xFFFF) continue;
            int ring = abs(row - crow) > abs(col - ccol) ? abs(row - crow) : abs(col - ccol);
            float cx = (float)((col - 64) * MAP_CELL);
            float cz = (float)((row - 64) * MAP_CELL);
            for (; (int)idx < top_of_map_elements; idx++) {
                const map_area_element_t *e = &map_elements[idx];
                if (is_grass_material(e->material))
                    map_grass(cx, cz, idx, e, ring);
                if ((uint16_t)e->code_index_p1 & 0x8000) break;
            }
        }
    }
    map_all_plants(crow, ccol);
}

static platform_sem_t *s_veg_lock, *s_veg_wake;
static int s_veg_thread = -1;         /* -1 not tried, 0 none, 1 running */
static int s_veg_req_row, s_veg_req_col, s_veg_req_map;
static unsigned s_veg_req_id;
static int s_veg_ready_map, s_veg_has_ready;

static void vegetation_thread(void *arg) {
    (void)arg;
    unsigned done = 0;
    for (;;) {
        platform_sem_wait(s_veg_wake);
        platform_sem_wait(s_veg_lock);
        unsigned id = s_veg_req_id;
        int row = s_veg_req_row, col = s_veg_req_col, key = s_veg_req_map;
        platform_sem_post(s_veg_lock);
        if (id == done) continue;

        clock_t t0 = clock();
        s_veg_work->n = 0;
        s_ell_target = s_veg_work;
        build_vegetation(row, col);
        DBG_LOG(2, "[MAP3D] vegetation %d ellipsoids in %ld ms\n", s_veg_work->n,
                (long)((clock() - t0) * 1000 / CLOCKS_PER_SEC));
        done = id;

        platform_sem_wait(s_veg_lock);
        ell_list_t *t = s_veg_ready;
        s_veg_ready = s_veg_work;
        s_veg_work = t;
        s_veg_ready_map = key;
        s_veg_has_ready = 1;
        platform_sem_post(s_veg_lock);
    }
}

static void request_vegetation(int crow, int ccol) {
    if (s_veg_thread < 0) {
        s_veg_thread = 0;
        s_veg_lock = platform_sem_create(1);
        s_veg_wake = platform_sem_create(0);
        if (s_veg_lock && s_veg_wake && platform_cpu_count() > 1 &&
            platform_thread_start("ecs_vegetation", vegetation_thread, NULL,
                                  PLATFORM_THREAD_BACKGROUND, -1))
            s_veg_thread = 1;
    }
    if (s_veg_thread == 0) {
        s_veg_front->n = 0;
        s_ell_target = s_veg_front;
        build_vegetation(crow, ccol);
        return;
    }
    platform_sem_wait(s_veg_lock);
    s_veg_req_row = crow;
    s_veg_req_col = ccol;
    s_veg_req_map = top_of_map_elements;
    s_veg_req_id++;
    platform_sem_post(s_veg_lock);
    platform_sem_post(s_veg_wake);
}

/* Takes a finished vegetation list; drops the shown one when the map it was
 * grown from has been replaced. Returns true when the front list changed. */
static int collect_vegetation(void) {
    if (s_veg_thread != 1) return 0;
    int changed = 0;
    platform_sem_wait(s_veg_lock);
    if (s_veg_has_ready) {
        s_veg_has_ready = 0;
        if (s_veg_ready_map == top_of_map_elements) {
            ell_list_t *t = s_veg_front;
            s_veg_front = s_veg_ready;
            s_veg_ready = t;
            changed = 1;
        }
    }
    platform_sem_post(s_veg_lock);
    return changed;
}

static void build_map_mesh(void) {
    int ccol = (view_pos.X >> 9) + 64;
    int crow = (view_pos.Z >> 9) + 64;

    if (collect_vegetation()) s_map_gen++;

    if (s_map_built_for == top_of_map_elements &&
        s_map_centre_row == crow && s_map_centre_col == ccol &&
        s_map_count > 0)
        return;

    if (s_map_built_for != top_of_map_elements) s_veg_front->n = 0;
    s_map_built_for  = top_of_map_elements;
    s_map_centre_row = crow;
    s_map_centre_col = ccol;
    s_map_count = 0;
    s_map_gen++;

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
    request_vegetation(crow, ccol);
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
    render_frame.map_ells = render_map3d ? s_veg_front->v : NULL;
    render_frame.map_ell_count = render_map3d ? s_veg_front->n : 0;
    render_frame.map_gen = s_map_gen;

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
