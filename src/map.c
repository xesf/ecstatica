/**
 * map.c
 *
 * Camera and map management: camera switching, view loading, visibility,
 * actor swap-in/out, display list management.
 */

#include "map.h"
#include "display.h"
#include "edit.h"
#include "game.h"
#include "icon.h"
#include "init.h"
#include "menu.h"
#include "move.h"
#include "render.h"
#include "prefetch.h"
#include "req.h"
#include "topo.h"
#include <string.h>
#include <stdio.h>
#include "prof.h"

uint16_t new_map[128][128];
map_area_element_t map_elements[60000];
int16_t map_element_code2[60000];
#ifdef ECS_ENABLE_GL
int16_t map_element_tex[60000][7];
#endif
int32_t top_of_map_elements = 0;

int16_t need_draw_graphics;
static int16_t camera_override = -1;
static int16_t local_num_cameras;
int16_t suppress_events;

static const char palette_red[16]   = { 0x00, 0x20, 0x30, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x30, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const char palette_green[16] = { 0x3F, 0x3F, 0x3F, 0x3F, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x30, 0x3F, 0x3F };
static const char palette_blue[16]  = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x30, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x20 };

/* map_switch_camera  E1: 0x441954 | E2: 0x44C60C */
void switch_camera(camera_data_t *camera) {
    copy_vector(&view_pos, &camera->view_pos);
    copy_vector(&view_rot, &camera->view_rot);
    zoom_factor = camera->zoom_factor << 12;
    active_camera = camera;
    calculate_view_matrices();
}

/* map_check_view_44BE20 */
static void check_view_body(int camera_idx);

void check_view(int camera_idx) {
    PROF_BEGIN(PROF_VIEW);
    check_view_body(camera_idx);
    PROF_END(PROF_VIEW);
}

static void check_view_body(int camera_idx) {
    camera_data_t *cam = &camera[camera_idx];
    if (cam == active_camera) return;
    if (camera_idx < 0) goto update_actors;

    selected_camera = camera_idx;
    stop_the_clock = 1;
    switch_camera(cam);

    if (!camera_idx) {
        draw_mode[3] = 1;
        a_pen_colour = 0;
        rect_fill(3, 0, 0, screen_width, screen_height);
        int16_t *mask_map_ptr = mask_map[2];
        for (int i = 0; i < screen_height * screen_width; ++i)
            *mask_map_ptr++ = 0x7FFF;
    } else {
        PROF_BEGIN(PROF_RAW);
        int raw_failed = load_raw();
        PROF_END(PROF_RAW);
        if (raw_failed) {
            if (mode_svga) {
                mode_svga = 0;
                set_vga_constants();
                int vga_ok = load_raw();
                mode_svga = 1;
                set_svga_constants();
                if (!vga_ok)
                    copy_vga_to_svga();
                else if (!moving_camera)
                    quit("Can't load view");
            } else {
                quit("Can't load view");
            }
        }
    }

    clip_mask(2, 1, 0, 0, screen_width, screen_height);
    background_status = 2;
    clip_blit(3, 0, 0, 2, 0, 0, screen_width, screen_height, 192);
    clip_mask(2, 1, 0, 0, screen_width, screen_height);
    clip_mask(2, 0, 0, 0, screen_width, screen_height);

    /* The only place bitmap[3] and mask_map[2] change for a new view. */
    render_invalidate_background();

    for (int i = 0; i < THING_TAB_SIZE; ++i) {
        if (thing_name_flags[i] & 2)
            check_thing_name_invis(i);
    }
    for (int i = 0; i < THING_TAB_SIZE; ++i) {
        if (thing_name_flags[i] & 2)
            check_thing_name_vis(i);
    }

update_actors:
    for (actor_t *actor = root_thing; actor; actor = actor->next_in_display_list)
        actor->flags = (actor->flags & 0xF3FF) | 0x400;

    for (int i = 0; i < 20; ++i) {
        if (subtitle_status[i] == 2)
            subtitle_status[i] = 1;
    }

    for (int i = 0; i < 25; ++i) {
        if (graphic_flag[i] == 2) {
            if (graphic_data[i]) {
                need_draw_graphics = 1;
                graphic_flag[i] = 1;
            }
        }
    }
}

