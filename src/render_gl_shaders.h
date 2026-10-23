/**
 * render_gl_shaders.h
 *
 * GLSL 330 sources as string literals, so no loose .glsl files ship beside
 * the .FAN archives. Every shader ends at a palette index expanded through
 * view_cmap: the original shade_map / shade_tab / palette chain is kept rather
 * than replaced by a modern lighting model.
 */

#ifndef RENDER_GL_SHADERS_H
#define RENDER_GL_SHADERS_H

#ifdef ECS_ENABLE_GL

/* Palette index to RGB. The palette is GL_RGB8, so this is a sampler2D, not a
 * usampler2D — a mismatched sampler type is undefined (saturated primaries). */
#define ECS_GLSL_PALETTE \
    "uniform sampler2D u_palette;\n" \
    "vec3 pal_rgb(uint idx) {\n" \
    "    return texelFetch(u_palette, ivec2(int(idx), 0), 0).rgb;\n" \
    "}\n"

/* Scene passes write RGB and the source palette index. Shadow, smoke and beam
 * remap the destination index through SHADOW.DAT, which is index-to-index. */
#define ECS_GLSL_DUAL_OUT \
    "layout(location = 0) out vec4 o_col;\n" \
    "layout(location = 1) out uint o_idx;\n"

/* ── Full-screen pass ──────────────────────────────────────────
 * One triangle from gl_VertexID; used by the background and the 2D composite.
 */
static const char *VS_FULLSCREEN =
    "#version 330 core\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    /* Engine row 0 is the top, GL row 0 the bottom. The 3D passes need no
     * flip: build_proj_matrix already negates Y. */
    "    v_uv = vec2(p.x, 1.0 - p.y);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/* ── Background ────────────────────────────────────────────────
 * Colour from bitmap[3], linear int16 view-space depth from mask_map[2].
 * gl_FragDepth converts it to build_proj_matrix's non-linear range, the one
 * place the two depth conventions meet.
 */
static const char *FS_BACKGROUND =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    ECS_GLSL_DUAL_OUT
    "uniform usampler2D u_bg_index;\n"
    "uniform isampler2D u_bg_depth;\n"
    "uniform float u_near;\n"
    "uniform float u_far;\n"
    ECS_GLSL_PALETTE
    "void main() {\n"
    "    ivec2 sz = textureSize(u_bg_index, 0);\n"
    "    ivec2 tc = ivec2(v_uv * vec2(sz));\n"
    "    tc = clamp(tc, ivec2(0), sz - 1);\n"
    "    uint idx = texelFetch(u_bg_index, tc, 0).r;\n"
    "    o_col = vec4(pal_rgb(idx), 1.0);\n"
    "    o_idx = idx;\n"
    "    float z = float(texelFetch(u_bg_depth, tc, 0).r);\n"
    /* The engine clears the mask to 0x7FFF for 'nothing here'; anything at or
     * past the far plane must not occlude, so it is pinned to the far end. */
    "    if (z >= u_far || z <= 0.0) { gl_FragDepth = 1.0; return; }\n"
    "    z = max(z, u_near);\n"
    "    float ndc = (u_far + u_near) / (u_far - u_near)\n"
    "              - 2.0 * u_far * u_near / ((u_far - u_near) * z);\n"
    "    gl_FragDepth = clamp(ndc * 0.5 + 0.5, 0.0, 1.0);\n"
    "}\n";

/* ── 2D composite ──────────────────────────────────────────────
 * The engine's 8bpp plane (menus, subtitles, HUD) drawn on top. A pixel counts
 * as 2D where it differs from the resident background texture. u_force = 1 on
 * menu screens, where the whole plane is the frame.
 */
