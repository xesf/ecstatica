/**
 * ellipse.c
 *
 * Ellipsoid rendering: shade_ellipse (column-based shade_map rendering),
 * draw_triangle_ell (flat-shaded triangle with z-buffer), fixed-point
 * arctan/arcsin, calculate_squash and find_ellipse (ellipsoid projection).
 */

#include "ellipse.h"
#include "asm_f.h"
#include "display.h"
#include "game.h"
#include "init.h"
#include "raster.h"
#include "topo.h"
#include <string.h>
#include <stdlib.h>
#include "compat.h"
#include <math.h>

/* ellipse_shade_ellipse  E1: 0x429A00 | E2: 0x430750 */
void shade_ellipse(part_t *part, int plane) {
    check_fade();
    shade_ellipse_win95(part, plane);
}

/* ellipse_shade_ellipse_win95_430FD8 — column-by-column shade_map ellipsoid;
 * part flags select the shadow, smoke and beam modes. */
void shade_ellipse_win95(part_t *part, int plane) {
    if (raster_recording)
        raster_record_ellipse(part, plane);
    else
        shade_ellipse_rows(part, plane, NULL, top_edge, bottom_edge);
}

/* top_edge and bottom_edge shadow the globals, so the body clips to the band. */
void shade_ellipse_rows(part_t *part, int plane, subarea_t *dirty, int top_edge, int bottom_edge) {
    const int mask_stride = screen_width;
    if (!part->vector_persp.Z) return;

    int pitch;
    char *plane_data = (char *)dd_lock(plane, &pitch);
    column_state_t cs;
    memset(&cs, 0, sizeof(cs));

    int z_off_x = part->depth_offset.X;
    int z_off_y = part->depth_offset.Y;
    int z_off_z = part->depth_offset.Z;

    int16_t pos_x = part->vector_persp.X;
    int16_t pos_y = part->vector_persp.Y;
    int16_t pos_z = part->vector_persp.Z;

    /* Compute projected half-sizes in pixels */
    int projection = (zoom_factor >> 4) / part->vector_persp.Z;
    int half_x = (int)((int64_t)part->projected_axes.X * projection * view_width / 320 >> 6);
    /* Both modes take 7/8 of the projection vertically; only SVGA adds the
     * 12/5 aspect term (0x429AA1-0x429AC4 VGA, 0x42A15F-0x42A179 win95). */
    int projection78 = projection - projection / 8;
    int half_y, half_z;
    if (screen_width <= 320) {
        half_y = (part->projected_axes.Y * projection78) >> 6;
        half_z = (part->projected_axes.Z * projection78) >> 6;
    } else {
        half_y = ((part->projected_axes.Y * 12 / 5) * projection78) >> 6;
        half_z = ((part->projected_axes.Z * 12 / 5) * projection78) >> 6;
    }
    if (half_y < 4 || half_x < 4) {
        dd_unlock(plane, plane_data);
        return;
    }

    int col_top_fp = (int)(((int64_t)(pos_y - (half_y + half_z)) + 16 * screen_centre_y) << 16);
    /* Widened: 32-bit `half_z << 20` wraps at |half_z| >= 2048, which SVGA's
     * 2.1x half_z reaches on long slanted parts near the camera. */
    int dy_per_col = (int)(((int64_t)half_z << 20) / half_x);

    int step_y = 0x4000000 / half_y;
    int step_x = 0x4000000 / half_x;

    int16_t ellipse_left = (screen_centre_x << 4) + pos_x - half_x;

    int z_step_x = (int)(((int64_t)z_off_x << 20) / half_x);
    int z_step_y = (int)(((int64_t)z_off_y << 20) / half_y);

    half_z = abs(half_z);

    int bound_top    = screen_centre_y + ((pos_y - (half_y + half_z)) >> 4);
    int bound_bottom = screen_centre_y + ((pos_y + (half_y + half_z)) >> 4) + 1;
    int bound_left   = screen_centre_x + ((pos_x - half_x) >> 4);
    int bound_right  = screen_centre_x + ((pos_x + half_x) >> 4) + 1;

    int z_col = (pos_z - (z_off_x + z_off_y)) << 16;

    if (bound_left >= right_edge || bound_right < left_edge ||
        bound_top >= bottom_edge || bound_bottom < top_edge) {
        dd_unlock(plane, plane_data);
        return;
    }

    subarea_t *dirty_rect = dirty ? dirty : part->parent_actor->area_to_clear;
    int16_t dirty_l = dirty_rect->left;
    int16_t dirty_r = dirty_rect->right;
    int16_t dirty_t = dirty_rect->top;
    int16_t dirty_b = dirty_rect->bottom;

    cs.z_scale = 2 * z_off_z;
    cs.fb_pitch = pitch;
    cs.depth_mask = (plane == 2) ? mask_map[1] : mask_map[0];

    /* Select rendering mode based on part flags */
    if (part->flags & 0x42) {
        if ((part->flags & 0x42) == 0x42) {
            cs.beam_tab1 = &shadow_tab[0][part->color][0];
            cs.beam_tab2 = &shadow_tab[2][part->color][0];
        } else if (part->flags & 2) {
            cs.shadow_lut = &shadow_tab[1][part->color][0];
        } else {
            cs.beam_tab1 = &shadow_tab[0][part->color][0];
        }
    } else {
        /* 0x430FD8: the shade base depends on moving_camera, or ellipses jump
         * brightness on camera transitions. */
        int depth_shade = moving_camera ? (159 - (pos_z >> 5)) : (191 - (pos_z >> 7));
        if (depth_shade < 0) depth_shade = 0;
        if (depth_shade > 127) depth_shade = 127;
        cs.shade_lut = shade_tab[part->color][part->color_shade * depth_shade >> 14];

    }

    /* Sub-texel X offset */
    int subpix_x = 0xF - (ellipse_left & 0xF);
    int shade_x = subpix_x * step_x >> 4;
    z_col += subpix_x * z_step_x >> 4;

    int16_t ellipse_right = (ellipse_left + 2 * half_x) >> 4;
    if ((ellipse_left >> 4) < dirty_l)
        dirty_l = ellipse_left >> 4;
    if (ellipse_right + 1 > dirty_r)
        dirty_r = ellipse_right + 1;

    int draw_x = ellipse_left >> 4;
    int draw_y = col_top_fp;

    while (draw_x < ellipse_right && draw_x < right_edge) {
        if (draw_x >= left_edge) {
            int mask_idx = draw_x + (draw_y >> 20) * mask_stride;
            char *draw_ptr = plane_data + (draw_y >> 20) * pitch + draw_x;
            int subpix_y = 0xF - ((draw_y >> 16) & 0xF);
            signed int shade_idx = ((shade_x & 0xFFFF0000) * 128) +
                                          (subpix_y * step_y >> 4);
            int16_t clip_top = draw_y >> 20;
            int z_interp = z_col +
                                      (z_step_y * subpix_y >> 4);

            int lines_above = top_edge - clip_top;
            if (lines_above > 0) {
                shade_idx += lines_above * step_y;
                z_interp += lines_above * z_step_y;
                draw_ptr += lines_above * pitch;
                mask_idx += lines_above * mask_stride;
                clip_top = top_edge;
            }

            int16_t col_bottom = (2 * half_y + (draw_y >> 16)) >> 4;
            int16_t clip_bottom = (col_bottom < bottom_edge) ? col_bottom : bottom_edge;

            if (clip_bottom > clip_top) {
                if (clip_top < dirty_t)
                    dirty_t = clip_top;
                if (clip_bottom > dirty_b)
                    dirty_b = clip_bottom;

                cs.shade_dy = step_y;
                cs.z_dy = z_step_y;

                int col_height = (clip_bottom - clip_top - 1) * 256;

                if (part->flags & 0x42) {
                    if ((part->flags & 0x42) == 0x42)
                        beam_line_win95(&cs, mask_idx, col_height, z_interp,
                                           draw_ptr, shade_idx);
                    else if (part->flags & 2)
                        shadow_line_win95(&cs, mask_idx, col_height, z_interp,
                                             draw_ptr, shade_idx);
                    else
                        smoke_line_win95(&cs, mask_idx, col_height, z_interp,
                                            draw_ptr, shade_idx);
                } else {
                    ellipse_line_win95(&cs, mask_idx, col_height, z_interp,
                                       draw_ptr, shade_idx);
                }
            }
        }

        draw_y += dy_per_col;
        shade_x += step_x;
        draw_x++;
        z_col += z_step_x;
    }

    dirty_rect->left  = dirty_l;
    dirty_rect->right = dirty_r;
    dirty_rect->top   = dirty_t;
    dirty_rect->bottom = dirty_b;

    dd_unlock(plane, plane_data);
}

