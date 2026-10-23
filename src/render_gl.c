/**
 * render_gl.c
 *
 * OpenGL 3.3 core backend. Every GL call in the project is here, so another
 * backend (Metal) would replace this one file.
 *
 * The frame is:
 *   background   full-screen, palette-expanded, depth written from the mask
 *   ellipsoids   instanced quadric impostors
 *   triangles    flat and textured
 *   composite    the engine's 8bpp 2D plane over the top
 */

#include "render.h"

#ifdef ECS_ENABLE_GL

#include "render_priv.h"
#include "render_gl_shaders.h"
#include "gl_loader.h"
#include "display.h"
#include "game.h"
#include "init.h"
#include "topo.h"
#include "win.h"
#include <string.h>
#include <stdlib.h>

#define MAX_TEX_LAYERS   32
#define TEX_LAYER_DIM    128

/* GPU instance layout; colour_shade is folded to 7 bits here. */
typedef struct {
    float centre[3];
    float axes[3];
    float rot[9];
    float radius;
    int32_t style[2];      /* colour, colour_shade >> 7 */
} gl_instance_t;

static struct {
    bool ready;
    bool active;
    platform_t *plat;

    int fb_w, fb_h;        /* current engine render size */
    int ss;                /* supersample factor actually in use */

    GLuint prog_bg, prog_composite, prog_flat, prog_tex, prog_ell, prog_ell_mod;
    GLuint prog_map;

    /* RGB to look at, plus each pixel's palette index for the shadow/smoke/beam
     * passes to remap. */
    GLuint scene_fbo, scene_color, scene_index, scene_depth;
    int    scene_w, scene_h;

    /* A bound attachment cannot be read, so the modulating passes read this copy. */
    GLuint copy_fbo, copy_index, copy_depth;

    GLuint tex_palette;     /* 256x1  R8UI ... stored RGB8 */
    GLuint tex_bg_index;    /* R8UI   background palette indices */
    GLuint tex_bg_depth;    /* R16I   background view-space depth */
    GLuint tex_ui_index;    /* R8UI   the engine's 2D plane */
    GLuint tex_shade_map;   /* 128x128 R8UI */
    GLuint tex_shade_tab;   /* 128x128x17 R8UI */
    GLuint tex_shadow_tab;  /* 256x16x3  R8UI — SHADOW.DAT */
    GLuint tex_array;       /* part textures */

    GLuint vao_empty;
    GLuint vao_tri, vbo_tri;
    GLuint vao_ell, vbo_ell;
    GLuint vao_map, vbo_map;

    int16_t tex_layer[TEXTURE_TAB_SIZE];
    int     tex_layer_next;

    bool bg_dirty;
    bool pal_dirty;
    bool tables_uploaded;
    /* Stays true across flips that carry no new geometry. */
    bool have_scene;

    uint8_t pal_cache[768];
} G;

/* ── Shader helpers ───────────────────────────────────────── */

static GLuint compile_shader(GLenum type, const char *src, const char *what) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);

    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei n = 0;
        glGetShaderInfoLog(s, (GLsizei)sizeof(log) - 1, &n, log);
        log[n < (GLsizei)sizeof(log) ? n : (GLsizei)sizeof(log) - 1] = 0;
        DBG_LOG(1, "[GL] %s shader failed to compile:\n%s\n", what, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program(const char *vs_src, const char *fs_src, const char *what) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src, what);
    if (!vs) return 0;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src, what);
    if (!fs) { glDeleteShader(vs); return 0; }

    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei n = 0;
        glGetProgramInfoLog(p, (GLsizei)sizeof(log) - 1, &n, log);
        log[n < (GLsizei)sizeof(log) ? n : (GLsizei)sizeof(log) - 1] = 0;
        DBG_LOG(1, "[GL] %s program failed to link:\n%s\n", what, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static void set_uniform_i(GLuint prog, const char *name, int v) {
    GLint loc = glGetUniformLocation(prog, name);
    if (loc >= 0) glUniform1i(loc, v);
}

static void set_uniform_f(GLuint prog, const char *name, float v) {
    GLint loc = glGetUniformLocation(prog, name);
    if (loc >= 0) glUniform1f(loc, v);
}

static void set_uniform_m4(GLuint prog, const char *name, const float *m) {
    GLint loc = glGetUniformLocation(prog, name);
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, m);
}

/* ── Texture helpers ──────────────────────────────────────── */

static GLuint make_tex_2d(GLenum internal, int w, int h, GLenum fmt, GLenum type,
                          const void *data) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, w, h, 0, fmt, type, data);
    return t;
}