static const char *FS_COMPOSITE =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 o_col;\n"
    "uniform usampler2D u_ui_index;\n"
    "uniform usampler2D u_bg_index;\n"
    "uniform int u_force;\n"
    ECS_GLSL_PALETTE
    "void main() {\n"
    "    ivec2 sz = textureSize(u_ui_index, 0);\n"
    "    ivec2 tc = ivec2(v_uv * vec2(sz));\n"
    "    tc = clamp(tc, ivec2(0), sz - 1);\n"
    "    uint ui = texelFetch(u_ui_index, tc, 0).r;\n"
    "    if (u_force == 0) {\n"
    "        uint bg = texelFetch(u_bg_index, tc, 0).r;\n"
    "        if (ui == bg) discard;\n"
    "    }\n"
    "    o_col = vec4(pal_rgb(ui), 1.0);\n"
    "}\n";

/* ── Flat triangles ────────────────────────────────────────────
 * The palette index comes from the CPU (face_palette_index).
 */
static const char *VS_TRI =
    "#version 330 core\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "layout(location = 1) in vec2 a_uv;\n"
    "layout(location = 2) in uint a_pal;\n"
    "layout(location = 3) in int  a_layer;\n"
    "uniform mat4 u_view;\n"
    "uniform mat4 u_proj;\n"
    "flat out uint v_pal;\n"
    "flat out int  v_layer;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    v_pal   = a_pal;\n"
    "    v_layer = a_layer;\n"
    "    v_uv    = a_uv;\n"
    "    gl_Position = u_proj * (u_view * vec4(a_pos, 1.0));\n"
    "}\n";

static const char *FS_TRI_FLAT =
    "#version 330 core\n"
    "flat in uint v_pal;\n"
    ECS_GLSL_DUAL_OUT
    ECS_GLSL_PALETTE
    "void main() { o_col = vec4(pal_rgb(v_pal), 1.0); o_idx = v_pal; }\n";

/* Textured faces are unlit, as tex_tri_line_win95. Nearest integer sampling:
 * filtering palette indices would blend unrelated colours. */
static const char *FS_TRI_TEX =
    "#version 330 core\n"
    "flat in int v_layer;\n"
    "in vec2 v_uv;\n"
    ECS_GLSL_DUAL_OUT
    "uniform usampler2DArray u_textures;\n"
    ECS_GLSL_PALETTE
    "void main() {\n"
    "    ivec2 t = ivec2(v_uv) & ivec2(127);\n"
    "    uint idx = texelFetch(u_textures, ivec3(t, v_layer), 0).r;\n"
    "    o_col = vec4(pal_rgb(idx), 1.0);\n"
    "    o_idx = idx;\n"
    "}\n";

/* ── Debug map ─────────────────────────────────────────────────
 * Straight RGB, not the palette; o_idx is still written for the modulating
 * passes.
 */
static const char *VS_MAP =
    "#version 330 core\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "layout(location = 1) in vec3 a_rgb;\n"
    "layout(location = 2) in vec2 a_uv;\n"
    "layout(location = 3) in int  a_layer;\n"
    "uniform mat4 u_view;\n"
    "uniform mat4 u_proj;\n"
    "out vec3 v_rgb;\n"
    "out vec2 v_uv;\n"
    "flat out int v_layer;\n"
    "void main() {\n"
    "    v_rgb = a_rgb;\n"
    "    v_uv = a_uv;\n"
    "    v_layer = a_layer;\n"
    "    gl_Position = u_proj * (u_view * vec4(a_pos, 1.0));\n"
    "}\n";

/* Textured faces sample E2's 128x128 palette textures, nearest, and scale
 * the palette colour by the face shade. */