/* ellipse_draw_triangle_431A7C — flat-shaded triangle with z-buffer, drawn
 * column by column with tri_line_win95. */
void draw_triangle_ell(tri_t *tri, int plane, tri_t *shade) {
    if (!tri->point1 || !tri->point2 || !tri->point3) return;

    check_fade();

    if (raster_recording)
        raster_record_tri(0, tri, plane, shade);
    else
        draw_triangle_ell_rows(tri, plane, shade, NULL, top_edge, bottom_edge);
}

/* top_edge and bottom_edge shadow the globals, so the body clips to the band. */
void draw_triangle_ell_rows(tri_t *tri, int plane, tri_t *shade, subarea_t *dirty,
                            int top_edge, int bottom_edge) {
    int x1 = tri->point1->screen_coord.X;
    int y1 = tri->point1->screen_coord.Y;
    int z1 = tri->point1->screen_coord.Z;
    int x2 = tri->point2->screen_coord.X;
    int y2 = tri->point2->screen_coord.Y;
    int z2 = tri->point2->screen_coord.Z;
    int x3 = tri->point3->screen_coord.X;
    int y3 = tri->point3->screen_coord.Y;
    int z3 = tri->point3->screen_coord.Z;

    if (!z1 || !z2 || !z3) return;

    int ref_z = z1;
    int mask_idx = (plane == 2) ? 1 : 0;

    int x_left  = (((x1 < x2 ? x1 : x2) < x3 ? (x1 < x2 ? x1 : x2) : x3) >> 4) + screen_centre_x;
    int x_right = (((x1 > x2 ? x1 : x2) > x3 ? (x1 > x2 ? x1 : x2) : x3) >> 4) + screen_centre_x;
    int y_top_b = (((y1 < y2 ? y1 : y2) < y3 ? (y1 < y2 ? y1 : y2) : y3) >> 4) + screen_centre_y;
    int y_bot_b = (((y1 > y2 ? y1 : y2) > y3 ? (y1 > y2 ? y1 : y2) : y3) >> 4) + screen_centre_y;

    if (x_left >= screen_width || x_right < 0 || y_top_b >= screen_height || y_bot_b < 0)
        return;

    if (tri->parent_actor) {
        subarea_t *area = dirty ? dirty : tri->parent_actor->area_to_clear;
        if (x_left   < area->left)   area->left   = x_left;
        if (x_right  > area->right)  area->right  = x_right;
        if (y_top_b  < area->top)    area->top    = y_top_b;
        if (y_bot_b  > area->bottom) area->bottom = y_bot_b;
    }

    /* Nothing in these rows; the margin covers the rounding of the edges. */
    if (y_bot_b + 2 < top_edge || y_top_b - 2 >= bottom_edge)
        return;

    int cross_x = (z2 - z3) * (y1 - y3) - (z1 - z3) * (y2 - y3);
    int cross_z = (y2 - y3) * (x1 - x3) - (y1 - y3) * (x2 - x3);
    int cross_y = (x2 - x3) * (z1 - z3) - (z2 - z3) * (x1 - x3);
    int tri_color;

    if (cross_z <= 0) {
        if (!(tri->tri_use_flag & 1)) return;
        cross_x = -cross_x;
        cross_y = (z2 - z3) * (x1 - x3) - (x2 - x3) * (z1 - z3);
        tri_color = tri->tri_color_4;
        cross_z = -cross_z;
    } else {
        tri_color = tri->tri_color_3;
    }

    if (shade) {
        int sx1 = shade->point1->screen_coord.X;
        int sy1 = shade->point1->screen_coord.Y;
        int sz1 = shade->point1->screen_coord.Z;
        int sx2 = shade->point2->screen_coord.X;
        int sy2 = shade->point2->screen_coord.Y;
        int sz2 = shade->point2->screen_coord.Z;
        int sx3 = shade->point3->screen_coord.X;
        int sy3 = shade->point3->screen_coord.Y;
        int sz3 = shade->point3->screen_coord.Z;

        cross_x = (sz2 - sz3) * (sy1 - sy3) - (sz1 - sz3) * (sy2 - sy3);
        cross_z = (sy2 - sy3) * (sx1 - sx3) - (sy1 - sy3) * (sx2 - sx3);
        cross_y = (sx2 - sx3) * (sz1 - sz3) - (sz2 - sz3) * (sx1 - sx3);
        ref_z = sz1;
    }

    while (cross_x < -0x4000 || cross_x > 0x4000 || cross_y < -0x4000 || cross_y > 0x4000 ||
           cross_z < -0x4000 || cross_z > 0x4000) {
        cross_x >>= 1; cross_y >>= 1; cross_z >>= 1;
    }

    /* Compute flat shade via shade_map lookup */
    vector_t normal;
    normal.X = cross_x; normal.Y = cross_y; normal.Z = cross_z;

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

    int shade_val = shade_map[shade_row][shade_col];
    int depth_shade;
    if (moving_camera)
        depth_shade = 159 - (ref_z >> 5);
    else
        depth_shade = 191 - (ref_z >> 7);
    if (depth_shade < 0) depth_shade = 0;
    if (depth_shade > 127) depth_shade = 127;

    int shade_idx = shade_val & 0x7F;
    int shade_band = tri->shade_multiplier * depth_shade >> 14;
    int pixel_color;
    if (tri->tri_use_flag & 0x10) {
        pixel_color = (unsigned char)shade_tab[tri_color][127][shade_idx];
    } else {
        pixel_color = (unsigned char)shade_tab[tri_color][shade_band][shade_idx];
    }

    point_t *left_pt, *mid_pt, *right_pt;
    if (x1 <= x2) {
        if (x2 >= x3) {
            if (x1 >= x3) { left_pt = tri->point3; mid_pt = tri->point1; }
            else             { left_pt = tri->point1; mid_pt = tri->point3; }
            right_pt = tri->point2;
        } else {
            left_pt = tri->point1; mid_pt = tri->point2; right_pt = tri->point3;
        }
    } else {
        if (x2 <= x3) {
            if (x1 <= x3) { left_pt = tri->point2; mid_pt = tri->point1; right_pt = tri->point3; }
            else             { left_pt = tri->point2; mid_pt = tri->point3; right_pt = tri->point1; }
        } else {
            left_pt = tri->point3; mid_pt = tri->point2; right_pt = tri->point1;
        }
    }

    int px1 = left_pt->screen_coord.X;
    int py1 = left_pt->screen_coord.Y;
    int pz1 = left_pt->screen_coord.Z;
    int px2 = mid_pt->screen_coord.X;
    int py2 = mid_pt->screen_coord.Y;
    int pz2 = mid_pt->screen_coord.Z;
    int px3 = right_pt->screen_coord.X;
    int py3 = right_pt->screen_coord.Y;
    int pz3 = right_pt->screen_coord.Z;

    int cols_left_mid   = (px2 >> 4) - (px1 >> 4);
    int cols_left_right = (px3 >> 4) - (px1 >> 4);

    /* Edge slopes: dY/dX in 16.16, dZ/dX in <<20 relative to sub-pixel X */
    int dy12 = 0, dz12 = 0;
    int dy13 = 0, dz13 = 0;
    int dy23 = 0, dz23 = 0;

    if (px2 == px1)
        dy12 = (py1 >= py2) ? (int)0x80000001 : 0x7FFFFFFF;
    else {
        dy12 = ((py2 - py1) << 16) / (px2 - px1);
        dz12 = ((pz2 - pz1) << 20) / (px2 - px1);
    }
    if (px3 != px1) {
        dy13 = ((py3 - py1) << 16) / (px3 - px1);
        dz13 = ((pz3 - pz1) << 20) / (px3 - px1);
    }
    if (px3 != px2) {
        dy23 = ((py3 - py2) << 16) / (px3 - px2);
        dz23 = ((pz3 - pz2) << 20) / (px3 - px2);
    }

    /* Per-row Z step (dZ/dY) for within-column interpolation */
    int dz_per_row = 0;
    {
        int z_span = pz1 + ((dz13 * (px2 - px1)) >> 20) - pz2;
        int y_span = ((py1 + ((px2 - px1) * dy13 >> 16)) - py2) >> 4;
        if (y_span) dz_per_row = (z_span << 16) / y_span;
    }

    int pitch;
    char *fb_data = (char *)dd_lock(plane, &pitch);

    int running_z = pz1 << 16;
    int col = screen_centre_x + (px1 >> 4);
    int subpix = 15 - (px1 & 0xF);

    /* Y intercepts in 16.16 fixed-point */
    int y_top_fp  = (screen_centre_y << 16) + (py1 << 12);
    int y_bot_fp  = y_top_fp;
    int y_corr_13 = (subpix * dy13) >> 4;
    int y_corr_12 = (subpix * dy12) >> 4;

    int col_count = 0;

    if (dy12 > dy13) {
        /* Case 1: edge 1→2 is below edge 1→3.
         * Top edge tracks 1→3, bottom edge tracks 1→2 then 2→3. */
        y_top_fp  += y_corr_13;
        running_z += (dz13 * subpix) >> 4;
        y_bot_fp  += y_corr_12;

        while (col_count < cols_left_mid) {
            if (col >= right_edge) break;
            if (col >= left_edge) {
                int ty = y_top_fp >> 16;
                int16_t *mptr = mask_map[mask_idx] + ty * screen_width + col;
                int z_at_row= ((dz_per_row * (15 - (ty & 0xF))) >> 4) + running_z;
                char *fbptr = fb_data + ty * pitch + col;
                int clip = top_edge - ty;
                if (clip > 0) {
                    fbptr += clip * pitch;
                    mptr  += clip * screen_width;
                    z_at_row   += dz_per_row * clip;
                    ty     = top_edge;
                }
                int by = y_bot_fp >> 16;
                if (by > bottom_edge) by = bottom_edge;
                if (by > ty) {
                    int height_color = (((by - ty) << 8) - 256) | pixel_color;
                    tri_line_win95(z_at_row, height_color, dz_per_row, mptr, fbptr, pitch);
                }
            }
            col++;
            y_top_fp  += dy13;
            running_z += dz13;
            y_bot_fp  += dy12;
            col_count++;
        }

        /* Right half: switch bottom edge to 2→3 */
        y_bot_fp = (((15 - (px2 & 0xF)) * dy23) >> 4)
                 + (py2 << 12) + (screen_centre_y << 16);

        while (col_count < cols_left_right) {
            if (col >= right_edge) break;
            if (col >= left_edge) {
                int ty = y_top_fp >> 16;
                int16_t *mptr = mask_map[mask_idx] + ty * screen_width + col;
                int z_at_row= ((dz_per_row * (15 - (ty & 0xF))) >> 4) + running_z;
                char *fbptr = fb_data + ty * pitch + col;
                int clip = top_edge - ty;
                if (clip > 0) {
                    fbptr += clip * pitch;
                    mptr  += clip * screen_width;
                    z_at_row   += dz_per_row * clip;
                    ty     = top_edge;
                }
                int by = y_bot_fp >> 16;
                if (by > bottom_edge) by = bottom_edge;
                if (by > ty) {
                    int height_color = (((by - ty) << 8) - 256) | pixel_color;
                    tri_line_win95(z_at_row, height_color, dz_per_row, mptr, fbptr, pitch);
                }
            }
            col++;
            y_top_fp  += dy13;
            running_z += dz13;
            y_bot_fp  += dy23;
            col_count++;
        }
    } else {
        /* Case 2: edge 1→2 is above or equal to edge 1→3.
         * Top edge tracks 1→2 then 2→3, bottom edge tracks 1→3. */
        y_top_fp  += y_corr_12;
        running_z += (dz12 * subpix) >> 4;
        y_bot_fp  += y_corr_13;

        while (col_count < cols_left_mid) {
            if (col >= right_edge) break;
            if (col >= left_edge) {
                int ty = y_top_fp >> 16;
                int16_t *mptr = mask_map[mask_idx] + ty * screen_width + col;
                int z_at_row= ((dz_per_row * (15 - (ty & 0xF))) >> 4) + running_z;
                char *fbptr = fb_data + ty * pitch + col;
                int clip = top_edge - ty;
                if (clip > 0) {
                    fbptr += clip * pitch;
                    mptr  += clip * screen_width;
                    z_at_row   += dz_per_row * clip;
                    ty     = top_edge;
                }
                int by = y_bot_fp >> 16;
                if (by > bottom_edge) by = bottom_edge;
                if (by > ty) {
                    int height_color = (((by - ty) << 8) - 256) | pixel_color;
                    tri_line_win95(z_at_row, height_color, dz_per_row, mptr, fbptr, pitch);
                }
            }
            col++;
            y_top_fp  += dy12;
            running_z += dz12;
            y_bot_fp  += dy13;
            col_count++;
        }

        /* Right half: switch top edge to 2→3, reset running Z from point2 */
        y_top_fp = (((15 - (px2 & 0xF)) * dy23) >> 4)
                 + (screen_centre_y << 16) + (py2 << 12);
        int running_z2 = ((dz23 * (15 - (px2 & 0xF))) >> 4) + (pz2 << 16);

        while (col_count < cols_left_right) {
            if (col >= right_edge) break;
            if (col >= left_edge) {
                int ty = y_top_fp >> 16;
                int16_t *mptr = mask_map[mask_idx] + ty * screen_width + col;
                int z_at_row= ((dz_per_row * (15 - (ty & 0xF))) >> 4) + running_z2;
                char *fbptr = fb_data + ty * pitch + col;
                int clip = top_edge - ty;
                if (clip > 0) {
                    fbptr += clip * pitch;
                    mptr  += clip * screen_width;
                    z_at_row   += dz_per_row * clip;
                    ty     = top_edge;
                }
                int by = y_bot_fp >> 16;
                if (by > bottom_edge) by = bottom_edge;
                if (by > ty) {
                    int height_color = (((by - ty) << 8) - 256) | pixel_color;
                    tri_line_win95(z_at_row, height_color, dz_per_row, mptr, fbptr, pitch);
                }
            }
            col++;
            y_top_fp   += dy23;
            running_z2 += dz23;
            y_bot_fp   += dy13;
            col_count++;
        }
    }

    dd_unlock(plane, fb_data);
}