/* ── Scene target ─────────────────────────────────────────── */

static void destroy_scene_target(void) {
    if (G.scene_fbo)   { glDeleteFramebuffers(1, &G.scene_fbo); G.scene_fbo = 0; }
    if (G.copy_fbo)    { glDeleteFramebuffers(1, &G.copy_fbo);  G.copy_fbo = 0; }
    if (G.scene_color) { glDeleteTextures(1, &G.scene_color);   G.scene_color = 0; }
    if (G.scene_index) { glDeleteTextures(1, &G.scene_index);   G.scene_index = 0; }
    if (G.scene_depth) { glDeleteTextures(1, &G.scene_depth);   G.scene_depth = 0; }
    if (G.copy_index)  { glDeleteTextures(1, &G.copy_index);    G.copy_index = 0; }
    if (G.copy_depth)  { glDeleteTextures(1, &G.copy_depth);    G.copy_depth = 0; }
    G.scene_w = G.scene_h = 0;
}

static bool ensure_scene_target(int w, int h) {
    if (G.scene_fbo && G.scene_w == w && G.scene_h == h) return true;
    destroy_scene_target();

    G.scene_color = make_tex_2d(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    G.scene_index = make_tex_2d(GL_R8UI, w, h, GL_RED_INTEGER, GL_UNSIGNED_BYTE, NULL);
    G.scene_depth = make_tex_2d(GL_DEPTH_COMPONENT24, w, h,
                                GL_DEPTH_COMPONENT, GL_FLOAT, NULL);

    glGenFramebuffers(1, &G.scene_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, G.scene_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G.scene_color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, G.scene_index, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,  GL_TEXTURE_2D, G.scene_depth, 0);
    {
        GLenum bufs[2];
        bufs[0] = GL_COLOR_ATTACHMENT0;
        bufs[1] = GL_COLOR_ATTACHMENT1;
        glDrawBuffers(2, bufs);
    }

    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        DBG_LOG(1, "[GL] scene framebuffer incomplete (0x%04X) at %dx%d\n", st, w, h);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        destroy_scene_target();
        return false;
    }

    G.copy_index = make_tex_2d(GL_R8UI, w, h, GL_RED_INTEGER, GL_UNSIGNED_BYTE, NULL);
    G.copy_depth = make_tex_2d(GL_DEPTH_COMPONENT24, w, h,
                               GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glGenFramebuffers(1, &G.copy_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, G.copy_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G.copy_index, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,  GL_TEXTURE_2D, G.copy_depth, 0);
    {
        GLenum one = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &one);
    }
    st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        DBG_LOG(1, "[GL] copy framebuffer incomplete (0x%04X)\n", st);
        destroy_scene_target();
        return false;
    }

    G.scene_w = w;
    G.scene_h = h;
    return true;
}

/* Snapshot index and depth for the modulating passes: after the background
 * (shadows, as draw_parts' pass order) and after the opaque geometry (smoke,
 * beams). */
static void snapshot_scene(void) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, G.scene_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, G.copy_fbo);
    /* The index is attachment 1; restore the read buffer after, as composite_2d
     * blits attachment 0 from this framebuffer. */
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    glBlitFramebuffer(0, 0, G.scene_w, G.scene_h, 0, 0, G.scene_w, G.scene_h,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBlitFramebuffer(0, 0, G.scene_w, G.scene_h, 0, 0, G.scene_w, G.scene_h,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_FRAMEBUFFER, G.scene_fbo);
}

/* ── Uploads ──────────────────────────────────────────────── */

/* Runs once: shade_map and shade_tab never change after load. */
static void upload_static_tables(void) {
    if (G.tables_uploaded) return;

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    G.tex_shade_map = make_tex_2d(GL_R8UI, 128, 128, GL_RED_INTEGER,
                                  GL_UNSIGNED_BYTE, &shade_map[0][0]);

    glGenTextures(1, &G.tex_shade_tab);
    glBindTexture(GL_TEXTURE_3D, G.tex_shade_tab);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    /* Dimensions are (shade, band, colour) to match the shader's texelFetch. */
    glTexImage3D(GL_TEXTURE_3D, 0, GL_R8UI, 128, 128, 17, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_BYTE, &shade_tab[0][0][0]);

    /* SHADOW.DAT [3][16][256], fetched as (src_index, colour, table). */
    glGenTextures(1, &G.tex_shadow_tab);
    glBindTexture(GL_TEXTURE_3D, G.tex_shadow_tab);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_R8UI, 256, 16, 3, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_BYTE, &shadow_tab[0][0][0]);

    glGenTextures(1, &G.tex_array);
    glBindTexture(GL_TEXTURE_2D_ARRAY, G.tex_array);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8UI,
                 TEX_LAYER_DIM, TEX_LAYER_DIM, MAX_TEX_LAYERS, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_BYTE, NULL);

    G.tables_uploaded = true;
}

