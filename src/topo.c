/**
 * topo.c
 *
 * Terrain: height queries, map element lookup, position update with
 * collision, velocity/gravity, background loading, palette, visibility map.
 */

#include "topo.h"
#include "compat.h"
#include "render.h"
#include "asm_f.h"
#include "display.h"
#include "ellipse.h"
#include "file.h"
#include "game.h"
#include "init.h"
#include "map.h"
#include "move.h"
#include "prefetch.h"
#include "prof.h"
#include "req.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

profile_height_t profile_height = {0};

int16_t topography = 0;
int16_t editor_mode = 0;
int16_t script_mode = 0;
int16_t make_backgrounds = 0;
int16_t moving_camera = 0;
int16_t demo_option = 0;
camera_data_t *old_camera = NULL;
int32_t last_camera_change = 0;
bool no_die = false;
int16_t loaded_background[4] = {-1, -1, -1, -1};
int16_t selected_camera = 0;
int16_t num_cameras = 0;
char palette_control[24];
int32_t palette_offset[1200];
int32_t visib_offset[1200];

/* Byte-swap helper for big-endian → little-endian int16_t values */
int16_t reverse_char_word_val(int16_t val) {
    unsigned short v = (unsigned short)val;
    return (int16_t)((v >> 8) | (v << 8));
}

/* topo_init_profile_heights  E1: 0x43E2F8 | E2: 0x448628 */
void init_profile_heights(void) {
    profile_height.field_0 = 0;
    profile_height.field_2 = -1536;
    profile_height.field_4 = 0;
    profile_height.field_6 = 0;
    profile_height.field_8 = -1536;
    profile_height.field_A = -128;
    profile_height.field_E = 0;
    profile_height.field_C = -256;
}

/* topo_find_map_element_448744 — resolves the triangle/quadrant subdivision. */
int find_map_element(vector_t *position) {
    int result_idx = -1;
    signed int test_height = -1;
    int col = (position->X >> 9) + 64;
    int row = (position->Z >> 9) + 64;

    /* topo_find_map_element_448694+35 indexes new_map with the flat offset and
     * no per-axis check, so a position just off the grid wraps into the next
     * row. Rejecting it returned -1, i.e. camera 0 and play_dead_scene(7).
     * Bound only the flat index. */
    int flat = row * 128 + col;
    if (flat < 0 || flat >= 128 * 128)
        return -1;
    int map_elem_idx = ((const uint16_t *)new_map)[flat];
    int norm_x = (position->X & 0x1FF) - 256;
    int norm_z = (position->Z & 0x1FF) - 256;
    int height_pos = 128 + ((-position->Y) >> height_shift);

    if (map_elem_idx == 0xFFFF)
        return -1;

    height_pos += (game_version == GAME_VERSION_E1) ? 1 : 4;
    map_area_element_t *map_elem = &map_elements[map_elem_idx];
    int code_idx;

    do {
        int in_region = 0;
        code_idx = map_elem->code_index_p1;

        if (map_elem->block_config <= 5) {
            switch (map_elem->block_config) {
                case 1: in_region = 1; break;
                case 2: if (norm_x >= norm_z) in_region = 1; break;
                case 3: if (norm_x <= -norm_z) in_region = 1; break;
                case 4: if (norm_x >  -norm_z) in_region = 1; break;
                case 5: if (norm_x <   norm_z) in_region = 1; break;
                default: break;
            }
        } else {
            if (norm_x <= 0) {
                if (norm_z <= 0) {
                    if (map_elem->block_config & 2) in_region = 1;
                } else {
                    if (map_elem->block_config & 8) in_region = 1;
                }
            } else {
                if (norm_z <= 0) {
                    if (map_elem->block_config & 1) in_region = 1;
                } else {
                    if (map_elem->block_config & 4) in_region = 1;
                }
            }
        }

        if (in_region && map_elem->height2 <= height_pos &&
            map_elem->def_height > test_height) {
            test_height = map_elem->def_height;
            result_idx = map_elem_idx;
        }

        ++map_elem;
        ++map_elem_idx;
    } while (!(code_idx & 0x8000));

    return result_idx;
}