static const char *FS_MAP =
    "#version 330 core\n"
    "in vec3 v_rgb;\n"
    "in vec2 v_uv;\n"
    "flat in int v_layer;\n"
    ECS_GLSL_DUAL_OUT
    "uniform usampler2DArray u_textures;\n"
    "uniform usampler2D u_bump;\n"
    ECS_GLSL_PALETTE
    "void main() {\n"
    /* -2: stone, the re-release's bumpmap.raw as mortar between blocks. */
    "    if (v_layer == -2) {\n"
    "        ivec2 b = ivec2(floor(v_uv)) & ivec2(127);\n"
    "        float m = float(texelFetch(u_bump, b, 0).r) / 255.0;\n"
    "        o_col = vec4(v_rgb * (1.0 - 0.55 * m), 1.0); o_idx = 0u; return;\n"
    "    }\n"
    "    if (v_layer < 0) { o_col = vec4(v_rgb, 1.0); o_idx = 0u; return; }\n"
    "    ivec2 t = ivec2(floor(v_uv)) & ivec2(127);\n"
    "    uint idx = texelFetch(u_textures, ivec3(t, v_layer), 0).r;\n"
    "    o_col = vec4(pal_rgb(idx) * v_rgb, 1.0);\n"
    "    o_idx = idx;\n"
    "}\n";

/* The 3D map's sky: the re-release's hiressky.raw stretched over the screen. */
static const char *FS_SKY =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    ECS_GLSL_DUAL_OUT
    "uniform sampler2D u_sky;\n"
    "void main() {\n"
    "    o_col = vec4(texture(u_sky, v_uv).rgb, 1.0);\n"
    "    o_idx = 0u;\n"
    "}\n";

/* Map ellipsoids: the same impostor as the actors' below, from world-space
 * axis vectors, lit by a fixed light instead of the shade tables. */
static const char *VS_MAP_ELL =
    "#version 330 core\n"
    "layout(location = 0) in vec3 a_centre;\n"
    "layout(location = 1) in vec3 a_ax;\n"
    "layout(location = 2) in vec3 a_ay;\n"
    "layout(location = 3) in vec3 a_az;\n"
    "layout(location = 4) in vec3 a_rgb;\n"
    "uniform mat4 u_view;\n"
    "uniform mat4 u_proj;\n"
    "uniform float u_near;\n"
    "flat out vec3 v_centre;\n"
    "flat out mat3 v_inv;\n"
    "flat out vec3 v_rgb;\n"
    "out vec3 v_ray;\n"
    "void main() {\n"
    "    mat3 V = mat3(u_view);\n"
    "    mat3 A = mat3(V * a_ax, V * a_ay, V * a_az);\n"
    "    vec3 c = (u_view * vec4(a_centre, 1.0)).xyz;\n"
    "    float r = max(length(a_ax), max(length(a_ay), length(a_az)));\n"
    "    v_centre = c;\n"
    "    v_inv = inverse(A);\n"
    "    v_rgb = a_rgb;\n"
    "    if (c.z + r < u_near) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); v_ray = vec3(0.0); return; }\n"
    /* A quad facing the eye, as wide as the ellipsoid's extent along each of
     * its edges (sqrt of the summed squared axis projections), so thin
     * needles do not pay for a square; grown by the perspective of its
     * nearest point. */
    "    vec3 dir = normalize(c);\n"
    "    vec3 lng = A[2] - dir * dot(A[2], dir);\n"
    "    vec3 s = length(lng) > 1e-3 * r ? normalize(lng) : normalize(cross(dir, vec3(1.0, 0.0, 0.0)));\n"
    "    vec3 t = cross(dir, s);\n"
    "    vec3 ps = s * A, pt = t * A;\n"
    "    float k = c.z / max(c.z - r, u_near) * 1.05;\n"
    "    float es = length(ps) * k, et = length(pt) * k;\n"
    "    vec2 corner = vec2((gl_VertexID & 1) == 0 ? -1.0 : 1.0,\n"
    "                       (gl_VertexID & 2) == 0 ? -1.0 : 1.0);\n"
    "    vec3 vp = c + s * (corner.x * es) + t * (corner.y * et);\n"
    "    vp.z = max(vp.z, u_near);\n"
    "    v_ray = vp;\n"
    "    gl_Position = u_proj * vec4(vp, 1.0);\n"
    "}\n";