static void upload_palette(void) {
    const uint8_t *pal = (const uint8_t *)view_cmap;
    uint8_t rgb[256 * 3];
    for (int i = 0; i < 256; i++) {
        /* 6-bit VGA DAC values, the same << 2 every backend applies. */
        rgb[i * 3 + 0] = (uint8_t)((pal[i * 3 + 0] & 0x3F) << 2);
        rgb[i * 3 + 1] = (uint8_t)((pal[i * 3 + 1] & 0x3F) << 2);
        rgb[i * 3 + 2] = (uint8_t)((pal[i * 3 + 2] & 0x3F) << 2);
    }
    memcpy(G.pal_cache, pal, 768);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (!G.tex_palette) {
        G.tex_palette = make_tex_2d(GL_RGB8, 256, 1, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    } else {
        glBindTexture(GL_TEXTURE_2D, G.tex_palette);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    }
    G.pal_dirty = false;
}

static void upload_background(void) {
    int w = screen_width, h = screen_height;
    if (w <= 0 || h <= 0 || !bitmap[3] || !mask_map[2]) return;

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    /* Planes are strided by hires_width, which may exceed the visible width. */
    static uint8_t  *idx_buf = NULL;
    static int16_t  *dep_buf = NULL;
    static int       buf_px  = 0;
    if (buf_px < w * h) {
        free(idx_buf); free(dep_buf);
        idx_buf = (uint8_t *)malloc((size_t)w * h);
        dep_buf = (int16_t *)malloc((size_t)w * h * sizeof(int16_t));
        buf_px = (idx_buf && dep_buf) ? w * h : 0;
        if (!buf_px) return;
    }
    for (int y = 0; y < h; y++) {
        memcpy(idx_buf + (size_t)y * w, bitmap[3] + (size_t)y * hires_width, w);
        memcpy(dep_buf + (size_t)y * w, mask_map[2] + (size_t)y * hires_width,
               (size_t)w * sizeof(int16_t));
    }

    if (G.tex_bg_index) glDeleteTextures(1, &G.tex_bg_index);
    if (G.tex_bg_depth) glDeleteTextures(1, &G.tex_bg_depth);
    G.tex_bg_index = make_tex_2d(GL_R8UI, w, h, GL_RED_INTEGER, GL_UNSIGNED_BYTE, idx_buf);
    G.tex_bg_depth = make_tex_2d(GL_R16I, w, h, GL_RED_INTEGER, GL_SHORT, dep_buf);

    /* Level 2 only: a full pass over the depth image. */
    if (debug_verbose >= 2) {
        int16_t lo = 0x7FFF, hi = -0x7FFF;
        long far_count = 0;
        for (int i = 0; i < w * h; i++) {
            int16_t v = dep_buf[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
            if (v >= 0x7FFF) far_count++;
        }
        DBG_LOG(2, "[GL] bg upload cam=%d %dx%d depth min=%d max=%d far=%ld/%d\n",
                (int)selected_camera, w, h, (int)lo, (int)hi, far_count, w * h);
    }

    G.bg_dirty = false;
#ifdef ENABLE_FRAME_DUMP
    {
        extern int render_gl_dump_after_cut;
        render_gl_dump_after_cut = 3;
    }
#endif
}

static void upload_ui_plane(void) {
    int w = screen_width, h = screen_height;
    if (w <= 0 || h <= 0) return;

    const char *plane = bitmap[db];
    if (!plane) return;

    static uint8_t *buf = NULL;
    static int      buf_px = 0;
    if (buf_px < w * h) {
        free(buf);
        buf = (uint8_t *)malloc((size_t)w * h);
        buf_px = buf ? w * h : 0;
        if (!buf_px) return;
    }
    for (int y = 0; y < h; y++)
        memcpy(buf + (size_t)y * w, plane + (size_t)y * hires_width, w);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (!G.tex_ui_index || G.fb_w != w || G.fb_h != h) {
        if (G.tex_ui_index) glDeleteTextures(1, &G.tex_ui_index);
        G.tex_ui_index = make_tex_2d(GL_R8UI, w, h, GL_RED_INTEGER, GL_UNSIGNED_BYTE, buf);
        G.fb_w = w;
        G.fb_h = h;
    } else {
        glBindTexture(GL_TEXTURE_2D, G.tex_ui_index);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED_INTEGER, GL_UNSIGNED_BYTE, buf);
    }
}

int render_gl_texture_layer(int16_t texture_name_index) {
    if (texture_name_index < 0 || texture_name_index >= TEXTURE_TAB_SIZE) return -1;
    if (!G.ready) return -1;

    int cached = G.tex_layer[texture_name_index];
    if (cached >= 0) return cached;

    texture_t *tex = texture_tab[texture_name_index];
    if (!tex || !tex->texture_data) return -1;
    if (G.tex_layer_next >= MAX_TEX_LAYERS) return -1;

    /* The rasteriser masks u and v with 0x7F, so every layer is 128x128 and a
     * smaller texture repeats its rows. */
    uint8_t layer[TEX_LAYER_DIM * TEX_LAYER_DIM];
    int tw = tex->x_size > 0 ? tex->x_size : TEX_LAYER_DIM;
    int th = tex->y_size > 0 ? tex->y_size : TEX_LAYER_DIM;
    for (int y = 0; y < TEX_LAYER_DIM; y++) {
        const unsigned char *src = (const unsigned char *)tex->texture_data + (y % th) * tw;
        for (int x = 0; x < TEX_LAYER_DIM; x++)
            layer[y * TEX_LAYER_DIM + x] = src[x % tw];
    }

    int slot = G.tex_layer_next++;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, G.tex_array);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, slot,
                    TEX_LAYER_DIM, TEX_LAYER_DIM, 1,
                    GL_RED_INTEGER, GL_UNSIGNED_BYTE, layer);

    G.tex_layer[texture_name_index] = (int16_t)slot;
    return slot;
}