/* map_check_hot_spots  E1: 0x4414AC | E2: 0x44C0B4 */
void check_hot_spots(void) {
    if (!selected_thing || selected_thing->actor_behavior == BH_DEAD)
        return;

    int map_elem_idx = find_map_element(&selected_thing->position_vector);
    int code_idx = 0;

    if (map_elem_idx >= 0 &&
        ((-(selected_thing->position_vector.Y) >> height_shift) + 0x80 -
         map_elements[map_elem_idx].def_height) < 8) {
        code_idx = map_elements[map_elem_idx].code_index_p1 & 0x3FFF;
        hero_material = map_elements[map_elem_idx].material;
    }

    if (code_idx > 0 && code_tab[code_idx - 1]) {
        execute_thing_code(selected_thing, code_idx - 1);
    }
}

/* map_check_camera  E1: 0x441560 | E2: 0x44C174 */
void check_camera(void) {
    if (!selected_thing) return;

    prefetch_views_near(&selected_thing->position_vector);

    int16_t camera_idx;
    int map_elem_idx = find_map_element(&selected_thing->position_vector);

    if (map_elem_idx < 0) {
        camera_idx = 0;
    } else {
        if ((-(selected_thing->position_vector.Y) >> height_shift) + 128 -
            map_elements[map_elem_idx].def_height >= 8) {
            camera_idx = (camera_override < 0)
                ? map_elements[map_elem_idx].camera_index
                : camera_override;
        } else {
            camera_idx = map_elements[map_elem_idx].camera_index;
            camera_override = map_elements[map_elem_idx].camera_override;
        }
    }

    camera_data_t *cam = &camera[camera_idx];
    if (cam == active_camera) return;

    if (camera_idx <= 0) {
        DBG_LOG(1, "[CAM] camera_idx=%d from map_elem=%d pos=(%d,%d,%d) "
                   "override=%d flags0=%04x -> dead scene 7\n",
                camera_idx, map_elem_idx,
                selected_thing->position_vector.X,
                selected_thing->position_vector.Y,
                selected_thing->position_vector.Z,
                camera_override, thing_name_flags[0]);
        if (!(thing_name_flags[0] & 4))
            play_dead_scene(7);
    } else {
        selected_camera = camera_idx;
        stop_the_clock = 1;
        switch_camera(cam);

        PROF_BEGIN(PROF_RAW);
        int raw_failed = load_raw();
        PROF_END(PROF_RAW);
        if (raw_failed) {
            if (mode_svga) {
                mode_svga = 0;
                set_vga_constants();
                int vga_ok = load_raw();
                mode_svga = 1;
                set_svga_constants();
                if (!vga_ok)
                    copy_vga_to_svga();
                else if (!moving_camera)
                    quit("Can't load view");
            } else {
                quit("Can't load view");
            }
        }

        clip_blit(3, 0, 0, 2, 0, 0, screen_width, screen_height, 192);
        clip_mask(2, 1, 0, 0, screen_width, screen_height);
        clip_mask(2, 0, 0, 0, screen_width, screen_height);
        background_status = 2;

        PROF_BEGIN(PROF_SWAP);
        for (int i = 0; i < THING_TAB_SIZE; ++i)
            if (thing_name_flags[i] & 2)
                check_thing_name_invis(i);

        for (int i = 0; i < THING_TAB_SIZE; ++i)
            if (thing_name_flags[i] & 2)
                check_thing_name_vis(i);
        PROF_END(PROF_SWAP);
    }

    for (actor_t *actor = root_thing; actor; actor = actor->next_in_display_list)
        actor->flags = (actor->flags & 0xF3FF) | 0x400;

    for (int i = 0; i < 20; ++i)
        if (subtitle_status[i] == 2)
            subtitle_status[i] = 1;

    for (int i = 0; i < 25; ++i) {
        if (graphic_flag[i] == 2 && graphic_data[i]) {
            need_draw_graphics = 1;
            graphic_flag[i] = 1;
        }
    }
}