/* topo_find_map_element_vis_4488A4 — find_map_element without the height step. */
signed int find_map_element_vis(vector_t *position) {
    int result_idx = -1;
    signed int test_height = -1;
    int col = (position->X >> 9) + 64;
    int row = (position->Z >> 9) + 64;
    int flat = row * 128 + col;
    if (flat < 0 || flat >= 128 * 128)
        return -1;
    uint16_t map_elem_idx = ((const uint16_t *)new_map)[flat];
    int norm_x = (position->X & 0x1FF) - 256;
    int norm_z = (position->Z & 0x1FF) - 256;
    int height_pos = 128 + ((-position->Y) >> height_shift);

    if (map_elem_idx == 0xFFFF)
        return -1;

    /* E1 uses the same height step here as find_map_element. */
    if (game_version == GAME_VERSION_E1)
        height_pos += 1;

    map_area_element_t *map_elem = &map_elements[map_elem_idx];
    int code_idx;

    do {
        int check = 0;
        code_idx = map_elem->code_index_p1;

        if (map_elem->block_config <= 5) {
            switch (map_elem->block_config) {
                case 1: check = 1; break;
                case 2: if (norm_x >= norm_z) check = 1; break;
                case 3: if (norm_x <= -norm_z) check = 1; break;
                case 4: if (norm_x >  -norm_z) check = 1; break;
                case 5: if (norm_x <   norm_z) check = 1; break;
                default: break;
            }
        } else {
            if (norm_x <= 0) {
                if (norm_z <= 0) { if (map_elem->block_config & 2) check = 1; }
                else                  { if (map_elem->block_config & 8) check = 1; }
            } else {
                if (norm_z <= 0) { if (map_elem->block_config & 1) check = 1; }
                else                  { if (map_elem->block_config & 4) check = 1; }
            }
        }

        if (check && map_elem->height2 <= height_pos &&
            map_elem->def_height > test_height) {
            test_height = map_elem->def_height;
            result_idx = map_elem_idx;
        }

        ++map_elem;
        ++map_elem_idx;
    } while (!(code_idx & 0x8000));

    return result_idx;
}

/* topo_find_height_now  E1: 0x43E644 | E2: 0x44898C */
int16_t find_height_now(vector_t *position, actor_t *actor) {
    return find_height_now_material(position, actor, 0);
}

/* topo_find_height_now_material_448A48 — also applies the CT_BLOCK_* tokens. */
int16_t find_height_now_material(vector_t *position, actor_t *actor, int *material) {
    int result;
    int map_elem_idx = find_map_element(position);
    int block_this = 0;

    if (map_elem_idx < 0) {
        if (material) *material = -1;
        return 0x7FFF;
    }

    if (actor) {
        int16_t code_idx = map_elements[map_elem_idx].code_index_p1 & 0x3FFF;
        if (code_idx > 0) {
            code_t *code = code_tab[code_idx - 1];
            if (code) {
                int token_idx = code->token_store_index;
                if (token_idx) {
                    int16_t *token = &token_store[token_idx];
                    while (*token) {
                        switch (*token) {
                            case CT_BLOCK_ACTOR:
                                ++token;
                                if ((*token & 0xFFF) == actor->name_index)
                                    block_this = 1;
                                break;
                            case CT_BLOCK_WANDERERS:
                                if (actor->flags & 2)
                                    block_this = 1;
                                break;
                            case CT_BLOCK_ALL:
                                if (actor != selected_thing)
                                    block_this = 1;
                                break;
                            case CT_BLOCK_AQUATIC:
                                if (actor != selected_thing && (actor->flags & 0x100))
                                    block_this = 1;
                                break;
                        }
                        ++token;
                    }
                }
            }
        }

        if (block_this) {
            actor->event_timer = 150;
            return (int16_t)-0x8000;
        }
    }

    int map_elem_material = map_elements[map_elem_idx].material;
    if (material)
        *material = map_elem_material;

    if (material_flags[map_elem_material] & 4)
        result = -0x8000;
    else
        result = (128 - (uint8_t)map_elements[map_elem_idx].height) << height_shift;

    return (int16_t)result;
}

/* topo_find_height_now_vis  E1: 0x43E7D0 | E2: 0x448B30 */
int16_t find_height_now_vis(vector_t *position) {
    signed int map_elem_idx = find_map_element_vis(position);
    if (map_elem_idx < 0)
        return 0x7FFF;
    return (int16_t)((0x80 - (uint8_t)map_elements[map_elem_idx].height) << height_shift);
}

/* topo_update_position_448C1C — halves large increments for collision precision. */
void update_position(actor_t *actor, vector_t *increment) {
    if (abs(increment->X) <= 64 && abs(increment->Y) <= 64 && abs(increment->Z) <= 64) {
        do_update_position(actor, increment);
    } else {
        vector_t half_inc;
        half_inc.X = increment->X >> 1;
        half_inc.Y = increment->Y >> 1;
        half_inc.Z = increment->Z >> 1;
        update_position(actor, &half_inc);
        update_position(actor, &half_inc);
    }
}

/* topo_do_update_position_448CB4 — probes five directions around the move,
 * checks other actors' boxes and slides along walls. */