/* ── Draw ─────────────────────────────────────────────────── */

static void bind_tex(int unit, GLenum target, GLuint tex) {
    glActiveTexture((GLenum)(GL_TEXTURE0 + unit));
    glBindTexture(target, tex);
}

static void draw_fullscreen(void) {
    glBindVertexArray(G.vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* The collision map as ordinary depth-tested geometry, so characters occlude
 * against the terrain the game walks on. */
static void draw_map3d(void) {
    glClearColor(0.05f, 0.06f, 0.09f, 1.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (render_frame.map_count <= 0) return;

    glUseProgram(G.prog_map);
    set_uniform_m4(G.prog_map, "u_view", render_frame.view);
    set_uniform_m4(G.prog_map, "u_proj", render_frame.proj);
    bind_tex(0, GL_TEXTURE_2D, G.tex_palette);
    set_uniform_i(G.prog_map, "u_palette", 0);
    bind_tex(1, GL_TEXTURE_2D_ARRAY, G.tex_array);
    set_uniform_i(G.prog_map, "u_textures", 1);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);

    glBindVertexArray(G.vao_map);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo_map);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t)render_frame.map_count * sizeof(render_map_vertex_t)),
                 render_frame.map_verts, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, render_frame.map_count);
}

static void draw_background(void) {
    glUseProgram(G.prog_bg);
    bind_tex(0, GL_TEXTURE_2D, G.tex_bg_index);
    bind_tex(1, GL_TEXTURE_2D, G.tex_bg_depth);
    bind_tex(2, GL_TEXTURE_2D, G.tex_palette);
    set_uniform_i(G.prog_bg, "u_bg_index", 0);
    set_uniform_i(G.prog_bg, "u_bg_depth", 1);
    set_uniform_i(G.prog_bg, "u_palette", 2);
    set_uniform_f(G.prog_bg, "u_near", (float)RENDER_NEAR_Z);
    set_uniform_f(G.prog_bg, "u_far",  (float)RENDER_FAR_Z);

    /* GL_ALWAYS with the test enabled: disabling GL_DEPTH_TEST also disables
     * depth writes and gl_FragDepth, and nothing would be occluded by the
     * background. */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    draw_fullscreen();
    glDepthFunc(GL_LEQUAL);
}

static void draw_triangles(const render_vertex_t *verts, int count, bool textured) {
    if (count <= 0) return;

    GLuint prog = textured ? G.prog_tex : G.prog_flat;
    glUseProgram(prog);
    set_uniform_m4(prog, "u_view", render_frame.view);
    set_uniform_m4(prog, "u_proj", render_frame.proj);
    bind_tex(0, GL_TEXTURE_2D, G.tex_palette);
    set_uniform_i(prog, "u_palette", 0);
    if (textured) {
        bind_tex(1, GL_TEXTURE_2D_ARRAY, G.tex_array);
        set_uniform_i(prog, "u_textures", 1);
    }

    glBindVertexArray(G.vao_tri);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo_tri);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t)count * sizeof(render_vertex_t)),
                 verts, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, count);
}