/* map_init_map  E1: 0x4419A8 | E2: 0x44C66C */
void init_map(void) {
    for (int i = 0; i < 1200; ++i) {
        camera[i].unused_12 = -1;
        camera[i].unused_14 = -1;
    }

    for (int i = 0; i < 128; ++i)
        for (int j = 0; j < 128; ++j)
            new_map[i][j] = -1;

    char red_coeff[16], green_coeff[16], blue_coeff[16];
    memcpy(red_coeff, palette_red, 16);
    memcpy(green_coeff, palette_green, 16);
    memcpy(blue_coeff, palette_blue, 16);

    memset(edit_map_cmap, 0, sizeof(edit_map_cmap));
    init_colours0to8(edit_map_cmap);

    for (int i = 8; i < 16; ++i) {
        edit_map_cmap[i].R = colour_map[i].R;
        edit_map_cmap[i].G = colour_map[i].G;
        edit_map_cmap[i].B = colour_map[i].B;
    }

    for (int i = 128, j = 0; i < 256; ++i, ++j) {
        int sub_idx = i % 16;
        int palette_idx = 2 * (j / 16);
        int brightness, saturation;

        if (sub_idx >= 8) {
            saturation = 256;
            brightness = 24 * (15 - sub_idx) + 64;
        } else {
            saturation = 24 * sub_idx + 64;
            brightness = 256;
        }

        edit_map_cmap[i].R = (63 - (brightness * (63 - (saturation * green_coeff[palette_idx] >> 8)) >> 8)) & 0x3F;
        edit_map_cmap[i].G = (63 - (brightness * (63 - (saturation * red_coeff[palette_idx] >> 8)) >> 8)) & 0x3F;
        edit_map_cmap[i].B = (63 - (brightness * (63 - (saturation * blue_coeff[palette_idx] >> 8)) >> 8)) & 0x3F;
    }

    for (int i = 0; i < 60000; ++i)
        memset(&map_elements[i], 0, sizeof(map_area_element_t));

    camera[0].view_pos.X = 0;
    camera[0].view_pos.Y = -0x190;
    camera[0].view_pos.Z = 0x800;
    camera[0].view_rot.X = 0;
    camera[0].view_rot.Y = (int16_t)0x8000;
    camera[0].view_rot.Z = 0;
    camera[0].zoom_factor = 0x400;
    selected_camera = 0;
    local_num_cameras = 1;
}

/* map_position_is_visible  E1: 0x4424B0 | E2: 0x44D1DC */
int position_is_visible(vector_t *position) {
    signed int map_elem_idx = find_map_element_vis(position);
    if (map_elem_idx < 0)
        return 0;
    return (map_elements[map_elem_idx].code_index_p1 & 0x4000) ? 1 : 0;
}

/* map_check_visibility  E1: 0x4424CC | E2: 0x44D204 */
void check_visibility(actor_t *actor) {
    action_t *action = actor->actor_act.act_action;

    if (action && (action->action_flags & 2)) {
        actor->flags |= 8;
    } else {
        vector_t *position = &actor->position_vector;
        if (actor->flags & 8) {
            if (actor != selected_thing && !position_is_visible(position) &&
                !actor->actor_velocity.Y) {
                actor->flags &= 0xFFF7;
            }
        } else {
            if (actor == selected_thing || position_is_visible(position)) {
                actor->flags |= 8;
            }
        }
    }
}

/* map_check_thing_name_invis  E1: 0x442564 | E2: 0x44D29C */
void check_thing_name_invis(int actor_idx) {
    actor_t *actor = thing_tab[actor_idx];
    if (!actor) return;
    if (actor->part_heap_link || actor == selected_thing) return;

    action_t *action = actor->actor_act.act_action;
    if (action && (action->action_flags & 2)) return;

    signed int map_elem_idx = find_map_element_vis(&actor->position_vector);
    int map_elem = (map_elem_idx < 0)
        ? 0
        : map_elements[map_elem_idx].code_index_p1 & 0x4000;

    if (!map_elem) {
        actor->flags &= 0xFFF7;
        if (actor->flags & 0x02) {
            if (actor->actor_behavior == BH_DYING)
                swap_out_actor(actor);
        } else {
            if (actor->actor_behavior != BH_DEAD)
                swap_out_actor(actor);
        }
    }
}