void do_update_position(actor_t *actor, vector_t *increment) {
    int16_t direction;
    int16_t distance;
    int16_t blocked_dirs[50];
    vector_t new_pos, probe_pos;
    int ground_material;
    int16_t closest_diff;
    int num_blocked = 0;

    find_dirn_and_dist(&direction, &actor->move_direction, increment->X, increment->Z);

    if (topography || editor_mode || script_mode) {
        actor->position_vector.X += increment->X;
        actor->position_vector.Y += increment->Y;
        actor->position_vector.Z += increment->Z;
        actor->state_flags &= ~4;
        return;
    }

    int actor_bbox = actor->actor_box_size;
    int start_x = actor->position_vector.X;
    int start_z = actor->position_vector.Z;
    probe_pos.Y = actor->position_vector.Y;
    int fully_blocked = 0;
    int16_t inc_x = increment->X;
    int inc_z = increment->Z;
    int move_angle = 0;

    if (root_thing) {
        for (actor_t *cur_actor = root_thing; cur_actor; cur_actor = cur_actor->next_in_display_list) {
            if (cur_actor == actor) continue;
            if (!(cur_actor->flags & 0x1000)) continue;
            if (cur_actor->actor_behavior == BH_DEAD) continue;
            if (cur_actor->part_heap_link && actor == cur_actor->part_heap_link->parent_actor)
                continue;

            int dx = cur_actor->position_vector.X - actor->position_vector.X;
            int dy = cur_actor->position_vector.Y - actor->position_vector.Y;
            int dz = cur_actor->position_vector.Z - actor->position_vector.Z;
            int combined_box = 3 * (actor->actor_box_size + cur_actor->actor_box_size) >> 2;

            if (abs(dx) < combined_box && abs(dy) < combined_box && abs(dz) < combined_box) {
                find_direction_and_distance(&direction, &distance, dx, dz);
                if (distance < combined_box && num_blocked < 50) {
                    blocked_dirs[num_blocked] = direction;
                    num_blocked++;
                }
            }
        }
    }

    /* Probe terrain in 5 directions around movement. */
    find_direction_and_distance(&direction, &distance, inc_x, inc_z);

#define PROBE_IS_DROP(h, py) (((int)(py) - (int)(h)) > 256)
#define PROBE_IS_WALL(h, py) (((int)(h) - (int)(py)) > 256)

#define PROBE_SET(dir) \
    probe_pos.X = start_x + (actor_bbox * cosn_table[(uint16_t)(dir)] >> 14); \
    probe_pos.Z = start_z + (actor_bbox * sine_table[(uint16_t)(dir)] >> 14)

    int16_t h;

    if (game_version == GAME_VERSION_E1) {
        /* E1 treats every probe alike: any height delta over 256 blocks. */
        int16_t probe_dirs[5];
        probe_dirs[0] = direction;
        probe_dirs[1] = (int16_t)(direction + 0x1000);
        probe_dirs[2] = (int16_t)(direction - 0x1000);
        probe_dirs[3] = (int16_t)(direction + 0x3000);
        probe_dirs[4] = (int16_t)(direction - 0x3000);
        for (int i = 0; i < 5; i++) {
            PROBE_SET(probe_dirs[i]);
            h = find_height_now_material(&probe_pos, actor, 0);
            if (abs((int)probe_pos.Y - (int)h) > 256 && num_blocked < 50)
                blocked_dirs[num_blocked++] = probe_dirs[i];
        }
    } else {
        int forward_wall_detected = 0;

        /* Forward probe — block drops, flag walls */
        PROBE_SET(direction);
        h = find_height_now_material(&probe_pos, actor, 0);
        if (PROBE_IS_WALL(h, probe_pos.Y)) {
            forward_wall_detected = 1;
        }
        if (PROBE_IS_DROP(h, probe_pos.Y) && num_blocked < 50) {
            blocked_dirs[num_blocked++] = direction;
        }

        /* +22.5 degree probe — only block for drops */
        int16_t probe_dir_right = (int16_t)(direction + 4096);
        PROBE_SET(probe_dir_right);
        h = find_height_now_material(&probe_pos, actor, 0);
        if (PROBE_IS_DROP(h, probe_pos.Y) && num_blocked < 50) {
            blocked_dirs[num_blocked++] = probe_dir_right;
        }

        /* -22.5 degree probe — only block for drops */
        int16_t probe_dir_left = (int16_t)(direction - 4096);
        PROBE_SET(probe_dir_left);
        h = find_height_now_material(&probe_pos, actor, 0);
        if (PROBE_IS_DROP(h, probe_pos.Y) && num_blocked < 50) {
            blocked_dirs[num_blocked++] = probe_dir_left;
        }

        /* +67.5 degree probe — block for drops + walls (if no forward wall) */
        int16_t probe_dir_rear_right = (int16_t)(direction + 12288);
        PROBE_SET(probe_dir_rear_right);
        h = find_height_now_material(&probe_pos, actor, 0);
        if (num_blocked < 50) {
            if (PROBE_IS_DROP(h, probe_pos.Y) || (PROBE_IS_WALL(h, probe_pos.Y) && !forward_wall_detected)) {
                blocked_dirs[num_blocked++] = probe_dir_rear_right;
            }
        }

        /* -67.5 degree probe — block for drops + walls (if no forward wall) */
        int16_t probe_dir_rear_left = (int16_t)(direction - 0x3000);
        PROBE_SET(probe_dir_rear_left);
        h = find_height_now_material(&probe_pos, actor, 0);
        if (num_blocked < 50) {
            if (PROBE_IS_DROP(h, probe_pos.Y) || (PROBE_IS_WALL(h, probe_pos.Y) && !forward_wall_detected)) {
                blocked_dirs[num_blocked++] = probe_dir_rear_left;
            }
        }
    }

    int adj_x = inc_x;
    int adj_z = inc_z;
    move_angle = arctan(inc_z, inc_x);

    if (num_blocked > 0) {
        int has_wall = 0;
        int closest_wall = 0;
        closest_diff = 0;

        for (int i = 0; i < num_blocked; i++) {
            int16_t angle_diff = (int16_t)(move_angle - blocked_dirs[i]);
            if (angle_diff <= 0x4000 && angle_diff >= -0x4000) {
                if (has_wall) {
                    if ((angle_diff >= 0 || closest_diff < 0) && (angle_diff < 0 || closest_diff >= 0)) {
                        if (abs(angle_diff) < abs(closest_diff)) {
                            closest_diff = angle_diff;
                            closest_wall = blocked_dirs[i];
                            has_wall = 1;
                        }
                    } else {
                        fully_blocked = 1;
                    }
                } else {
                    closest_wall = blocked_dirs[i];
                    closest_diff = angle_diff;
                    has_wall = 1;
                }
            }
        }

        if (fully_blocked) {
            adj_x = 0;
            adj_z = 0;
            actor->state_flags |= 4;
            goto apply_movement;
        }

        if (has_wall) {
            int16_t deflect_angle = (int16_t)((closest_diff >= 0) ? closest_wall + 0x4000 : closest_wall - 0x4000);
            int16_t d_sin = sine_table[(uint16_t)deflect_angle];
            int16_t d_cos = cosn_table[(uint16_t)deflect_angle];
            int projected = (d_cos * inc_x + inc_z * d_sin) >> 14;
            projected = (projected <= 0) ? -projected : projected;
            adj_x = projected * d_cos >> 14;
            adj_z = d_sin * projected >> 14;

#undef PROBE_IS_DROP
#undef PROBE_IS_WALL
#undef PROBE_SET

            if (!(actor->state_flags & 2)) {
                int16_t move_type = actor->move_type;
                if (move_type == 1 || move_type == 7 || move_type == 180 || move_type == 183) {
                    int16_t turn_diff = (int16_t)(move_angle - deflect_angle);
                    int abs_turn_diff = abs(turn_diff);
                    int16_t max_turn = interval << 9;
                    if (abs_turn_diff >= max_turn) {
                        turn_actor(actor, (turn_diff < 0) ? -max_turn : max_turn);
                    } else {
                        turn_actor(actor, turn_diff);
                    }
                }
            }
        }
    }

apply_movement:
    new_pos.X = adj_x + actor->position_vector.X;
    new_pos.Z = adj_z + actor->position_vector.Z;
    new_pos.Y = actor->position_vector.Y;
    new_pos.Y = find_height_now_material(&new_pos, 0, 0);
    new_pos.Y = find_height_now_material(&new_pos, 0, &ground_material);

    /* Water/lava material check */
    if (ground_material == 7 && !(actor->flags & 0x0100)) {
        int16_t beh = actor->actor_behavior;
        if (beh != BH_GET_HIT && beh != BH_DYING && beh != BH_DEAD) {
            actor->actor_behavior = BH_GET_HIT;
            actor->hit_type = 3;
            actor->actor_hitpoints = -100;
        }
    }

    int height_diff = (int)actor->position_vector.Y - (int)new_pos.Y;

    if (game_version == GAME_VERSION_E1) {
        if (abs(height_diff) > 256)
            return;
        actor->position_vector.X = new_pos.X;
        actor->position_vector.Z = new_pos.Z;
        actor->position_vector.Y = new_pos.Y;
    } else {
        if (height_diff > 256)
            return;
        actor->position_vector.X = new_pos.X;
        actor->position_vector.Z = new_pos.Z;
        if (!actor->actor_velocity.Y && height_diff >= -256)
            actor->position_vector.Y = new_pos.Y;
    }
}