/* Uploads every ellipsoid of one mode; returns how many. */
static int stage_ellipsoids(int mode) {
    static gl_instance_t *inst = NULL;
    static int inst_cap = 0;

    int n = render_frame.ellipsoid_count;
    if (n <= 0) return 0;
    if (inst_cap < n) {
        free(inst);
        inst = (gl_instance_t *)malloc((size_t)n * sizeof(gl_instance_t));
        inst_cap = inst ? n : 0;
        if (!inst_cap) return 0;
    }

    int used = 0;
    for (int i = 0; i < n; i++) {
        const render_ellipsoid_t *e = &render_frame.ellipsoids[i];
        if (e->mode != mode) continue;
        gl_instance_t *g = &inst[used++];
        memcpy(g->centre, e->centre, sizeof(g->centre));
        memcpy(g->axes,   e->axes,   sizeof(g->axes));
        memcpy(g->rot,    e->rot,    sizeof(g->rot));
        g->radius   = e->radius;
        g->style[0] = e->colour;
        /* >> 7 keeps the shader's (shade * depth_shade) >> 7 in int range and
         * matches the original's (colour_shade * depth_shade) >> 14. */
        g->style[1] = e->colour_shade >> 7;
    }
    if (!used) return 0;

    glBindVertexArray(G.vao_ell);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo_ell);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t)used * sizeof(gl_instance_t)),
                 inst, GL_STREAM_DRAW);
    return used;
}

static void draw_ellipsoids_solid(void) {
    int used = stage_ellipsoids(RENDER_ELL_SOLID);
    if (!used) return;

    glUseProgram(G.prog_ell);
    set_uniform_m4(G.prog_ell, "u_proj", render_frame.proj);
    bind_tex(0, GL_TEXTURE_2D, G.tex_palette);
    bind_tex(1, GL_TEXTURE_2D, G.tex_shade_map);
    bind_tex(2, GL_TEXTURE_3D, G.tex_shade_tab);
    set_uniform_i(G.prog_ell, "u_palette", 0);
    set_uniform_i(G.prog_ell, "u_shade_map", 1);
    set_uniform_i(G.prog_ell, "u_shade_tab", 2);
    set_uniform_f(G.prog_ell, "u_near", (float)RENDER_NEAR_Z);
    set_uniform_f(G.prog_ell, "u_far",  (float)RENDER_FAR_Z);
    set_uniform_i(G.prog_ell, "u_moving_camera", render_frame.moving_camera ? 1 : 0);
    set_uniform_i(G.prog_ell, "u_enhanced_light", render_enhanced_light ? 1 : 0);

    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, used);
}

/* Shadow, smoke and beam remap what is already drawn: they read the snapshot,
 * depth-test in the shader and never write depth.
 *
 * ECSTATICA_GL_PASSES masks them for debugging (1 shadow, 2 smoke, 4 beam). */
static int modulate_pass_mask(void) {
    static int parsed = 0, mask = 7;
    if (!parsed) {
        parsed = 1;
        const char *e = getenv("ECSTATICA_GL_PASSES");
        if (e && *e) mask = atoi(e);
    }
    return mask;
}

static void draw_ellipsoids_modulated(int mode) {
    int bit = (mode == RENDER_ELL_SHADOW) ? 1 : (mode == RENDER_ELL_SMOKE) ? 2 : 4;
    if (!(modulate_pass_mask() & bit)) return;

    int used = stage_ellipsoids(mode);
    if (!used) return;

    int shader_mode = (mode == RENDER_ELL_SHADOW) ? 1
                    : (mode == RENDER_ELL_SMOKE)  ? 2 : 3;

    glUseProgram(G.prog_ell_mod);
    set_uniform_m4(G.prog_ell_mod, "u_proj", render_frame.proj);
    bind_tex(0, GL_TEXTURE_2D, G.tex_palette);
    bind_tex(1, GL_TEXTURE_2D, G.copy_index);
    bind_tex(2, GL_TEXTURE_2D, G.copy_depth);
    bind_tex(3, GL_TEXTURE_3D, G.tex_shadow_tab);
    set_uniform_i(G.prog_ell_mod, "u_palette", 0);
    set_uniform_i(G.prog_ell_mod, "u_scene_index", 1);
    set_uniform_i(G.prog_ell_mod, "u_scene_depth", 2);
    set_uniform_i(G.prog_ell_mod, "u_shadow_tab", 3);
    set_uniform_f(G.prog_ell_mod, "u_near", (float)RENDER_NEAR_Z);
    set_uniform_f(G.prog_ell_mod, "u_far",  (float)RENDER_FAR_Z);
    set_uniform_i(G.prog_ell_mod, "u_mode", shader_mode);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, used);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