/* map_swap_out_actor  E1: 0x4425FC | E2: 0x44D338 */
void swap_out_actor(actor_t *actor) {
    int actor_idx = actor->name_index;

    copy_vector(&actor_position[actor_idx], &actor->position_vector);
    copy_vector(&actor_orientation[actor_idx], &actor->rotate_vector);
    actor_rep_name[actor_idx] = actor->actor_rep_index;
    actor_hit_points[actor_idx] = actor->actor_hitpoints;
    actor_magic[actor_idx] = (int8_t)actor->actor_magic;

    part_t *part = actor->part_heap_link;
    int16_t parent_actor_idx;

    if (part) {
        actor_held_by_part[actor_idx] = part->name_index;
        parent_actor_idx = actor->part_heap_link->parent_actor->name_index;
    } else {
        parent_actor_idx = -1;
        actor_held_by_part[actor_idx] = -1;
    }

    actor_held_by_actor[actor_idx] = parent_actor_idx;
    actor->flags &= 0xFFF7;
    remove_from_display_list(actor);

    for (part_t *p = actor->actor_parts_list; p; p = p->next_in_display_list) {
        if (p->actor_2_held)
            swap_out_actor(p->actor_2_held);
    }
}

/* map_check_thing_name_vis  E1: 0x4426BC | E2: 0x44D404 */
void check_thing_name_vis(int actor_idx) {
    actor_t *actor = thing_tab[actor_idx];

    if (actor && (actor_last_act[actor_idx] & 0xC000) != 0xC000) {
        if (!actor->part_heap_link) {
            action_t *action = actor->actor_act.act_action;

            if (actor == selected_thing || (action && (action->action_flags & 2))) {
                actor->flags |= 8;
                add_to_display_list(actor);
                for (part_t *part = actor->actor_parts_list; part; part = part->next_in_display_list)
                    if (part->actor_2_held)
                        add_to_display_list_held(part->actor_2_held);
            } else {
                signed int map_elem_idx = find_map_element_vis(&actor->position_vector);
                int map_elem = (map_elem_idx < 0)
                    ? 0
                    : map_elements[map_elem_idx].code_index_p1 & 0x4000;

                if (map_elem) {
                    actor->flags |= 8;
                    add_to_display_list(actor);
                    for (part_t *part = actor->actor_parts_list; part; part = part->next_in_display_list)
                        if (part->actor_2_held)
                            add_to_display_list_held(part->actor_2_held);
                }
            }
        }
    } else {
        signed int map_elem_idx = find_map_element_vis(&actor_position[actor_idx]);
        int map_elem = (map_elem_idx < 0)
            ? 0
            : map_elements[map_elem_idx].code_index_p1 & 0x4000;

        if (map_elem && actor_held_by_actor[actor_idx] < 0)
            swap_in_actor(actor_idx);
    }
}

/* map_add_to_display_list_held  E1: 0x4427D8 | E2: 0x44D558 */
void add_to_display_list_held(actor_t *actor) {
    add_to_display_list(actor);
    for (part_t *part = actor->actor_parts_list; part; part = part->next_in_display_list)
        if (part->actor_2_held)
            add_to_display_list_held(part->actor_2_held);
}