/* topo_update_velocity  E1: ? | E2: 0x449530 */
void update_velocity(actor_t *actor) {
    int grounded = 0;
    int work_interval;

    for (work_interval = interval; work_interval > 7; work_interval -= 7) {
        grounded = do_update_velocity(actor, 7);
        if (grounded) break;
    }
    if (!grounded)
        grounded = do_update_velocity(actor, work_interval);

    if (grounded) {
        if (interval) {
            actor->actor_velocity.X = (actor->position_vector.X - actor->previous_position.X) / interval;
            actor->actor_velocity.Z = (actor->position_vector.Z - actor->previous_position.Z) / interval;
        }
        copy_vector(&actor->previous_position, &actor->position_vector);
    }
}

/* topo_do_update_velocity_449680 — gravity, fall damage and fall-impact actions. */
int do_update_velocity(actor_t *actor, int time_interval) {
    int map_area_height = find_height_now_material(&actor->position_vector, 0, 0);
    int landed = 0;
    int height_above_map = actor->position_vector.Y - map_area_height;

    if (height_above_map < 0 || (height_above_map == 0 && actor->actor_velocity.Y < 0)) {
        vector_t new_position;
        new_position.X = time_interval * actor->actor_velocity.X;
        new_position.Y = 0;
        new_position.Z = time_interval * actor->actor_velocity.Z;

        update_position(actor, &new_position);
        map_area_height = find_height_now_material(&actor->position_vector, 0, 0);

        int fall_delta = time_interval * actor->actor_velocity.Y;
        int16_t new_actor_height;

        if (map_area_height - actor->position_vector.Y <= fall_delta) {
            new_actor_height = map_area_height;
        } else {
            actor->actor_velocity.Y += 4 * time_interval; /* free-fall accel */
            new_position.X = actor->position_vector.X;
            new_position.Z = actor->position_vector.Z;
            new_position.Y = fall_delta + actor->position_vector.Y;
            if (find_height_now_material(&new_position, 0, 0) != map_area_height) {
                actor->actor_velocity.Y = 0;
                goto check_landing;
            }
            new_actor_height = new_position.Y;
        }
        actor->position_vector.Y = new_actor_height;
    }

check_landing:
    if (actor->position_vector.Y - map_area_height >= 0) {
        int fall_action_idx = -1;

        if (actor->actor_velocity.Y > 0) {
            if (actor == selected_thing)
                check_hot_spots();

            if (!actor->actor_scene) {
                rephead_t *repertoire = actor->actor_reperture;
                if (repertoire) {
                    if (actor->actor_velocity.Y >= 100) {
                        if (actor->actor_velocity.Y >= 120) {
                            if (actor != selected_thing || !no_die) {
                                if (actor->actor_velocity.Y >= 150)
                                    actor->actor_hitpoints = -100;
                                else
                                    actor->actor_hitpoints -= (actor->actor_velocity.Y - 100) / 2;
                            }
                            if (actor == selected_thing)
                                draw_life_bar();
                            actor->actor_behavior = BH_GET_HIT;
                            actor->hit_type = 3;
                        } else {
                            fall_action_idx = repertoire->action_slots[26];
                        }
                    } else {
                        fall_action_idx = repertoire->action_slots[25];
                    }
                }
            }
        }

        if (fall_action_idx >= 0) {
            check_action_loaded(fall_action_idx);
            action_t *fall_impact_action = action_tab[fall_action_idx];
            if (fall_impact_action)
                force_action(actor, fall_impact_action, 0);
        }

        actor->actor_velocity.Y = 0;
        actor->position_vector.Y = map_area_height;
        landed = 1;
    }
    return landed;
}