static const char *FS_MAP_ELL =
    "#version 330 core\n"
    "flat in vec3 v_centre;\n"
    "flat in mat3 v_inv;\n"
    "flat in vec3 v_rgb;\n"
    "in vec3 v_ray;\n"
    ECS_GLSL_DUAL_OUT
    "uniform float u_near;\n"
    "uniform float u_far;\n"
    "uniform vec3 u_light;\n"
    "void main() {\n"
    "    vec3 d = normalize(v_ray);\n"
    "    vec3 o2 = -(v_inv * v_centre);\n"
    "    vec3 d2 = v_inv * d;\n"
    "    float a = dot(d2, d2);\n"
    "    float b = 2.0 * dot(o2, d2);\n"
    "    float c = dot(o2, o2) - 1.0;\n"
    "    float disc = b * b - 4.0 * a * c;\n"
    "    if (disc < 0.0) discard;\n"
    "    float sq = sqrt(disc);\n"
    "    float t = (-b - sq) / (2.0 * a);\n"
    "    if (t <= 0.0) t = (-b + sq) / (2.0 * a);\n"
    "    if (t <= 0.0) discard;\n"
    "    vec3 u = o2 + t * d2;\n"
    "    vec3 p = t * d;\n"
    "    if (p.z < u_near) discard;\n"
    "    float ndc = (u_far + u_near) / (u_far - u_near)\n"
    "              - 2.0 * u_far * u_near / ((u_far - u_near) * p.z);\n"
    "    gl_FragDepth = clamp(ndc * 0.5 + 0.5, 0.0, 1.0);\n"
    "    vec3 n = normalize(transpose(v_inv) * u);\n"
    "    float l = 0.35 + 0.75 * max(dot(n, u_light), 0.0);\n"
    "    o_col = vec4(min(v_rgb * l, vec3(1.0)), 1.0);\n"
    "    o_idx = 0u;\n"
    "}\n";

/* ── Ellipsoids ────────────────────────────────────────────────
 * A screen-aligned quad per instance; the fragment shader intersects the view
 * ray with the quadric for an exact silhouette and per-pixel depth. The quad
 * covers the bounding sphere of centre + rot * diag(axes) * u.
 */
static const char *VS_ELLIPSOID =
    "#version 330 core\n"
    "layout(location = 0) in vec3 a_centre;\n"
    "layout(location = 1) in vec3 a_axes;\n"
    "layout(location = 2) in vec3 a_rot0;\n"     /* rows of the view-space R */
    "layout(location = 3) in vec3 a_rot1;\n"
    "layout(location = 4) in vec3 a_rot2;\n"
    "layout(location = 5) in float a_radius;\n"
    "layout(location = 6) in ivec2 a_style;\n"   /* colour, colour_shade >> 7 */
    "uniform mat4 u_proj;\n"
    "flat out vec3 v_centre;\n"
    "flat out vec3 v_rot0;\n"
    "flat out vec3 v_rot1;\n"
    "flat out vec3 v_rot2;\n"
    "flat out vec3 v_axes;\n"
    "flat out ivec2 v_style;\n"
    "out vec3 v_ray;\n"
    "void main() {\n"
    "    v_centre = a_centre;\n"
    "    v_rot0 = a_rot0; v_rot1 = a_rot1; v_rot2 = a_rot2;\n"
    "    v_axes   = a_axes;\n"
    "    v_style  = a_style;\n"
    "    vec2 corner = vec2((gl_VertexID & 1) == 0 ? -1.0 : 1.0,\n"
    "                       (gl_VertexID & 2) == 0 ? -1.0 : 1.0);\n"
    /* The quad sits at the centre's depth, so the near plane cannot clip it
     * while the centre is in front. A bounding-sphere point is at most radius
     * nearer, which magnifies its offset by centre.z / (centre.z - radius). */
    "    float denom = max(a_centre.z - a_radius, 1.0);\n"
    "    float ext = a_radius * (a_centre.z + length(a_centre.xy)) / denom;\n"
    "    ext = clamp(ext * 1.05, a_radius, 32767.0);\n"
    "    vec3 vp = vec3(a_centre.xy + corner * ext, a_centre.z);\n"
    "    v_ray = vp;\n"
    "    gl_Position = u_proj * vec4(vp, 1.0);\n"
    "}\n";