static void composite_2d(bool force_full) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    int win_w = G.fb_w, win_h = G.fb_h;
    platform_gfx_drawable_size(G.plat, &win_w, &win_h);
    glViewport(0, 0, win_w, win_h);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    /* The 3D image first, scaled to the window; then the 2D plane over it at
     * native resolution so text is never resampled. */
    if (!force_full && G.scene_color) {
        glUseProgram(G.prog_composite);
        bind_tex(0, GL_TEXTURE_2D, G.tex_ui_index);
        bind_tex(1, GL_TEXTURE_2D, G.tex_bg_index);
        bind_tex(2, GL_TEXTURE_2D, G.tex_palette);
        set_uniform_i(G.prog_composite, "u_ui_index", 0);
        set_uniform_i(G.prog_composite, "u_bg_index", 1);
        set_uniform_i(G.prog_composite, "u_palette", 2);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, G.scene_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, G.scene_w, G.scene_h,
                          0, 0, win_w, win_h,
                          GL_COLOR_BUFFER_BIT, G.ss > 1 ? GL_LINEAR : GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        glUseProgram(G.prog_composite);
        set_uniform_i(G.prog_composite, "u_force", 0);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        draw_fullscreen();
        glDisable(GL_BLEND);
    } else {
        glUseProgram(G.prog_composite);
        bind_tex(0, GL_TEXTURE_2D, G.tex_ui_index);
        bind_tex(1, GL_TEXTURE_2D, G.tex_bg_index ? G.tex_bg_index : G.tex_ui_index);
        bind_tex(2, GL_TEXTURE_2D, G.tex_palette);
        set_uniform_i(G.prog_composite, "u_ui_index", 0);
        set_uniform_i(G.prog_composite, "u_bg_index", 1);
        set_uniform_i(G.prog_composite, "u_palette", 2);
        set_uniform_i(G.prog_composite, "u_force", 1);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        draw_fullscreen();
    }

    glDepthMask(GL_TRUE);
}

/* ── Frame entry points ───────────────────────────────────── */

void render_gl_frame_begin(void) {
    if (!G.active) return;

    /* 0 = match the drawable: on Retina a 640x480 window is 1280x960, and
     * rendering at engine size would throw half of it away. */
    int ss = render_supersample;
    if (ss <= 0) {
        int dw = 0, dh = 0;
        platform_gfx_drawable_size(G.plat, &dw, &dh);
        ss = (screen_width > 0 && dw > 0) ? (dw + screen_width / 2) / screen_width : 1;
    }
    if (ss < 1) ss = 1;
    if (ss > 4) ss = 4;
    G.ss = ss;

    if (!ensure_scene_target(screen_width * ss, screen_height * ss)) {
        /* Nothing sane left to draw into; drop back rather than spin. */
        render_backend = RENDER_SOFTWARE;
        return;
    }

    if (G.pal_dirty || memcmp(G.pal_cache, view_cmap, 768) != 0) upload_palette();
    if (G.bg_dirty || !G.tex_bg_index) upload_background();

    glBindFramebuffer(GL_FRAMEBUFFER, G.scene_fbo);
    glViewport(0, 0, G.scene_w, G.scene_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    if (render_map3d) {
        draw_map3d();
    } else {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        draw_background();
    }
    G.have_scene = true;
}

#ifdef ENABLE_FRAME_DUMP
/* ECSTATICA_GL_DUMP=n1,n2,...: write presented frames as PPM. win.c's dump
 * shows only the 2D plane under this renderer; this is the comparable one. */
int render_gl_dump_after_cut = 0;   /* set by upload_background, counted down here */

static void maybe_dump_frame(void) {
    static int parsed = 0, frames[8], nframes = 0, counter = 0;
    static int on_cut = 0;
    static int seq = 0;

    if (!parsed) {
        parsed = 1;
        const char *e = getenv("ECSTATICA_GL_DUMP");
        if (e && strcmp(e, "cut") == 0) {
            /* The frames after a camera change. */
            on_cut = 1;
        } else {
            while (e && *e && nframes < 8) {
                frames[nframes++] = atoi(e);
                const char *c = strchr(e, ',');
                e = c ? c + 1 : NULL;
            }
        }
    }
    counter++;
    int wanted = 0;
    for (int i = 0; i < nframes; i++) if (frames[i] == counter) wanted = 1;
    if (on_cut && render_gl_dump_after_cut > 0) {
        render_gl_dump_after_cut--;
        wanted = 1;
    }
    if (!wanted) return;

    int w = 0, h = 0;
    platform_gfx_drawable_size(G.plat, &w, &h);
    if (w <= 0 || h <= 0) return;

    uint8_t *px = (uint8_t *)malloc((size_t)w * h * 3);
    if (!px) return;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);

    char name[64];
    snprintf(name, sizeof(name), "gl_dump_%03d.ppm", seq++);
    FILE *f = fopen(name, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        /* GL origin is bottom-left; PPM is top-down. */
        for (int y = h - 1; y >= 0; y--)
            fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
        fclose(f);
        DBG_LOG(1, "[GL] wrote %s (%dx%d) at frame %d\n", name, w, h, counter);
    }
    free(px);
}
#endif