/* topo_flush_backgrounds  E1: 0x43F41C | E2: 0x449A80 */
void flush_backgrounds(void) {
    loaded_background[0] = -1;
    loaded_background[1] = -1;
    loaded_background[2] = -1;
    loaded_background[3] = -1;
}

/* topo_load_raw_449B4C — packed background for the current camera. */
/* load_raw reads from the file, or from a copy the prefetcher already holds. */
typedef struct {
    FILE *f;
    const uint8_t *p;
    size_t left;
} raw_src_t;

static void raw_read(raw_src_t *src, void *dst, size_t n) {
    if (src->f) {
        fread_ignore(dst, 1, n, src->f);
        return;
    }
    size_t k = n < src->left ? n : src->left;
    memcpy(dst, src->p, k);
    src->p += k;
    src->left -= k;
}

/* Widescreen: the original 640x480 view goes in the middle of the wider
 * plane, and the columns either side come from the re-release's wide view of
 * the same camera, scaled back up by 3/2. Its depth is in mask_map units, so
 * actors stay hidden behind the scenery out there too. Where the wide view has
 * nothing (no file, or a narrower one) the sides are black and hide nothing. */
void widen_background(const char *img, const int16_t *mask) {
    static uint8_t side_img[640 * 320];
    static uint16_t side_depth[640 * 320];
    int side_w = file_read_wide_view(selected_camera, side_img, side_depth);

    int w = screen_width;
    int left = (w - 640) / 2;
    for (int y = 0; y < 480; y++) {
        char *row = bitmap[3] + y * w;
        int16_t *mrow = mask_map[2] + y * w;
        memcpy(row + left, img + y * 640, 640);
        memcpy(mrow + left, mask + y * 640, 640 * sizeof(int16_t));

        const uint8_t *srow = side_img + (y * 2 / 3) * side_w;
        const uint16_t *drow = side_depth + (y * 2 / 3) * side_w;
        for (int x = 0; x < w; x++) {
            if (x == left) x += 640;
            if (x >= w) break;
            /* Narrower wide views are centred too. */
            int sx = ((x - w / 2) * 2 + 3 * side_w / 2) / 3;
            if (sx < 0 || sx >= side_w) {
                row[x] = 0;
                mrow[x] = 0x7FFC;
                continue;
            }
            row[x] = (char)srow[sx];
            mrow[x] = drow[sx] > 0x7FFC ? 0x7FFC : (int16_t)drow[sx];
        }
    }
}