/* ellipse_arctan_432C64 — table-based; 0x4000 = 90 degrees. */
int16_t arctan(int16_t X, int16_t Y) {
    int16_t result;
    int tan_val;

    if (X <= 0) {
        if (Y <= 0) {
            if (Y < 0) {
                tan_val = ((-X) << 8) / (-Y);
                if (tan_val < 0x400) {
                    result = atan_tab0[tan_val >> 2];
                    return (result - 128) << 8;
                }
                if (tan_val < 0x4000) {
                    result = atan_tab1[tan_val >> 6];
                    return (result - 128) << 8;
                }
                if (tan_val < 0x5200)
                    return -16128;
            }
        } else {
            tan_val = ((-X) << 8) / Y;
            if (tan_val < 0x400) {
                result = atan_tab0[tan_val >> 2];
                return -256 * result;
            }
            if (tan_val < 0x4000) {
                result = atan_tab1[tan_val >> 6];
                return -256 * result;
            }
            if (tan_val < 0x5200)
                return -16128;
        }
        return -16384;
    }

    if (Y > 0) {
        tan_val = (X << 8) / Y;
        if (tan_val < 0x400) {
            result = atan_tab0[tan_val >> 2];
            return result << 8;
        }
        if (tan_val < 0x4000) {
            result = atan_tab1[tan_val >> 6];
            return result << 8;
        }
        if (tan_val < 0x5200)
            return 16128;
        return 0x4000;
    }

    if (Y == 0)
        return 0x4000;

    tan_val = (X << 8) / (-Y);
    if (tan_val < 0x400) {
        result = atan_tab0[tan_val >> 2];
        return (-result - 128) << 8;
    }
    if (tan_val < 0x4000) {
        result = atan_tab1[tan_val >> 6];
        return (-result - 128) << 8;
    }
    if (tan_val >= 0x5200)
        return 0x4000;
    return 16640;
}