void render_gl_frame_end(void) {
    if (!G.active) return;

    if (render_frame.have_3d) {
        glBindFramebuffer(GL_FRAMEBUFFER, G.scene_fbo);
        glViewport(0, 0, G.scene_w, G.scene_h);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);

        /* draw_parts' order: shadows on the background, solid geometry, smoke,
         * then beams, the brightest. */
        snapshot_scene();
        draw_ellipsoids_modulated(RENDER_ELL_SHADOW);

        draw_ellipsoids_solid();
        draw_triangles(render_frame.flat_verts, render_frame.flat_count, false);
        draw_triangles(render_frame.tex_verts,  render_frame.tex_count,  true);

        snapshot_scene();
        draw_ellipsoids_modulated(RENDER_ELL_SMOKE);
        draw_ellipsoids_modulated(RENDER_ELL_BEAM);

        if (debug_verbose >= 2) {
            DBG_LOG(2, "[GL] frame cam=%d ell=%d flat=%d tex=%d\n",
                    (int)selected_camera, render_frame.ellipsoid_count,
                    render_frame.flat_count / 3, render_frame.tex_count / 3);
        }
    }

    /* A flip with no new 3D re-presents the scene target: present_delay and
     * the menus flip without prepare_parts during camera changes. The full
     * plane path is only for before any 3D frame (logos, title). */
    upload_ui_plane();
    composite_2d(!G.have_scene);

#ifdef ENABLE_FRAME_DUMP
    maybe_dump_frame();
#endif

    platform_gfx_swap(G.plat);
}

void render_gl_invalidate_background(void) { G.bg_dirty  = true; }
void render_gl_invalidate_palette(void)    { G.pal_dirty = true; }

/* ── Lifecycle ────────────────────────────────────────────── */

static bool build_programs(void) {
    G.prog_bg        = link_program(VS_FULLSCREEN, FS_BACKGROUND, "background");
    G.prog_composite = link_program(VS_FULLSCREEN, FS_COMPOSITE,  "composite");
    G.prog_flat      = link_program(VS_TRI,        FS_TRI_FLAT,   "flat triangle");
    G.prog_tex       = link_program(VS_TRI,        FS_TRI_TEX,    "textured triangle");
    G.prog_ell       = link_program(VS_ELLIPSOID,  FS_ELLIPSOID,  "ellipsoid");
    G.prog_ell_mod   = link_program(VS_ELLIPSOID,  FS_ELL_MODULATE, "ellipsoid modulate");
    G.prog_map       = link_program(VS_MAP,        FS_MAP,        "debug map");

    return G.prog_bg && G.prog_composite && G.prog_flat && G.prog_tex &&
           G.prog_ell && G.prog_ell_mod && G.prog_map;
}