/* map_swap_in_actor  E1: 0x442804 | E2: 0x44D584 */
void swap_in_actor(int actor_idx) {
    for (int i = 0; i < 5000; i++) {
        if (thing_name_flags[i] & 2) {
            int actor_held_idx = actor_held_by_actor[actor_idx];
            if (actor_held_idx == i)
                swap_in_actor(actor_held_idx);
        }
    }

    actor_t *saved_selected = selected_thing;
    check_actor_loaded(thing_names[actor_idx].field_0);
    actor_t *actor = thing_tab[actor_idx];

    if (!actor) {
        selected_thing = saved_selected;
        return;
    }

    initialise_actor(actor);
    copy_vector(&actor->position_vector, &actor_position[actor_idx]);

    if ((actor_last_act[actor_idx] & 0xC000) == 0xC000) {
        if (actor_rep_name[actor_idx] != -2)
            actor->actor_rep_index = actor_rep_name[actor_idx];
    } else {
        copy_vector(&actor->rotate_vector, &actor_orientation[actor_idx]);
        actor->actor_hitpoints = actor_hit_points[actor_idx];
        actor->actor_magic = actor_magic[actor_idx];
        if (actor_rep_name[actor_idx] != -2)
            actor->actor_rep_index = actor_rep_name[actor_idx];
    }

    if (actor_held_by_actor[actor_idx] >= 0) {
        actor_t *held_actor = thing_tab[actor_held_by_actor[actor_idx]];
        if (held_actor) {
            part_t *part = held_actor->_PartTab->field_0[actor_held_by_part[actor_idx]];
            if (part) {
                actor->part_heap_link = part;
                part->actor_2_held = actor;
            } else {
                do_info2_req("Can't find Holding Part", "in Swap In Actor");
            }
        }
    }

    if (thing_name_flags[actor_idx] & 4)
        actor->actor_behavior = 11;

    actor->flags |= 8;
    add_to_display_list(actor);
    actor->full_actor_hp = 100;
    int16_t code_idx = actor->actor_init_code;
    actor->actor_hitpoints = actor->full_actor_hp;

    if (code_idx >= 0) {
        code_t *code = code_tab[code_idx];
        if (code)
            execute_thing_code(actor, code_idx);
    }

    int action_index = actor_last_act[actor_idx];
    if (action_index) {
        if (action_index & 0x8000) {
            int scene_idx = (action_index & 0x3FFF) - 1;
            check_scene_loaded(scene_idx);
            scene_t *scene = scene_tab[scene_idx];
            if (!scene)
                quit("Missing scene in 'swap in actor'");

            update_thing(actor);

            script_t *script;
            for (script = scene->scene_script_list; script; script = script->next_script) {
                if (script->script_actor_index == actor_idx)
                    break;
            }

            if (!script) {
                do_info3_req("Actor not in scene in 'swap in actor'",
                             thing_names[actor_idx].field_0, scene_names[scene_idx].field_0);
                quit("");
            }

            key_state_t *key = script->script_action.key_list;
            if (key && !key->KEY_position) {
                int saved_rep_idx = actor->actor_rep_index;
                copy_defaults_to_actual(actor);
                actor->actor_rep_index = saved_rep_idx;
                actor->anchored_part = actor->actor_parts_list;
            }

            actor->actor_act.act_action = &script->script_action;
            initialise_act(&actor->actor_act);
            if (!(action_index & 0x4000))
                suppress_events = 1;
            complete_act(&actor->actor_act, actor);
            actor->actor_scene = 0;
            actor->actor_act.act_action = 0;
            suppress_events = 0;

            if (!(scene->scene_use_flag & 1)) {
                scene->scene_time = game_time - 1;
                selected_thing = saved_selected;
                return;
            }
        } else {
            check_action_loaded(action_index - 1);
            action_t *action = action_tab[action_index - 1];
            if (!action)
                quit("Missing action in 'swap in actor'");

            actor->actor_act.act_action = action;
            initialise_act(&actor->actor_act);
            suppress_events = 1;
            complete_act(&actor->actor_act, actor);
            suppress_events = 0;
            actor->actor_act.act_action = 0;

            for (actor_t *find_actor = thing_list; ; find_actor = find_actor->next_thing1) {
                if (!find_actor) {
                    action->action_time = game_time - 1;
                    break;
                }
                if (action == find_actor->actor_act.act_action)
                    break;
            }
        }
    }

    selected_thing = saved_selected;
}

/* map_make_invisible  E2: 0x44D8F8 */
void make_invisible(actor_t *actor) {
    actor->flags &= ~0x08;
}

/* map_copy_vga_to_svga  E2: 0x44C434 — 2x horizontally, 2.4x vertically
 * (rows repeated 3,2,2,3,2 per 5). */