/* ellipse_arcsin  E1: 0x42BC74 | E2: 0x432D64 */
int16_t arcsin(int16_t value) {
    if (value < 0x4000) {
        if (value > -0x4000)
            return arcsin_tab[0x4000 + value];
        else
            return -0x4000;
    }
    return 0x4000;
}

/* ellipse_calculate_squash_432E08 */
void calculate_squash(part_t *part) {
    int16_t sx = part->VECTOR_Squash.X;
    int16_t sy = part->VECTOR_Squash.Y;
    int16_t sz = part->VECTOR_Squash.Z;
    int16_t max_dim;

    if (sx <= sy && sy > sz)
        max_dim = sy;
    else if (sx > sz)
        max_dim = sx;
    else
        max_dim = sz;

    part->max_squash = max_dim;

    if (max_dim) {
        part->squash_ratio.X = (sx == max_dim) ? 0x4000 : (sx << 14) / max_dim;
        part->squash_ratio.Y = (sy == max_dim) ? 0x4000 : (sy << 14) / max_dim;
        part->squash_ratio.Z = (sz == max_dim) ? 0x4000 : (sz << 14) / max_dim;
    }
}

/* ellipse_find_ellipse_432ECC — projected 2D ellipse (axes, tilt, depth
 * offsets) from the part's rotation matrix. */