static void build_geometry(void) {
    glGenVertexArrays(1, &G.vao_empty);

    /* Triangle stream: world position, texel UV, palette index, texture layer. */
    glGenVertexArrays(1, &G.vao_tri);
    glGenBuffers(1, &G.vbo_tri);
    glBindVertexArray(G.vao_tri);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo_tri);
    {
        GLsizei stride = (GLsizei)sizeof(render_vertex_t);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                              (void *)(size_t)offsetof(render_vertex_t, pos));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
                              (void *)(size_t)offsetof(render_vertex_t, uv));
        glEnableVertexAttribArray(2);
        glVertexAttribIPointer(2, 1, GL_UNSIGNED_BYTE, stride,
                               (void *)(size_t)offsetof(render_vertex_t, pal));
        glEnableVertexAttribArray(3);
        glVertexAttribIPointer(3, 1, GL_SHORT, stride,
                               (void *)(size_t)offsetof(render_vertex_t, layer));
    }

    /* Ellipsoid instances: one attribute per row so the quad is generated from
     * gl_VertexID and there is no per-vertex buffer at all. */
    glGenVertexArrays(1, &G.vao_ell);
    glGenBuffers(1, &G.vbo_ell);
    glBindVertexArray(G.vao_ell);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo_ell);
    {
        GLsizei stride = (GLsizei)sizeof(gl_instance_t);
        struct { int loc, size; GLenum type; size_t off; } a[] = {
            { 0, 3, GL_FLOAT, offsetof(gl_instance_t, centre) },
            { 1, 3, GL_FLOAT, offsetof(gl_instance_t, axes)   },
            { 2, 3, GL_FLOAT, offsetof(gl_instance_t, rot) + 0 * sizeof(float) },
            { 3, 3, GL_FLOAT, offsetof(gl_instance_t, rot) + 3 * sizeof(float) },
            { 4, 3, GL_FLOAT, offsetof(gl_instance_t, rot) + 6 * sizeof(float) },
            { 5, 1, GL_FLOAT, offsetof(gl_instance_t, radius) },
        };
        for (int i = 0; i < 6; i++) {
            glEnableVertexAttribArray((GLuint)a[i].loc);
            glVertexAttribPointer((GLuint)a[i].loc, a[i].size, a[i].type, GL_FALSE,
                                  stride, (void *)a[i].off);
            glVertexAttribDivisor((GLuint)a[i].loc, 1);
        }
        glEnableVertexAttribArray(6);
        glVertexAttribIPointer(6, 2, GL_INT, stride,
                               (void *)(size_t)offsetof(gl_instance_t, style));
        glVertexAttribDivisor(6, 1);
    }

    /* Debug map: position plus a straight RGB colour. */
    glGenVertexArrays(1, &G.vao_map);
    glGenBuffers(1, &G.vbo_map);
    glBindVertexArray(G.vao_map);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo_map);
    {
        GLsizei stride = (GLsizei)sizeof(render_map_vertex_t);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                              (void *)(size_t)offsetof(render_map_vertex_t, pos));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                              (void *)(size_t)offsetof(render_map_vertex_t, rgb));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                              (void *)(size_t)offsetof(render_map_vertex_t, uv));
        glEnableVertexAttribArray(3);
        glVertexAttribIPointer(3, 1, GL_INT, stride,
                               (void *)(size_t)offsetof(render_map_vertex_t, layer));
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

bool render_gl_init(platform_t *p) {
    memset(&G, 0, sizeof(G));
    G.plat = p;
    G.ss   = 1;
    for (int i = 0; i < TEXTURE_TAB_SIZE; i++) G.tex_layer[i] = -1;

    if (!platform_gfx_create(p)) {
        DBG_LOG(1, "[GL] no context\n");
        return false;
    }
    platform_gfx_make_current(p);

    if (!gl_load(platform_gl_proc)) {
        DBG_LOG(1, "[GL] driver is short of OpenGL 3.3\n");
        platform_gfx_destroy(p);
        return false;
    }

    DBG_LOG(1, "[GL] %s | %s | %s\n",
            (const char *)glGetString(GL_VENDOR),
            (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_VERSION));

    if (!build_programs()) {
        platform_gfx_destroy(p);
        return false;
    }
    build_geometry();
    upload_static_tables();
    upload_palette();

    /* Keep the context, give back the surface: this only probed for GL 3.3. */
    platform_gfx_set_active(p, false);

    G.ready = true;
    return true;
}

bool render_gl_start(void) {
    if (!G.ready) return false;
    /* set_active attaches the drawable and makes the context current. */
    platform_gfx_set_active(G.plat, true);
    platform_gfx_make_current(G.plat);
    G.active     = true;
    G.bg_dirty   = true;
    G.pal_dirty  = true;
    G.have_scene = false;
    return true;
}

void render_gl_stop(void) {
    G.active = false;
    /* Give the surface back, or the software blit paints into a GL window. */
    platform_gfx_set_active(G.plat, false);
}

void render_gl_shutdown(void) {
    if (!G.ready) return;
    destroy_scene_target();
    platform_gfx_set_active(G.plat, false);
    platform_gfx_destroy(G.plat);
    G.ready = false;
}

#endif /* ECS_ENABLE_GL */