int load_raw(void) {
    if (selected_camera < 0) {
        do_info_req("Can't load negative camera");
        return -1;
    }

    int cam_viewed_idx = selected_camera / 8;
    if (cam_viewed_idx < 150)
        cameras_viewed[cam_viewed_idx] |= 1 << (selected_camera & 7);

    char source[64];

    /* The source must match the active mode, or the row stride is wrong and
     * the image tiles. A camera with no HIRES entry fails so the caller can
     * reload in VGA and upscale with copy_vga_to_svga(). */
    FILE *stream = NULL;
    size_t cached_len = 0;
    const uint8_t *cached = NULL;
    PROF_BEGIN(PROF_RAWREAD);
    if (mode_svga) {
        snprintf(source, sizeof(source), "HIRES/%04d.RAW", selected_camera);
        cached = prefetch_get(source, &cached_len);
        if (!cached) stream = fopen_ci(source, "rb");
        if (!stream && !cached) {
            DBG_LOG(2, "[LR] cam=%d no HIRES view, deferring to VGA upscale\n",
                    selected_camera);
            PROF_END(PROF_RAWREAD);
            return -1;
        }
    } else {
        snprintf(source, sizeof(source), "VIEWS/%04d.RAW", selected_camera);
        cached = prefetch_get(source, &cached_len);
        if (!cached) stream = fopen_ci(source, "rb");
    }
    PROF_COUNT_EVENT(cached ? PROF_C_PF_HIT : PROF_C_PF_MISS);
    if (!stream && !cached) {
        PROF_END(PROF_RAWREAD);
        DBG_LOG(1, "[LR] cam=%d FAILED open '%s'\n", selected_camera, source);
        do_info2_req("Can't load view", source);
        return -1;
    }

    raw_src_t src;
    src.f = stream;
    src.p = cached;
    src.left = cached_len;
    int16_t signature = 0;
    raw_read(&src, &signature, 2);

    /* The files are 640 wide; a wide screen unpacks them aside first. */
    static char *wide_img = NULL;
    static int16_t *wide_mask = NULL;
    int widen = mode_svga && screen_width != 640;
    if (widen && !wide_img) {
        wide_img = (char *)malloc(640 * 480);
        wide_mask = (int16_t *)malloc(640 * 480 * sizeof(int16_t));
        if (!wide_img || !wide_mask) quit("not enough memory for the wide view");
    }
    char *img_dst = widen ? wide_img : bitmap[3];
    int16_t *mask_dst = widen ? wide_mask : mask_map[2];
    int raw_w = widen ? 640 : screen_width;

    if (signature == 0x686D) {
        PROF_END(PROF_RAWREAD);
        raw_read(&src, img_dst, 0x1E);
        raw_read(&src, img_dst, 0x300);
        raw_read(&src, img_dst, screen_height * raw_w);
        raw_read(&src, mask_dst, 2 * screen_height * raw_w);
    } else {
        int bg_size = 0, hmap_size = 0;
        raw_read(&src, &bg_size, 4);
        raw_read(&src, &hmap_size, 4);

        if (hmap_size + bg_size >= 2 * screen_height * screen_width)
            quit("packed info too big!");

        /* A whole prefetched file is unpacked in place, without the copy. */
        const char *packed = (const char *)mask_map[0];
        if (!stream && src.left >= (size_t)(hmap_size + bg_size))
            packed = (const char *)src.p;
        else
            raw_read(&src, mask_map[0], hmap_size + bg_size);
        PROF_END(PROF_RAWREAD);
        PROF_BEGIN(PROF_UNPACK);
        unpack_bitmap(img_dst, (char *)packed);
        unpack_mask(mask_dst, (char *)packed + bg_size);
        PROF_END(PROF_UNPACK);
    }

    if (stream) fclose(stream);
    if (widen) widen_background(wide_img, wide_mask);

    PROF_BEGIN(PROF_VISMAP);
    load_visibility_map();
    PROF_END(PROF_VISMAP);
    PROF_BEGIN(PROF_PAL);
    load_palette(NULL);
    PROF_END(PROF_PAL);
    clip_mask(2, 1, 0, 0, screen_width, screen_height);
    clip_mask(2, 0, 0, 0, screen_width, screen_height);

    /* Here rather than in check_view/check_camera: this is the one function
     * that rewrites bitmap[3] and mask_map[2]. */
    render_invalidate_background();

    return 0;
}