static const char *FS_ELLIPSOID =
    "#version 330 core\n"
    "flat in vec3 v_centre;\n"
    "flat in vec3 v_rot0;\n"
    "flat in vec3 v_rot1;\n"
    "flat in vec3 v_rot2;\n"
    "flat in vec3 v_axes;\n"
    "flat in ivec2 v_style;\n"
    "in vec3 v_ray;\n"
    ECS_GLSL_DUAL_OUT
    "uniform usampler2D u_shade_map;\n"
    "uniform usampler3D u_shade_tab;\n"
    "uniform float u_near;\n"
    "uniform float u_far;\n"
    "uniform int u_moving_camera;\n"
    "uniform int u_enhanced_light;\n"
    ECS_GLSL_PALETTE
    "void main() {\n"
    "    mat3 Rt = mat3(v_rot0, v_rot1, v_rot2);\n"  /* columns = rows of R */
    "    vec3 d = normalize(v_ray);\n"
    /* u = (R^T (p - c)) / axes; Rt * v applies the inverse rotation. */
    "    vec3 o2 = -(Rt * v_centre) / v_axes;\n"
    "    vec3 d2 =  (Rt * d) / v_axes;\n"
    "    float a = dot(d2, d2);\n"
    "    float b = 2.0 * dot(o2, d2);\n"
    "    float c = dot(o2, o2) - 1.0;\n"
    "    float disc = b * b - 4.0 * a * c;\n"
    "    if (disc < 0.0) discard;\n"           /* the exact silhouette */
    "    float sq = sqrt(disc);\n"
    "    float t = (-b - sq) / (2.0 * a);\n"
    "    if (t <= 0.0) t = (-b + sq) / (2.0 * a);\n"
    "    if (t <= 0.0) discard;\n"
    "    vec3 u = o2 + t * d2;\n"
    "    vec3 p = t * d;\n"                    /* view-space hit; eye at origin */
    "    if (p.z < u_near) discard;\n"
    "    float ndc = (u_far + u_near) / (u_far - u_near)\n"
    "              - 2.0 * u_far * u_near / ((u_far - u_near) * p.z);\n"
    "    gl_FragDepth = clamp(ndc * 0.5 + 0.5, 0.0, 1.0);\n"
    /* Normal: R * (u / axes) up to scale. The original's lighting is
     * screen-fixed (shade_map in the projected disc frame); the view-space
     * normal reproduces that. Enhanced lighting uses the object-space
     * direction so parts light consistently as they turn. */
    "    vec3 n = normalize((u / v_axes) * Rt);\n"
    "    vec2 sc = (u_enhanced_light != 0) ? normalize(u).xy : n.xy;\n"
    "    ivec2 smp = ivec2(clamp(sc * 64.0 + 64.0, vec2(0.0), vec2(127.0)));\n"
    "    uint sm = texelFetch(u_shade_map, smp, 0).r;\n"
    "    int shade = int(sm & 0x7Fu);\n"
    /* ellipse.c:133 — the fog band base depends on camera motion, or parts
     * jump brightness on a cut. colour_shade arrives pre-shifted by 7. */
    "    int zi = int(p.z);\n"
    "    int depth_shade = (u_moving_camera != 0) ? (159 - (zi >> 5))\n"
    "                                             : (191 - (zi >> 7));\n"
    "    depth_shade = clamp(depth_shade, 0, 127);\n"
    "    int band = clamp((v_style.y * depth_shade) >> 7, 0, 127);\n"
    "    uint idx = texelFetch(u_shade_tab,\n"
    "                          ivec3(shade, band, v_style.x), 0).r;\n"
    "    o_col = vec4(pal_rgb(idx), 1.0);\n"
    "    o_idx = idx;\n"
    "}\n";