void find_ellipse(part_t *part) {
    vector_t tmp, output_vec, input_vec, dst;

    uint16_t angle_xy = -arctan(
        part->squash_ratio.X * part->matrix_2._31 >> 14,
        part->squash_ratio.Y * part->matrix_2._32 >> 14);

    tmp.Z = 0;
    tmp.X = part->squash_ratio.X * cosn_table[angle_xy] >> 14;
    tmp.Y = part->squash_ratio.Y * sine_table[angle_xy] >> 14;
    matrix_vector(&tmp, &output_vec, &part->matrix_2);

    int16_t angle_xz = -arctan(
        (part->squash_ratio.X * part->matrix_2._31) >> 14,
        (part->squash_ratio.Z * part->matrix_2._33) >> 14);
    uint16_t angle_yz = -arctan(
        (part->squash_ratio.Y * part->matrix_2._32) >> 14,
        (part->squash_ratio.Z * part->matrix_2._33) >> 14);

    uint16_t sel_a, sel_b;
    if (((angle_xy + 0x2000) & 0x7FFF) >= 0x4000) {
        sel_b = -angle_xz;
        sel_a = angle_xy + 0x8000;
    } else {
        sel_a = angle_xy + 0x4000;
        sel_b = -angle_yz;
    }

    uint16_t tilt = -arctan(
        sine_table[sel_b],
        sine_table[sel_a] * cosn_table[sel_b] >> 14);
    uint16_t tilt_90 = tilt + 0x4000;

    tmp.X = -((((part->squash_ratio.X * cosn_table[tilt]) >> 14) * sine_table[angle_xy]) >> 14);
    tmp.Y =  (((part->squash_ratio.Y * cosn_table[tilt]) >> 14) * cosn_table[angle_xy]) >> 14;
    tmp.Z =  (part->squash_ratio.Z * sine_table[tilt]) >> 14;
    matrix_vector(&tmp, &input_vec, &part->matrix_2);

    tmp.X = -((((part->squash_ratio.X * cosn_table[tilt_90]) >> 14) * sine_table[angle_xy]) >> 14);
    tmp.Y =  (((part->squash_ratio.Y * cosn_table[tilt_90]) >> 14) * cosn_table[angle_xy]) >> 14;
    tmp.Z =  (part->squash_ratio.Z * sine_table[tilt_90]) >> 14;
    matrix_vector(&tmp, &dst, &part->matrix_2);

    dst.X -= dst.Z * part->persp_origin.X / part->vector_persp.Z;
    dst.Y -= dst.Z * part->persp_origin.Y / part->vector_persp.Z;

    vector_t src, out_copy, in_copy;
    copy_vector(&src, &dst);
    copy_vector(&out_copy, &output_vec);
    copy_vector(&in_copy, &input_vec);

    uint16_t rot_z = -arctan(dst.Y, dst.X);
    rotate_vector_about_z(&dst, rot_z);
    rotate_vector_about_z(&output_vec, rot_z);
    rotate_vector_about_z(&input_vec, rot_z);

    uint16_t blend = arctan(input_vec.Y, output_vec.Y);
    uint16_t blend_90 = blend + 0x4000;

    int16_t blend_x = ((cosn_table[blend_90] * output_vec.X) >> 14) +
                      ((sine_table[blend_90] * input_vec.X) >> 14);
    tmp.X = (blend_x *  cosn_table[rot_z]) >> 14;
    tmp.Y = (blend_x * -sine_table[rot_z]) >> 14;
    tmp.Z = 0;

    int16_t proj_x = ((cosn_table[blend] * out_copy.X) >> 14) + ((sine_table[blend] * in_copy.X) >> 14);
    int16_t proj_y = ((cosn_table[blend] * out_copy.Y) >> 14) + ((sine_table[blend] * in_copy.Y) >> 14);

    uint16_t rot_proj = arctan(dst.X, blend_x);
    int16_t final_x = ((tmp.X * cosn_table[rot_proj]) >> 14) + ((src.X * sine_table[rot_proj]) >> 14);
    int16_t final_y = ((tmp.Y * cosn_table[rot_proj]) >> 14) + ((src.Y * sine_table[rot_proj]) >> 14);
    int16_t depth_x = src.Z * sine_table[rot_proj] >> 14;
    int16_t depth_z = src.Z * cosn_table[rot_proj] >> 14;

    uint16_t final_angle = arctan(final_x, proj_x);
    uint16_t final_angle_90 = final_angle + 0x4000;

    part->projected_axes.Z = (part->max_squash * (((proj_y * cosn_table[final_angle]) >> 14) + ((final_y * sine_table[final_angle]) >> 14))) >> 14;
    part->projected_axes.X = (part->max_squash * (((proj_x * cosn_table[final_angle]) >> 14) + ((final_x * sine_table[final_angle]) >> 14))) >> 14;
    part->projected_axes.Y = (part->max_squash * (((proj_y * cosn_table[final_angle_90]) >> 14) + ((final_y * sine_table[final_angle_90]) >> 14))) >> 14;
    part->depth_offset.X = (part->max_squash * (((depth_x * sine_table[final_angle]) >> 14) + 0)) >> 14;
    part->depth_offset.Y = (part->max_squash * (((depth_x * sine_table[final_angle_90]) >> 14) + 0)) >> 14;
    part->depth_offset.Z = (part->max_squash * depth_z) >> 14;

    if (part->projected_axes.X < 0) {
        part->projected_axes.X = -part->projected_axes.X;
        part->depth_offset.X = -part->depth_offset.X;
    }

    if (part->projected_axes.Y < 0) {
        part->projected_axes.Y = -part->projected_axes.Y;
        part->depth_offset.Y = -part->depth_offset.Y;
    }

    if (part->depth_offset.Z < 0)
        part->depth_offset.Z = -part->depth_offset.Z;
}

/* ellipse_view_trans_long_tri  E2: 0x432B58 */
void view_trans_long_tri(tri_t *tri) {
    long_view_transform((long_vector_t *)&tri->point1->screen_coord,
                        &tri->point1->world_position);
    long_view_transform((long_vector_t *)&tri->point2->screen_coord,
                        &tri->point2->world_position);
    long_view_transform((long_vector_t *)&tri->point3->screen_coord,
                        &tri->point3->world_position);
}

/* ellipse_arctan_slow  E2: 0x432B8C */
int16_t arctan_slow(int16_t x, int16_t y) {
    if (x == 0 && y == 0) return 0;
    double angle = atan2((double)y, (double)x);
    angle += M_PI;
    angle *= 32768.0 / M_PI;
    return (int16_t)(int)angle;
}