/* topo_load_raw_graphic  E1: 0x43FA24 | E2: 0x44A0C8 */
char *load_raw_graphic(const char *source, int *size_x, int *size_y) {
    char destination[52];
    FILE *stream = NULL;

    /* The Win95 data splits title/end art: GRAPHICS holds the 640x480 set and
     * LOWGRAPH the 320x200 one. DOS data has only GRAPHICS, already lo-res, so
     * the LOWGRAPH lookup simply misses and falls through. */
    if (!mode_svga) {
        snprintf(destination, sizeof(destination), "LOWGRAPH/%s", source);
        stream = fopen_ci(destination, "rb");
    }
    if (!stream) {
        snprintf(destination, sizeof(destination), "graphics/%s", source);
        stream = fopen_ci(destination, "rb");
    }
    if (!stream) return NULL;

    bitmap_hdr_t header;
    char palette[768];
    fread_ignore(&header, 1, 0x20, stream);
    fread_ignore(palette, 1, 768, stream);

    int16_t sx = reverse_char_word_val(header.size_x);
    int16_t sy = reverse_char_word_val(header.size_y);
    *size_x = sx;
    *size_y = sy;

    /* Zeroed: a short file must not leave heap garbage on screen. */
    char *result = (char *)calloc(1, (size_t)*size_y * (size_t)*size_x);
    if (!result) quit("Not enough memory for Graphic");

    fread_ignore(result, 1, *size_y * *size_x, stream);
    fclose(stream);

    return result;
}

/* topo_load_palette  E1: ? | E2: 0x44AD28 */
void load_palette(const char *filename) {
    if (!load_by_offset || intro_flag) {
        char destination[64];
        if (intro_flag && game_version == GAME_VERSION_E2)
            snprintf(destination, sizeof(destination), "VIEWS/%04d.PA2", selected_camera);
        else
            snprintf(destination, sizeof(destination), "VIEWS/%04d.PAL", selected_camera);

        FILE *stream = fopen_ci(destination, "rb");
        int loaded = 0;
        if (stream) {
            fread_ignore(&view_cmap, 1, 2, stream);
            fread_ignore(&palette_control, 1, 24, stream);
            fread_ignore(&view_cmap, 1, 768, stream);
            fclose(stream);
            loaded = 1;
        } else if (remaster_layout &&
                   file_read_remaster_palette(selected_camera,
                                              intro_flag && game_version == GAME_VERSION_E2,
                                              (uint8_t *)view_cmap)) {
            memset(palette_control, 0, sizeof(palette_control));
            loaded = 1;
        }

        if (!loaded) {
            memset(palette_control, 0, sizeof(palette_control));
            for (int i = 0; i < 256; ++i) {
                view_cmap[i].R = fade_cmap[i].R = colour_map[i].R;
                view_cmap[i].G = fade_cmap[i].G = colour_map[i].G;
                view_cmap[i].B = fade_cmap[i].B = colour_map[i].B;
            }
        } else {
            if (selected_camera) {
                for (int i = 8; i < 16; ++i) {
                    view_cmap[i].R = colour_map[i].R;
                    view_cmap[i].G = colour_map[i].G;
                    view_cmap[i].B = colour_map[i].B;
                }
            }
            /* fade_in interpolates from fade_cmap toward the camera palette. */
            memcpy(fade_cmap, view_cmap, sizeof(view_cmap));
        }
    } else {
        int32_t read_offset = palette_offset[selected_camera];
        if (read_offset < 0) {
            memset(palette_control, 0, sizeof(palette_control));
            for (int i = 0; i < 256; ++i) {
                view_cmap[i].R = fade_cmap[i].R = colour_map[i].R;
                view_cmap[i].G = fade_cmap[i].G = colour_map[i].G;
                view_cmap[i].B = fade_cmap[i].B = colour_map[i].B;
            }
        } else {
            fseek(file_pointer, read_offset, SEEK_SET);
            /* skip signature */
            fgetc(file_pointer);
            fgetc(file_pointer);

            for (int i = 0; i < 24; ++i)
                palette_control[i] = fgetc(file_pointer);

            for (int i = 0; i < 256; ++i) {
                view_cmap[i].R = fgetc(file_pointer);
                view_cmap[i].G = fgetc(file_pointer);
                view_cmap[i].B = fgetc(file_pointer);
            }

            if (selected_camera) {
                for (int i = 8; i < 16; ++i) {
                    view_cmap[i].R = colour_map[i].R;
                    view_cmap[i].G = colour_map[i].G;
                    view_cmap[i].B = colour_map[i].B;
                }
            }
            memcpy(fade_cmap, view_cmap, sizeof(view_cmap));
        }
    }
}