/* ── Shadows, smoke and beams ──────────────────────────────────
 * Remap what is already drawn through a table, using the conditions of the
 * asm_f.c span routines on the two roots of the quadric:
 *
 *   smoke  (asm_f.c:425)  near <= scene            -> table 0
 *   shadow (asm_f.c:386)  near <= scene <= far     -> table 1
 *   beam   (asm_f.c:346)  near <= scene            -> table 2 if also <= far,
 *                                                     else table 0
 *
 * No depth test can express these, so the shader reads scene depth.
 */
static const char *FS_ELL_MODULATE =
    "#version 330 core\n"
    "flat in vec3 v_centre;\n"
    "flat in vec3 v_rot0;\n"
    "flat in vec3 v_rot1;\n"
    "flat in vec3 v_rot2;\n"
    "flat in vec3 v_axes;\n"
    "flat in ivec2 v_style;\n"
    "in vec3 v_ray;\n"
    ECS_GLSL_DUAL_OUT
    "uniform usampler2D u_scene_index;\n"
    "uniform sampler2D  u_scene_depth;\n"
    "uniform usampler3D u_shadow_tab;\n"   /* 256 x 16 x 3 */
    "uniform float u_near;\n"
    "uniform float u_far;\n"
    "uniform int u_mode;\n"                /* 1 shadow, 2 smoke, 3 beam */
    ECS_GLSL_PALETTE
    "void main() {\n"
    "    mat3 Rt = mat3(v_rot0, v_rot1, v_rot2);\n"
    "    vec3 d = normalize(v_ray);\n"
    "    vec3 o2 = -(Rt * v_centre) / v_axes;\n"
    "    vec3 d2 =  (Rt * d) / v_axes;\n"
    "    float a = dot(d2, d2);\n"
    "    float b = 2.0 * dot(o2, d2);\n"
    "    float c = dot(o2, o2) - 1.0;\n"
    "    float disc = b * b - 4.0 * a * c;\n"
    "    if (disc < 0.0) discard;\n"
    "    float sq = sqrt(disc);\n"
    "    float t_near = (-b - sq) / (2.0 * a);\n"
    "    float t_far  = (-b + sq) / (2.0 * a);\n"
    "    if (t_far <= 0.0) discard;\n"
    "    float z_near = (t_near * d).z;\n"
    "    float z_far  = (t_far  * d).z;\n"
    /* Recover the scene's view-space Z from the stored depth — the inverse of
     * the mapping build_proj_matrix and the background pass both apply. */
    "    ivec2 tc = ivec2(gl_FragCoord.xy);\n"
    "    float dv = texelFetch(u_scene_depth, tc, 0).r;\n"
    "    float ndc = dv * 2.0 - 1.0;\n"
    "    float denom = (u_far + u_near) - ndc * (u_far - u_near);\n"
    "    if (denom <= 0.0) discard;\n"
    "    float z_scene = 2.0 * u_far * u_near / denom;\n"
    "    if (z_near > z_scene) discard;\n"
    "    int table;\n"
    "    if (u_mode == 1) {\n"
    "        if (z_far < z_scene) discard;\n"
    "        table = 1;\n"
    "    } else if (u_mode == 2) {\n"
    "        table = 0;\n"
    "    } else {\n"
    "        table = (z_far >= z_scene) ? 2 : 0;\n"
    "    }\n"
    "    uint src = texelFetch(u_scene_index, tc, 0).r;\n"
    "    uint idx = texelFetch(u_shadow_tab,\n"
    "                          ivec3(int(src), v_style.x, table), 0).r;\n"
    "    o_col = vec4(pal_rgb(idx), 1.0);\n"
    "    o_idx = idx;\n"
    "}\n";

#endif /* ECS_ENABLE_GL */
#endif /* RENDER_GL_SHADERS_H */