void copy_vga_to_svga(void) {
    static char svga_buf[640 * 480];
    /* A wide screen builds the 640-wide view aside and widens it after. */
    static int16_t wide_mask[640 * 480];
    int widen = screen_width != 640;
    int16_t *mask_dst = widen ? wide_mask : mask_map[0];

    int dst_y = 0;
    int src_pix_off = 0;
    int src_mask_off = 0;

    for (int y = 0; y < 200; y++) {
        int rem = y % 5;
        int triple = (rem == 0 || rem == 3);

        int dst_row0_bmp = dst_y * 640;
        int dst_row1_bmp = (dst_y + 1) * 640;
        int dst_row2_bmp = (dst_y + 2) * 640;
        int dst_row0_mask = dst_y * 640;
        int dst_row1_mask = (dst_y + 1) * 640;
        int dst_row2_mask = (dst_y + 2) * 640;

        for (int x = 0; x < 320; x++) {
            unsigned char pixel = (unsigned char)bitmap[3][src_pix_off + x];
            int dx = x * 2;

            svga_buf[dst_row0_bmp + dx]     = pixel;
            svga_buf[dst_row0_bmp + dx + 1] = pixel;
            svga_buf[dst_row1_bmp + dx]     = pixel;
            svga_buf[dst_row1_bmp + dx + 1] = pixel;
            if (triple) {
                svga_buf[dst_row2_bmp + dx]     = pixel;
                svga_buf[dst_row2_bmp + dx + 1] = pixel;
            }

            int16_t mask_val = mask_map[2][src_mask_off + x];
            int mx = x * 2;
            mask_dst[dst_row0_mask + mx]     = mask_val;
            mask_dst[dst_row0_mask + mx + 1] = mask_val;
            mask_dst[dst_row1_mask + mx]     = mask_val;
            mask_dst[dst_row1_mask + mx + 1] = mask_val;
            if (triple) {
                mask_dst[dst_row2_mask + mx]     = mask_val;
                mask_dst[dst_row2_mask + mx + 1] = mask_val;
            }
        }

        dst_y += triple ? 3 : 2;
        src_pix_off += 320;
        src_mask_off += 320;
    }

    if (widen) {
        widen_background(svga_buf, wide_mask);
        clip_mask(2, 0, 0, 0, screen_width, screen_height);
        clip_mask(2, 1, 0, 0, screen_width, screen_height);
        return;
    }
    memcpy(bitmap[3], svga_buf, 640 * 480);
    clip_mask(0, 1, 0, 0, screen_width, screen_height);
    clip_mask(0, 2, 0, 0, screen_width, screen_height);
}

/* map_copy_background2to01  E2: 0x44D9D8 */
void copy_background2to01(void) {
    clip_blit(2, 0, 0, 0, 0, 0, screen_width, screen_height, 0xC0);
    clip_blit(2, 0, 0, 1, 0, 0, screen_width, screen_height, 0xC0);
}

/* map_find_highest_camera_num_44D0AC — E2: 0x44D0AC */
int find_highest_camera_num(void) {
    uint8_t used[1200] = {0};

    for (int row = 0; row < 128; row++) {
        for (int col = 0; col < 128; col++) {
            uint16_t elem_idx = new_map[row][col];
            if (elem_idx == 0xFFFF)
                continue;
            while (1) {
                map_area_element_t *elem = &map_elements[elem_idx];
                if (elem->camera_index != 0)
                    used[elem->camera_index] = 1;
                if (elem->camera_override != 0)
                    used[elem->camera_override] = 1;
                if (elem->code_index_p1 < 0)
                    break;
                elem_idx++;
            }
        }
    }

    int highest = 0;
    for (int i = 1199; i >= 0; i--) {
        if (used[i]) {
            highest = i + 1;
            break;
        }
    }
    return highest;
}

/* map_copy_background3to012  E2: 0x44DA34 */
void copy_background3to012(void) {
    clip_blit(3, 0, 0, 0, 0, 0, screen_width, screen_height, 0xC0);
    clip_blit(3, 0, 0, 1, 0, 0, screen_width, screen_height, 0xC0);
    clip_blit(3, 0, 0, 2, 0, 0, screen_width, screen_height, 0xC0);
}