/* topo_load_visibility_map  E1: 0x440674 | E2: 0x44AFFC */
void load_visibility_map(void) {
    map_area_element_t *work_map_elem;
    uint16_t map_elem_idx;

    for (int i = 0; i < 128; ++i) {
        for (int j = 0; j < 128; ++j) {
            map_elem_idx = new_map[j][i];
            while (1) {
                if (map_elem_idx == 0xFFFF) break;
                work_map_elem = &map_elements[map_elem_idx];
                work_map_elem->code_index_p1 &= 0xBFFF; /* clear visibility bit */
                if (work_map_elem->code_index_p1 & 0x8000) break;
                map_elem_idx++;
            }
        }
    }

    FILE *vis_fp = NULL;
    uint16_t vis_count = 0;

    /* E2 keeps visibility in FILES/ECSTATIC; the re-release ships loose
     * .vis files for both games. */
    if (game_version == GAME_VERSION_E1 || !load_by_offset) {
        char vis_path[64];
        snprintf(vis_path, sizeof(vis_path), "VISIB/%04d.VIS", selected_camera);
        vis_fp = fopen_ci(vis_path, "rb");
        if (!vis_fp) return;

        char signature[4];
        if (fread(signature, 1, 4, vis_fp) != 4 || strncmp(signature, "VisM", 4)) {
            do_info_req("Not a valid visibility map");
            fclose(vis_fp);
            return;
        }

        vis_count = getwLoHi(vis_fp);
        if (vis_count >= 32000) {
            do_info_req("visibility info too big!");
            fclose(vis_fp);
            return;
        }

        unsigned char *data = (unsigned char *)mask_map[0];
        if (fread(data, 1, vis_count * 4, vis_fp) != (size_t)(vis_count * 4)) {
            fclose(vis_fp);
            return;
        }
        fclose(vis_fp);
    } else {
        int vis_data_offset = visib_offset[selected_camera];
        if (vis_data_offset < 0) return;

        fseek(file_pointer, vis_data_offset, SEEK_SET);

        char signature[4];
        for (int i = 0; i < 4; ++i)
            signature[i] = fgetc(file_pointer);

        if (strncmp(signature, "VisM", 4)) {
            do_info_req("Not a valid visibility map");
            return;
        }

        vis_count = getwLoHi(file_pointer);
        if (vis_count >= 32000) {
            do_info_req("visibility info too big!");
            return;
        }

        /* One fread: a locked fgetc per byte cost ~100 ms on a Vita. A short
         * read is padded with 0xFF, which is what fgetc's EOF stored. */
        size_t want = (size_t)vis_count * 4;
        size_t got = fread(mask_map[0], 1, want, file_pointer);
        if (got < want)
            memset((unsigned char *)mask_map[0] + got, 0xFF, want - got);
    }

    unsigned char *data = (unsigned char *)mask_map[0];

    for (int i = 0; i < vis_count; ++i) {
        unsigned char col = *data++;
        unsigned char row = *data++;
        unsigned char start_h = *data++;
        unsigned char end_h = *data++;

        map_elem_idx = new_map[row][col];
        while (1) {
            if (map_elem_idx == 0xFFFF) break;
            work_map_elem = &map_elements[map_elem_idx];

            if (work_map_elem->def_height == start_h && work_map_elem->block_config == end_h)
                work_map_elem->code_index_p1 |= 0x4000; /* set visibility bit */

            if (work_map_elem->code_index_p1 & 0x8000) break;
            map_elem_idx++;
        }
    }
}

/* topo_swap_xy_447F44 — E2: 0x447F44 */
void swap_xy(int32_t *a, int32_t *b) {
    int32_t tmp = *a;
    *a = *b;
    *b = tmp;
}

/* topo_find_def_height_44893C — E2: 0x44893C */
int16_t find_def_height(vector_t *pos) {
    int idx = find_map_element(pos);
    if (idx < 0)
        return 0x7FFF;
    if (map_elements[idx].material == 1)
        return (int16_t)0x8000;
    return (int16_t)((0x80 - map_elements[idx].def_height) << height_shift);
}

