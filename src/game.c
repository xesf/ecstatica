/**
 * game.c
 *
 * Script execution engine, actor spawning/removal, game state,
 * fade effects, collision, camera, combat, wanderer system.
 */

#include "game.h"
#include "prof.h"
#include "asm_f.h"
#include "display.h"
#include "edit.h"
#include "ellipse.h"
#include "file.h"
#include "icon.h"
#include "init.h"
#include "map.h"
#include "menu.h"
#include "move.h"
#include "music.h"
#include "req.h"
#include "topo.h"
#include "win.h"
#include "platform.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "compat.h"
#ifndef _WIN32
#endif

/* Part context for CT_SPAWN_LIVE dispatch — set by execute_part_code. */
static part_t *g_execute_part = NULL;

/* E2 scripts name the code's subject and object as the pseudo-things SUB (2)
 * and OBJ (3); DoExecuteCode (0x44F3A0) receives both as arguments. E1 has no
 * such names and reads the index as-is. */
#define THING_SUB 2
#define THING_OBJ 3
static actor_t *g_code_sub = NULL;
static actor_t *g_code_obj = NULL;

bool game_up_and_running = false;
bool program_up_and_running = false;
int32_t game_timer = 0;
int32_t game_timer_start = 0;
int32_t game_time = 0;
int32_t interval = 0;
int32_t x_time = 0;
int32_t break_do_movement = 0;
int32_t num_info_lines = 0;
int32_t key_esc_was_forced = 0;
bool intro_flag = false;
bool female = false;
int16_t difficulty = 1;
int16_t language = 0;
bool stop_the_clock = false;
/* InitScene skips scenes already started: set while replay_skipped_startups runs. */
static bool startup_repair = false;
bool no_wanderers = false;
int16_t no_icons = 0;
bool slow_motion = false;

int16_t kill_count = 0;
int16_t treasure_count = 0;
int16_t map_count = 0;
int16_t armour_factor = 100;
int32_t poison_time = 0;
int16_t hero_material = 0;
int32_t mode_svga = 0;
int32_t chosen_svga = 1;
int32_t low_res_only = 0;
int32_t hires_available = 0;
int32_t e1_dos_data = 0;
int32_t select_flag = 0;
int32_t eagle_card = 0;
int16_t height_shift = 7;

bool fade_to_black = false;
bool fade_to_white = false;
bool fade_in = false;
int32_t fade_start = 0;
int32_t fade_time = 0;
int32_t check_time = 0;
int16_t last_fade_factor = 0;
int32_t lightning = 0;

actor_t *root_thing = NULL;
actor_t *thing_list = NULL;
actor_t *stuck_thing_list = NULL;
actor_t *selected_thing = NULL;
scene_t *selected_scene = NULL;
int16_t last_actor_dir = -1;
action_t *action_list = NULL;
scene_t *root_scene = NULL;
scene_t *scene_list = NULL;
code_t *code_list = NULL;
rephead_t *repertoire_list = NULL;
sound_t *sound_list = NULL;
map_area_t *map_area_list = NULL;
texture_t *texture_list = NULL;
point_t *point_list = NULL;
tri_t *triangle_list = NULL;
script_t *script_list = NULL;

actor_t *thing_tab[THING_TAB_SIZE] = {0};
action_t *action_tab[ACTION_TAB_SIZE] = {0};
scene_t *scene_tab[SCENE_TAB_SIZE] = {0};
code_t *code_tab[CODE_TAB_SIZE] = {0};
rephead_t *repertoire_tab[REPERTOIRE_TAB_SIZE] = {0};
sound_t *sound_tab[SOUND_TAB_SIZE] = {0};
map_area_t *map_area_tab[MAP_AREA_TAB_SIZE] = {0};
texture_t *texture_tab[TEXTURE_TAB_SIZE] = {0};

actor_t actor_heap_arr[ACTOR_POOL_SIZE];
part_tab_t part_tab_heap_arr[ACTOR_POOL_SIZE];
triangle_tab_t triangle_tab_heap_arr[ACTOR_POOL_SIZE];
point_tab_t point_tab_heap_arr[ACTOR_POOL_SIZE];
part_t part_heap_arr[PART_POOL_SIZE];
action_t action_heap_arr[ACTION_POOL_SIZE];
scene_t scene_heap_arr[SCENE_POOL_SIZE];
event_t event_heap_arr[EVENT_POOL_SIZE];
key_state_t key_heap_arr[KEY_POOL_SIZE];
tri_t tri_arr[TRI_SIZE];
point_t point_heap_arr[POINT_POOL_SIZE];
sound_t sound_heap_arr[SOUND_POOL_SIZE];
script_t script_arr[SCRIPT_SIZE];
rephead_t rep_heap_arr[REP_POOL_SIZE];
texture_t texture_heap_arr[TEXTURE_POOL_SIZE];
taction_t taction_heap_arr[TACTION_POOL_SIZE];
act_t act_arr[ACT_SIZE];

name_text_t *thing_names = NULL;
name_text_t *action_names = NULL;
name_text_t *scene_names = NULL;
name_text_t *code_names = NULL;
name_text_t *repertoire_names = NULL;
name_text_t *sound_names = NULL;
name_text_t *part_names = NULL;
name_text_t *point_names = NULL;
name_text_t *triangle_names = NULL;
name_text_t *map_area_names = NULL;
name_text_t *texture_names = NULL;

int16_t thing_name_flags[THING_TAB_SIZE];
int16_t scene_name_flags[SCENE_TAB_SIZE];

int16_t actor_rep_name[THING_TAB_SIZE];
int16_t actor_magic[THING_TAB_SIZE];
int16_t actor_hit_points[THING_TAB_SIZE];
vector_t actor_position[THING_TAB_SIZE];
vector_t actor_orientation[THING_TAB_SIZE];
int16_t actor_last_act[THING_TAB_SIZE];
int16_t actor_held_by_part[THING_TAB_SIZE];
int16_t actor_held_by_actor[THING_TAB_SIZE];
int16_t actor_flags[THING_TAB_SIZE];

int16_t *token_store = NULL;
int32_t top_of_tokens = 0;

bool is_god_mode = false;

int16_t saved_game_num = 0;
int32_t extra_life_time = 0;

UNUSED_ATTR static int find_empty_actor_slot(void);

static void execute_with_sub_obj(code_t *code, actor_t *actor, actor_t *sub, actor_t *obj) {
    actor_t *prev_sub = g_code_sub, *prev_obj = g_code_obj;
    g_code_sub = sub;
    g_code_obj = obj;
    do_execute_code(code, actor);
    g_code_sub = prev_sub;
    g_code_obj = prev_obj;
}

/* game_execute_code_425678 — main script interpreter */
void execute_code(code_t *code, actor_t *actor) {
    if (!code) return;
    execute_with_sub_obj(code, actor, NULL, NULL);
}

/* game_execute_sub_obj_code  E1: 0x443D4C | E2: 0x44F1C4 — pick-up and hit
 * codes, which name the actor acting (SUB) and the one acted on (OBJ). */
void execute_sub_obj_code(code_t *code, actor_t *actor, actor_t *sub, actor_t *obj) {
    if (!code) return;
    execute_with_sub_obj(code, actor, sub, obj);
}

/* Unpack a signed 12-bit value from a token word */
static int unpack_token(int16_t token) {
    if (token & 0x0800)
        return (int)(token | (int16_t)0xF000u);
    else
        return (int)(token & 0x0FFF);
}

/* Get unsigned 12-bit value from a token (name/index reference) */
static int16_t get_value_from_token(int16_t token) {
    return token & 0x0FFF;
}

/* An absent SUB/OBJ resolves to THING_TAB_SIZE, which every caller already
 * rejects (the original uses 5000 the same way). */
static int16_t resolve_sub_obj(int16_t thing_index) {
    if (game_version == GAME_VERSION_E1)
        return thing_index;
    if (thing_index == THING_SUB)
        return g_code_sub ? g_code_sub->name_index : THING_TAB_SIZE;
    if (thing_index == THING_OBJ)
        return g_code_obj ? g_code_obj->name_index : THING_TAB_SIZE;
    return thing_index;
}

/* Check if the token's upper nibble indicates a "no value" marker.
 * token & 0xF000 promotes to a plain non-negative int (0x0000-0xF000), so
 * the right side must stay a plain int too — casting it to int16_t first
 * sign-extends it back to negative on the way to the comparison and the
 * two sides can then never match. */
static bool check_token_value_exist(int16_t token) {
    return (token & 0xF000) == 0xF000;
}

/* Check if the token's upper nibble indicates a string/second-value marker.
 * Same int16_t sign-extension trap as check_token_value_exist() above. */
UNUSED_ATTR static bool check_token_second_value_exist(int16_t token) {
    return (token & 0xF000) == 0xE000;
}

/* skip_to_matching_endif_44E74C — honours nested IF blocks and 0xE000 string tokens. */
void skip_to_matching_endif(int16_t **pp) {
    while (1) {
        int16_t token = **pp;
        if (token == CT_END_IF)
            break;
        if (token == CT_IF) {
            ++(*pp);
            skip_to_matching_endif(pp);
            ++(*pp);
        } else {
            if ((token & 0xF000) == 0xE000) {
                int len = token & 0xFFF;
                *pp += (len + 1) / 2 + 1;
            } else {
                ++(*pp);
            }
        }
    }
}

/* skip_to_matching_if_type_44E7A4 — stops at ELSE, ELSE_IF or END_IF of the same level. */
void skip_to_matching_if_type(int16_t **pp) {
    while (1) {
        int16_t token = **pp;
        if (token == CT_ELSE || token == CT_ELSE_IF || token == CT_END_IF)
            break;
        if (token == CT_IF) {
            ++(*pp);
            skip_to_matching_endif(pp);
            ++(*pp);
        } else {
            if ((token & 0xF000) == 0xE000) {
                int len = token & 0xFFF;
                *pp += (len + 1) / 2 + 1;
            } else {
                ++(*pp);
            }
        }
    }
}

/* _PartTab slots of the hero's hands: E1 is right=0 / left=1. */
static int right_hand_part(void) {
    return (game_version == GAME_VERSION_E1) ? E1_RIGHT_HAND_PART : E2_RIGHT_HAND_PART;
}
static int left_hand_part(void) {
    return (game_version == GAME_VERSION_E1) ? E1_LEFT_HAND_PART : E2_LEFT_HAND_PART;
}

/* execute_boolean_44E810 — advances *pp past the consumed tokens. */
static int execute_boolean_body(int16_t **pp, actor_t *actor);

int execute_boolean(int16_t **pp, actor_t *actor) {
#ifdef ECS_PROFILE
    int16_t *tp0 = *pp;
    int key = PROF_OP_BOOL + (*tp0 == CT_NOT ? tp0[1] : tp0[0]);
    uint32_t t0 = prof_clock_us();
    int r = execute_boolean_body(pp, actor);
    prof_op(key, prof_clock_us() - t0);
    return r;
#else
    return execute_boolean_body(pp, actor);
#endif
}

static int execute_boolean_body(int16_t **pp, actor_t *actor) {
    bool result = false;
    bool inverted = false;
    int16_t *tp = *pp;

    if (*tp == CT_NOT) {
        inverted = true;
        tp++;
    }

    int16_t token = *tp;
    if (token == CT_SKIP_KEY_PRESSED)
        token = CT_ANY_KEY_PRESSED;

    if (token == CT_STARTED || token == CT_NOT_STARTED ||
        token == CT_FINISHED || token == CT_NOT_FINISHED ||
        token == CT_NOT_PLAYING || token == CT_SCENE_FLAGGED) {
        int16_t scene_index = get_value_from_token(tp[1]);
        if (scene_index >= SCENE_TAB_SIZE) {
            do_info_req("Scene name error in boolean!");
            result = false;
        } else {
            int16_t flags = scene_name_flags[scene_index];
            if (token == CT_STARTED)
                result = (flags & 2) != 0;
            else if (token == CT_NOT_STARTED)
                result = (flags & 2) == 0;
            else if (token == CT_FINISHED)
                result = (flags & 4) != 0;
            else if (token == CT_NOT_FINISHED)
                result = (flags & 4) == 0;
            else if (token == CT_NOT_PLAYING) {
                if ((flags & 2) && !(flags & 4))
                    result = false;
                else
                    result = true;
            } else if (token == CT_SCENE_FLAGGED)
                result = (flags & 8) != 0;
        }
        tp += 2;
    }
    else if (token == CT_FACING_NORTH) {
        if (selected_thing)
            result = ((selected_thing->rotate_vector.Y + 0x4000) & 0x8000) != 0;
        tp++;
    }
    else if (token == CT_FACING_SOUTH) {
        if (selected_thing)
            result = ((selected_thing->rotate_vector.Y + 0x4000) & 0x8000) == 0;
        tp++;
    }
    else if (token == CT_FACING_EAST) {
        if (selected_thing)
            result = (selected_thing->rotate_vector.Y & 0x8000) != 0;
        tp++;
    }
    else if (token == CT_FACING_WEST) {
        if (selected_thing)
            result = (selected_thing->rotate_vector.Y & 0x8000) == 0;
        tp++;
    }
    else if (token == CT_ANY_KEY_PRESSED || token == CT_NO_KEY_PRESSED) {
        /* ESC deliberately absent: get_joystick claims it for the menu and
         * clears it in the same frame, so a script reading it here would race
         * the pause menu for the same press. */
        result = key1_pressed || key2_pressed || key3_pressed ||
                 key4_pressed || key5_pressed || key6_pressed ||
                 key7_pressed || key8_pressed || key9_pressed ||
                 space_pressed || enter_pressed;
        if (!joystick_control) {
            if (scene_name_flags[497] & 2)
                result = true;
        }
        if (token == CT_NO_KEY_PRESSED)
            result = !result;
        tp++;
    }
    else if (token == CT_KEY1_PRESSED) {
        result = key1_pressed != 0;
        tp++;
    }
    else if (token == CT_KEY3_PRESSED) {
        result = key3_pressed != 0;
        tp++;
    }
    else if (token == CT_KEY1_OR_3_PRESSED) {
        result = key1_pressed || key3_pressed;
        tp++;
    }
    else if (token == CT_SPACE_PRESSED) {
        result = space_pressed != 0;
        tp++;
    }
    else if (token == CT_FEMALE) {
        result = female;
        tp++;
    }
    else if (token == CT_ENGLISH) {
        result = (language == 0);
        tp++;
    }
    else if (token == CT_FRENCH) {
        result = (language == 2);
        tp++;
    }
    else if (token == CT_GERMAN) {
        result = (language == 1);
        tp++;
    }
    else if (token == CT_ITALIAN) {
        result = (language == 4);
        tp++;
    }
    else if (token == CT_SPANISH) {
        result = (language == 5);
        tp++;
    }
    else if (token == CT_POLISH) {
        result = (language == 6);
        tp++;
    }
    else if (token == CT_JAPANESE) {
        result = false;
        tp++;
    }
    else if (token == CT_IN_RIGHT_HAND) {
        int16_t actor_index = get_value_from_token(tp[1]);
        result = false;
        if (actor_index < THING_TAB_SIZE && selected_thing) {
            part_t *part = selected_thing->_PartTab->field_0[right_hand_part()];
            if (part) {
                actor_t *held = part->actor_2_held;
                /* 0x443710: a scene-bound held thing only counts while the
                 * holder is itself in a scene. */
                if (held && actor_index == held->name_index &&
                    (!held->actor_scene || part->parent_actor->actor_scene))
                    result = true;
            }
        }
        tp += 2;
    }
    else if (token == CT_IN_LEFT_HAND) {
        int16_t actor_index = get_value_from_token(tp[1]);
        result = false;
        if (actor_index < THING_TAB_SIZE && selected_thing) {
            part_t *part = selected_thing->_PartTab->field_0[left_hand_part()];
            if (part) {
                actor_t *held = part->actor_2_held;
                if (held && actor_index == held->name_index &&
                    (!held->actor_scene || part->parent_actor->actor_scene))
                    result = true;
            }
        }
        tp += 2;
    }
    else if (token == CT_LEFT_HAND_FREE) {
        /* 0x443CC2: false when the hand part does not exist, not true. */
        result = false;
        if (selected_thing) {
            part_t *part = selected_thing->_PartTab->field_0[left_hand_part()];
            if (part)
                result = part->actor_2_held == NULL;
        }
        tp++;
    }
    else if (token == CT_RIGHT_HAND_FREE) {
        result = false;
        if (selected_thing) {
            part_t *part = selected_thing->_PartTab->field_0[right_hand_part()];
            if (part)
                result = part->actor_2_held == NULL;
        }
        tp++;
    }
    else if (token == CT_CAMERA_WAS_OFF) {
        if (check_token_value_exist(tp[1]))
            do_info_req("2 numbers expected after 'CameraWasOff'");
        int16_t cam_idx = unpack_token(tp[1]);
        if (check_token_value_exist(tp[2]))
            do_info_req("2 numbers expected after 'CameraWasOff'");
        result = false;
        int cam_time = unpack_token(tp[2]) * 70 / 10;
        if (cam_idx < 1200 && game_time - camera[cam_idx].time > cam_time)
            result = true;
        tp += 3;
    }
    else if (token == CT_REPIS) {
        int16_t actor_index = resolve_sub_obj(get_value_from_token(tp[1]));
        int16_t rep_index = get_value_from_token(tp[2]);
        result = false;
        if (actor_index < THING_TAB_SIZE) {
            actor_t *a = thing_tab[actor_index];
            if (a && rep_index == a->actor_rep_index)
                result = true;
        }
        tp += 3;
    }
    else if (token == CT_CHECK_ACTOR) {
        /* E1 0x443924 / E2 0x44ED65: an actor still held in thing_tab only
         * counts as present while flagged or seen in the last 1400 ticks, and
         * any displayed actor with flag 2 set vetoes the check. */
        int16_t actor_index = get_value_from_token(tp[1]);
        result = true;
        if (actor_index < THING_TAB_SIZE) {
            actor_t *a = thing_tab[actor_index];
            if (a && ((a->flags & 1) || game_time - a->time_actor < 1400))
                result = false;
        }
        for (actor_t *a = root_thing; a; a = a->next_in_display_list)
            if (a->flags & 2)
                result = false;
        tp += 2;
    }
    else if (token == CT_ACTOR_IS_DEAD) {
        int16_t actor_index = get_value_from_token(tp[1]);
        result = true;
        if (actor_index < THING_TAB_SIZE)
            result = (thing_name_flags[actor_index] & 4) != 0;
        tp += 2;
    }
    else if (token == CT_ACTOR_IS_NEAR) {
        int16_t actor_index = get_value_from_token(tp[1]);
        result = false;
        int distance = unpack_token(tp[2]) << 6;
        if (actor && actor_index < THING_TAB_SIZE) {
            actor_t *a = thing_tab[actor_index];
            if (a) {
                int16_t dx = abs(actor->position_vector.X - a->position_vector.X);
                int16_t dy = abs(actor->position_vector.Y - a->position_vector.Y);
                int16_t dz = abs(actor->position_vector.Z - a->position_vector.Z);
                if (dx < distance && dy < 2 * distance && dz < distance)
                    result = true;
            }
        }
        tp += 3;
    }
    else if (token == CT_ACTIVATED_BELOW) {
        int num_part = 0;
        if (check_token_value_exist(tp[1]))
            do_info_req("Number expected after 'ActivatedBelow'");
        for (actor_t *a = root_thing; a; a = a->next_in_display_list) {
            if (a->flags & 8) {
                for (part_t *p = a->actor_parts_list; p; p = p->next_in_display_list)
                    num_part++;
            }
        }
        result = num_part < unpack_token(tp[1]);
        tp += 2;
    }
    else if (token == CT_HIT_POINTS_ABOVE || token == CT_HITPTS_ABOVE_X10) {
        result = false;
        if (check_token_value_exist(tp[1]))
            do_info_req("Number expected after 'HitPointsAbove'");
        int hp = unpack_token(tp[1]);
        if (token == CT_HITPTS_ABOVE_X10)
            hp *= 10;
        if (actor && actor->actor_hitpoints > hp)
            result = true;
        tp += 2;
    }
    else if (token == CT_MAGIC_BELOW) {
        result = false;
        if (check_token_value_exist(tp[1]))
            do_info_req("Number expected after 'MagicBelow'");
        int magic = unpack_token(tp[1]);
        if (actor && actor->actor_magic < magic)
            result = true;
        tp += 2;
    }
    else if (token == CT_PART_IS) {
        int16_t actor_index = get_value_from_token(tp[1]);
        if (g_execute_part != NULL) {
            result = g_execute_part->actor_2_held != NULL
                     && g_execute_part->actor_2_held->name_index == actor_index;
        } else if (selected_thing && selected_thing->_PartTab) {
            int rh = (game_version == GAME_VERSION_E1) ? 1 : 8;
            int lh = (game_version == GAME_VERSION_E1) ? 0 : 7;
            part_t *p;
            result = false;
            p = selected_thing->_PartTab->field_0[rh];
            if (p && p->actor_2_held && p->actor_2_held->name_index == actor_index)
                result = true;
            if (!result) {
                p = selected_thing->_PartTab->field_0[lh];
                if (p && p->actor_2_held && p->actor_2_held->name_index == actor_index)
                    result = true;
            }
        } else {
            result = false;
        }
        tp += 2;
    }
    else if (token == CT_OBJECT_IS) {
        int16_t actor_index = get_value_from_token(tp[1]);
        /* E2 0x44EEB5 tests the code's object, not the actor running it. */
        actor_t *obj = (game_version == GAME_VERSION_E1) ? actor : g_code_obj;
        result = (obj != NULL && obj->name_index == actor_index);
        tp += 2;
    }
    else if (token == CT_RANDOM) {
        if (check_token_value_exist(tp[1]))
            do_info_req("Number expected after 'Random'");
        int random_value = rand() & 0xFFFF;
        result = 100 * random_value >> 16 < unpack_token(tp[1]);
        tp += 2;
    }
    else if (token == CT_TIMED_EXISTS) {
        int16_t actor_index = get_value_from_token(tp[1]);
        int16_t taction_index = get_value_from_token(tp[2]);
        result = false;
        if (actor_index < THING_TAB_SIZE) {
            actor_t *a = thing_tab[actor_index];
            if (a) {
                for (taction_t *ta = a->tactions_list; ta; ta = ta->next) {
                    if (taction_index == ta->taction_index) {
                        result = true;
                        break;
                    }
                }
            }
        }
        tp += 3;
    }
    else if (token == CT_DEMO) {
        result = false; /* not in demo mode */
        tp++;
    }
    else {
        DBG_LOG(1, "[CODE] unexpected boolean token %d (%d %d %d) actor=%s\n", token,
                tp[1], tp[2], tp[3], actor ? file_find_thing_name(actor->name_index) : "-");
        quit("Unexpected token in boolean expression");
        tp++;
    }

    if (inverted)
        result = !result;

    *pp = tp;
    return result;
}

void check_actor_loaded_by_index(int16_t actor_index);

/* Deliberate deviation: these one-shot event scenes were authored as
 * CT_REPEAT_SCENE, which replays them on every zone re-entry. Treat them as
 * CT_PLAY_SCENE instead. Matched by name: E1's two databases number scenes
 * differently (W/ has an extra entry at 0, so skinny2 is 146 there and 145 in
 * the 320x200 bundle the handhelds load). */
static int scene_is_play_once(int16_t scene_id) {
    static const char *const e1_once[] = {
        "skinny2",      /* the lady dying at the table: skinny1 / table6 / head_ski */
    };

    if (game_version != GAME_VERSION_E1)
        return 0;
    const char *name = file_find_scene_name(scene_id);
    for (size_t i = 0; i < sizeof(e1_once) / sizeof(e1_once[0]); i++)
        if (strcasecmp(name, e1_once[i]) == 0)
            return 1;
    return 0;
}

/* game_do_execute_code_4256B4 — no operand stack; conditions are evaluated
 * inline by execute_boolean(). */
void do_execute_code(code_t *code, actor_t *actor) {
    if (!code) return;

    /* Demo hand-off: turn off only when CT_END_OF_INTRO clears intro_flag.
     * Keeps auto-press active through title-overlay scenes (0, 101) so they
     * advance their subtitles and trigger scene_code_2 → next gameplay scene. */

    int16_t *tokens = &token_store[code->token_store_index];
    if (!tokens) return;

    int pc = 0;
#ifdef ECS_PROFILE
    int prof_prev_op = -1;
    uint32_t prof_prev_t = 0;
#endif

    for (int safety = 0; safety < 10000; safety++) {
        int16_t opcode = tokens[pc++];
#ifdef ECS_PROFILE
        {
            uint32_t now = prof_clock_us();
            if (prof_prev_op >= 0) prof_op(prof_prev_op, now - prof_prev_t);
            prof_prev_op = opcode;
            prof_prev_t = now;
        }
#endif

        if (opcode == 0) {
            return;
        }
        if (opcode == CT_PLAY_SCENE_AFTER_VOICE)
            opcode = CT_PLAY_SCENE;
        else if (opcode == CT_SKIP_KEY_PRESSED)
            opcode = CT_ANY_KEY_PRESSED;

        switch (opcode) {

        case CT_IF: {
            int16_t *tp = &tokens[pc];
            bool cond_result = execute_boolean(&tp, actor);
            if (!cond_result) {
                skip_to_matching_if_type(&tp);
                while (*tp == CT_ELSE_IF) {
                    tp++;
                    if (execute_boolean(&tp, actor))
                        break;
                    skip_to_matching_if_type(&tp);
                }
                if (*tp == CT_ELSE || *tp == CT_END_IF)
                    tp++;
            }
            pc = (int)(tp - tokens);
            continue;
        }

        case CT_ELSE:
        case CT_ELSE_IF: {
            int16_t *tp = &tokens[pc];
            skip_to_matching_endif(&tp);
            tp++;  /* skip past CT_END_IF */
            pc = (int)(tp - tokens);
            continue;
        }

        case CT_END_IF:
            break;

        case CT_NOT:
            break;

        case CT_RANDOM:
            break;

        case CT_STARTED:
        case CT_NOT_STARTED:
        case CT_FINISHED:
        case CT_NOT_FINISHED:
        case CT_NOT_PLAYING:
        case CT_ANY_KEY_PRESSED:
        case CT_NO_KEY_PRESSED:
        case CT_SPACE_PRESSED:
        case CT_FACING_NORTH:
        case CT_FACING_SOUTH:
        case CT_FACING_EAST:
        case CT_FACING_WEST:
        case CT_FEMALE:
        case CT_DEMO:
        case CT_ENGLISH:
        case CT_FRENCH:
        case CT_GERMAN:
        case CT_ITALIAN:
        case CT_SPANISH:
        case CT_POLISH:
        case CT_JAPANESE:
        case CT_CAMERA_WAS_OFF:
        case CT_KEY1_PRESSED:
        case CT_KEY3_PRESSED:
        case CT_KEY1_OR_3_PRESSED:
        case CT_LEFT_HAND_FREE:
        case CT_RIGHT_HAND_FREE:
        case CT_ACTOR_IS_DEAD:
        case CT_TIMED_EXISTS:
            break;

        case CT_ACTIVATED_BELOW:
        case CT_HIT_POINTS_ABOVE:
        case CT_HITPTS_ABOVE_X10:
        case CT_MAGIC_BELOW:
        case CT_CHECK_ACTOR:
        case CT_SCENE_FLAGGED:
        case CT_REPIS:
        case CT_PART_IS:
        case CT_OBJECT_IS:
        case CT_IN_RIGHT_HAND:
        case CT_IN_LEFT_HAND:
        case CT_ACTOR_IS_NEAR:
            pc++;  /* skip parameter */
            break;

        case CT_GAME_TIMER:
            game_timer_start = unpack_token(tokens[pc++]);
            game_timer = game_timer_start;
            break;

        case CT_FORCE_ACTION: {
            int16_t actor_index = resolve_sub_obj(get_value_from_token(tokens[pc++]));
            int16_t action_index = get_value_from_token(tokens[pc++]);
            if (actor_index < THING_TAB_SIZE) {
                actor_t *target = thing_tab[actor_index];
                if (target && action_index < ACTION_TAB_SIZE) {
                    check_action_loaded(action_index);
                    action_t *action = action_tab[action_index];
                    if (action)
                        target->force_action_to_execute = action;
                }
            }
            break;
        }

        case CT_PLAY_SCENE: {
            int16_t scene_id = tokens[pc++] & 0x0FFF;
            if (scene_id < SCENE_TAB_SIZE && !(scene_name_flags[scene_id] & 2)) {
                check_scene_loaded(scene_id);
                scene_t *scene = scene_tab[scene_id];
                if (scene) {
                    if (!check_scene_ok_to_start(scene)) break;
                    check_actors_in_scene_loaded(scene);
                    DBG_LOG(1, "[SS] PLAY_SCENE scene=%d flags=0x%x\n",
                            scene_id, scene_name_flags[scene_id]);
                    start_scene(scene);
                } else {
                    do_info2_req("Missing scene", file_find_scene_name(scene_id));
                }
            }
            break;
        }

        case CT_REPEAT_SCENE: {
            int16_t scene_id = tokens[pc++] & 0x0FFF;
            if (scene_id < SCENE_TAB_SIZE) {
                bool should_start = false;
                scene_t *scene = scene_tab[scene_id];
                if (!scene || !(scene->scene_use_flag & 1)) {
                    should_start = true;
                } else if (!(scene_name_flags[scene_id] & 2)) {
                    should_start = true;
                } else if (scene_name_flags[scene_id] & 4) {
                    should_start = true;
                }
                if (should_start && scene_is_play_once(scene_id) &&
                    (scene_name_flags[scene_id] & 2)) {
                    should_start = false;
                }
                if (should_start) {
                    check_scene_loaded(scene_id);
                    scene = scene_tab[scene_id];
                    if (scene) {
                        if (!check_scene_ok_to_start(scene)) break;
                        check_actors_in_scene_loaded(scene);
                        DBG_LOG(1, "[SS] REPEAT_SCENE scene=%d flags=0x%x\n",
                                scene_id, scene_name_flags[scene_id]);
                        start_scene(scene);
                    } else {
                        do_info2_req("Missing scene", file_find_scene_name(scene_id));
                    }
                }
            }
            break;
        }

        case CT_DRAW_SCENE:
            /* Fatal — should not be encountered at runtime */
            pc++;
            break;

        case CT_PLAY_END_SCENE: {
            int16_t scene_id = tokens[pc++] & 0x0FFF;
            if (scene_id < SCENE_TAB_SIZE) {
                play_dead_scene(scene_id);
            }
            break;
        }

        case CT_ADD_SCENE: {
            /* E1-only: add actors from a scene into the world without
             * playing the scene's scripted actions.  E2 quits here
             * ("Unexpected adds") — token is never emitted for E2. */
            int16_t scene_id = tokens[pc++] & 0x0FFF;
            if (scene_id < SCENE_TAB_SIZE) {
                check_scene_loaded(scene_id);
                scene_t *scene = scene_tab[scene_id];
                if (scene) {
                    check_actors_in_scene_loaded(scene);
                } else {
                    do_info2_req("Missing scene", file_find_scene_name(scene_id));
                }
            }
            break;
        }

        case CT_INIT_SCENE: {
            /* Initialize scene state — next token is scene index.
             * Mirrors asm loc_44F7F2: prime each script's actor with sentinel
             * actor_last_act = (scene_idx+1) | 0xC000 so subsequent swap_in_actor
             * runs the scene action without firing events. */
            int16_t scene_id = tokens[pc++] & 0x0FFF;
            if (startup_repair && scene_id < SCENE_TAB_SIZE
                && (scene_name_flags[scene_id] & 6))
                break;
            if (scene_id < SCENE_TAB_SIZE) {
                check_scene_loaded(scene_id);
                scene_t *scene = scene_tab[scene_id];
                if (scene) {
                    /* Pass 1: reset actor held-tables + initialise_actor for every script */
                    for (script_t *scr = scene->scene_script_list; scr; scr = scr->next_script) {
                        int16_t ai = scr->script_actor_index;
                        if (ai >= 0 && ai < THING_TAB_SIZE) {
                            actor_held_by_part[ai] = -1;
                            actor_held_by_actor[ai] = -1;
                            actor_t *th = thing_tab[ai];
                            if (th) initialise_actor(th);
                        }
                    }
                    /* Pass 2: per-script, walk first key's events; on SCRIPT_MOVE copy
                     * actor_position into event params; on INTERACT param1==3 wire
                     * held-by side tables. If any events found, prime actor_last_act
                     * with (scene_idx+1) | 0xC000 and set thing_name_flags bit 1. */
                    for (script_t *scr = scene->scene_script_list; scr; scr = scr->next_script) {
                        int16_t ai = scr->script_actor_index;
                        if (ai < 0 || ai >= THING_TAB_SIZE) continue;
                        key_state_t *key = scr->script_action.key_list;
                        if (!key) continue;
                        event_t *evt = key->key_event_list;
                        if (!evt) continue;
                        for (; evt; evt = evt->next) {
                            if (evt->event_type == 0x16 /* SCRIPT_MOVE */) {
                                copy_vector(&actor_position[ai], (vector_t *)&evt->param1);
                                /* asm +3B5: also write live heap struct so already-
                                 * loaded actors get repositioned immediately. */
                                actor_t *th2 = thing_tab[ai];
                                if (th2) copy_vector(&th2->position_vector, (vector_t *)&evt->param1);
                            } else if (evt->event_type == 0x2E /* INTERACT */ && evt->param1 == 3) {
                                if (evt->param2 >= 0 && evt->param2 < THING_TAB_SIZE) {
                                    actor_held_by_actor[evt->param2] = ai;
                                    actor_held_by_part[evt->param2] = evt->event_index;
                                }
                            }
                        }
                        actor_last_act[ai] = (int16_t)((scene->scene_index + 1) | 0xC000);
                        thing_name_flags[ai] |= 2;
                    }
                    scene->scene_time = game_time - 1;
                    scene_name_flags[scene_id] |= 6;
                }
            }
            break;
        }

        case CT_DRAW_ROOF:
            break;

        case CT_SUBTITLE: {
            clear_subtitles = 1;
            int16_t sub_index = unpack_token(tokens[pc++]);
            int16_t name_length = unpack_token(tokens[pc++]);
            if (subtitles_on) {
                int16_t volume = 127;
                if (actor && actor != selected_thing && selected_thing) {
                    /* 0x44484D: horizontal X/Z distance only — Y does not take part. */
                    int16_t dir, dist16;
                    find_direction_and_distance(&dir, &dist16,
                        (int16_t)(actor->position_vector.X - selected_thing->position_vector.X),
                        (int16_t)(actor->position_vector.Z - selected_thing->position_vector.Z));
                    int dist = dist16;
                    if (actor->actor_scene != NULL) dist -= 1024;
                    if (dist < 0) dist = 0;
                    volume = (0x2000 - dist) >> 6;
                    if (volume < 0) volume = 0;
                    if (volume > 127) volume = 127;
                }
                if (volume > 64 && sub_index >= 0 && sub_index < 20) {
                    subtitle_length[sub_index] = name_length;
                    subtitle_text[sub_index] = (char *)&tokens[pc];
                    subtitle_status[sub_index] = 1;
                    subtitle_offset[sub_index] = (screen_width - 6 * name_length - 2) / 2;
                    subtitle_colour[sub_index] = 6;
                    subtitles_time = game_time;
                    DBG_LOG(2, "[SUBT] post slot=%d len=%d game_time=%d rt=%d (drift %d)\n",
                            (int)sub_index, (int)name_length, (int)game_time,
                            (int)my_time(), (int)(my_time() - game_time / 3));
                }
            }
            pc += (name_length + 1) / 2;
            break;
        }

        case CT_CLEAR_SUBTITLES:
            clear_subtitles = 1;
            break;

        case CT_RENDER_VIEWS:
            break;

        case CT_SET_SCENE_FLAG: {
            int16_t scene_index = get_value_from_token(tokens[pc++]);
            if (scene_index < SCENE_TAB_SIZE) {
                scene_name_flags[scene_index] |= 8;
            }
            break;
        }

        case CT_CLEAR_SCENE_FLAG: {
            int16_t scene_index = get_value_from_token(tokens[pc++]);
            if (scene_index < SCENE_TAB_SIZE)
                scene_name_flags[scene_index] &= ~8;
            break;
        }

        case CT_MAKE_REP: {
            int16_t actor_index = resolve_sub_obj(get_value_from_token(tokens[pc++]));
            int16_t rep_index = get_value_from_token(tokens[pc++]);
            if (actor_index < THING_TAB_SIZE) {
                actor_t *target = thing_tab[actor_index];
                if (target && rep_index < 500)
                    target->actor_rep_index = rep_index;
                actor_rep_name[actor_index] = rep_index;
            }
            break;
        }

        case CT_MAP_AREA_HEIGHT: {
            int16_t map_area_index = get_value_from_token(tokens[pc++]);
            int16_t height_adj = get_value_from_token(tokens[pc++]);
            if (map_area_index < MAP_AREA_TAB_SIZE && map_area_tab[map_area_index]) {
                for (int i = 0; i < 10; i++) {
                    uint16_t elem_idx = map_area_tab[map_area_index]->map_area_element_num[i];
                    if (elem_idx != 0xFFFF && elem_idx < 60000) {
                        map_elements[elem_idx].height = map_elements[elem_idx].def_height + height_adj;
                    }
                }
            }
            break;
        }

        case CT_START_TUNE: {
            int16_t tune_id = unpack_token(tokens[pc++]);
            play_tune(tune_id);
            break;
        }

        case CT_STOP_TUNE:
            stop_tune();
            break;

        case CT_FADE_OUT_TUNE:
            fade_tune(0, 4);
            break;

        case CT_PUT_GRAPHIC: {
            int16_t pos_x = unpack_token(tokens[pc++]);
            int16_t pos_y = unpack_token(tokens[pc++]);
            int16_t name_length = unpack_token(tokens[pc++]);
            if (name_length > 0 && name_length <= 8) {
                char gr_name[12];
                memcpy(gr_name, &tokens[pc], name_length);
                gr_name[name_length] = 0;
                put_a_graphic(gr_name, pos_x, pos_y, 1);
            }
            pc += (name_length + 1) / 2;
            break;
        }
        case CT_LOAD_GRAPHIC: {
            int16_t name_length = unpack_token(tokens[pc++]);
            if (name_length > 0 && name_length <= 8) {
                char gr_name[12];
                memcpy(gr_name, &tokens[pc], name_length);
                gr_name[name_length] = 0;
                load_a_graphic(gr_name);
            }
            pc += (name_length + 1) / 2;
            break;
        }

        case CT_CLEAR_GRAPHIC: {
            int16_t name_length = unpack_token(tokens[pc++]);
            if (name_length > 0 && name_length <= 8) {
                char gr_name[12];
                memcpy(gr_name, &tokens[pc], name_length);
                gr_name[name_length] = 0;
                clear_a_graphic(gr_name);
            }
            pc += (name_length + 1) / 2;
            break;
        }

        case CT_REMOVE_ALL_GRAPHICS:
            remove_all_graphics();
            break;

        case CT_SWAP_HANDS:
            if (selected_thing && selected_thing->_PartTab) {
                part_t *rh = selected_thing->_PartTab->field_0[right_hand_part()];
                part_t *lh = selected_thing->_PartTab->field_0[left_hand_part()];
                if (rh && lh) {
                    actor_t *tmp = rh->actor_2_held;
                    rh->actor_2_held = lh->actor_2_held;
                    lh->actor_2_held = tmp;
                    if (rh->actor_2_held)
                        rh->actor_2_held->part_heap_link = (part_t *)rh;
                    if (lh->actor_2_held)
                        lh->actor_2_held->part_heap_link = (part_t *)lh;
                }
            }
            break;

        case CT_CANT_BE_HIT:
            if (actor) actor->flags |= ACTOR_FLAG_CANT_BE_HIT;
            break;

        case CT_CAN_BE_HIT:
            if (actor) actor->flags &= (uint16_t)~ACTOR_FLAG_CANT_BE_HIT;
            break;

        case CT_MAKE_HIDDEN:
            if (selected_thing) selected_thing->flags |= ACTOR_FLAG_HIDDEN;
            break;

        case CT_MAKE_VISIBLE:
            if (selected_thing) selected_thing->flags &= (uint16_t)~ACTOR_FLAG_HIDDEN;
            break;

        case CT_MAKE_DEAD: {
            actor_t *target = actor ? actor : selected_thing;
            if (target) make_dead(target);
            break;
        }

        case CT_MAKE_ACTOR_DEAD:
            if (actor)
                make_dead(actor);
            break;

        case CT_ADJUST_HIT_POINTS: {
            int hp = unpack_token(tokens[pc++]);
            actor_t *target = actor ? actor : selected_thing;
            if (target) {
                target->actor_hitpoints += hp;
                if (target->code_at_hp_change >= 0 && target->code_at_hp_change < CODE_TAB_SIZE) {
                    code_t *hp_code = code_tab[target->code_at_hp_change];
                    if (hp_code)
                        do_execute_code(hp_code, target);
                }
                if (!target->name_index)
                    draw_life_bar();
            }
            break;
        }

        case CT_CAUSE_GET_HIT: {
            actor_t *target = actor ? actor : selected_thing;
            if (target) {
                target->actor_behavior = BH_GET_HIT;
                target->flags |= 0x2000;
            }
            break;
        }

        case CT_SET_HIT_POINTS:
        case CT_SET_HITPT_X100: {
            int hp = unpack_token(tokens[pc++]);
            if (opcode == CT_SET_HITPT_X100) hp *= 100;
            actor_t *target = actor ? actor : selected_thing;
            if (target && target->actor_hitpoints != hp) {
                target->actor_hitpoints = hp;
                if (target->code_at_hp_change >= 0 && target->code_at_hp_change < CODE_TAB_SIZE) {
                    code_t *hp_code = code_tab[target->code_at_hp_change];
                    if (hp_code)
                        do_execute_code(hp_code, target);
                }
            }
            break;
        }

        case CT_SET_FULL_HIT_POINTS:
        case CT_SET_FULL_HITPT_X100: {
            int hp = unpack_token(tokens[pc++]);
            if (opcode == CT_SET_FULL_HITPT_X100) hp *= 100;
            if (actor)
                actor->full_actor_hp = hp;
            if (actor && !actor->name_index)
                update_game_icons();
            break;
        }

        case CT_RECOVER_HIT_POINTS: {
            int hp = unpack_token(tokens[pc++]);
            if (actor) {
                hp += actor->actor_hitpoints;
                int full_hp = actor->full_actor_hp;
                actor->actor_hitpoints = hp;
                if (hp > full_hp)
                    actor->actor_hitpoints = full_hp;
                if (!actor->name_index)
                    draw_life_bar();
            }
            break;
        }

        case CT_EXECUTE_CODE: {
            int16_t code_id = tokens[pc++] & 0x0FFF;
            if (code_id < CODE_TAB_SIZE && code_tab[code_id]) {
                do_execute_code(code_tab[code_id], actor);
            }
            break;
        }

        case CT_EXECUTE_ACTOR_CODE: {
            int16_t actor_index = get_value_from_token(tokens[pc++]);
            int16_t code_index = get_value_from_token(tokens[pc++]);
            if (actor_index < THING_TAB_SIZE) {
                actor_t *target_actor = thing_tab[actor_index];
                if (target_actor && code_index < CODE_TAB_SIZE) {
                    code_t *target_code = code_tab[code_index];
                    /* 0x44F52D: the target becomes the subject. */
                    if (target_code)
                        execute_with_sub_obj(target_code, target_actor, target_actor, NULL);
                }
            }
            break;
        }

        case CT_BLOCK_ACTOR:
            pc++;
            break;
        case CT_BLOCK_WANDERERS:
        case CT_BLOCK_ALL:
        case CT_BLOCK_AQUATIC:
            break;

        case CT_FORCE_ESCAPE_KEY:
            if (joystick_control)
                key_esc_was_forced = 1;
            break;

        case CT_FOLLOW_ACTOR: {
            int16_t actor_index = get_value_from_token(tokens[pc++]);
            if (actor) {
                if (actor_index >= THING_TAB_SIZE)
                    actor->interact_target_index = -1;
                else
                    actor->interact_target_index = actor_index;
                actor->field_80 = 15;
            }
            break;
        }

        case CT_SPEED_FACTOR:
            if (actor)
                actor->actor_Speed_factor = unpack_token(tokens[pc]);
            pc++;
            break;

        case CT_FORCE_FOLLOWING: {
            int16_t actor_index = get_value_from_token(tokens[pc++]);
            int16_t action_index = get_value_from_token(tokens[pc++]);
            if (actor_index < THING_TAB_SIZE) {
                actor_t *target = thing_tab[actor_index];
                if (target && action_index < ACTION_TAB_SIZE) {
                    check_action_loaded(action_index);
                    action_t *action = action_tab[action_index];
                    if (action)
                        target->queued_action = action;
                }
            }
            break;
        }

        case CT_FORCE_TIMED: {
            int16_t actor_index = get_value_from_token(tokens[pc++]);
            int16_t taction_index = get_value_from_token(tokens[pc++]);
            int16_t action_ticks = unpack_token(tokens[pc++]);
            force_timed(actor_index, taction_index, action_ticks);
            break;
        }

        case CT_CANT_STOP:
            actor->flags |= ACTOR_FLAG_CANT_STOP;
            break;

        case CT_CAN_STOP:
            actor->flags &= (uint16_t)~ACTOR_FLAG_CANT_STOP;
            break;

        case CT_LOAD_HERO: {
            int16_t actor_index = get_value_from_token(tokens[pc++]);
            if (actor_index < THING_TAB_SIZE) {
                actor_t *existing = thing_tab[actor_index];
                if (existing) {
                    thing_tab[existing->name_index] = NULL;
                    remove_from_display_list(existing);
                    if (existing == thing_list) {
                        thing_list = thing_list->next_thing1;
                    } else {
                        for (actor_t *prev = thing_list; prev && prev->next_thing1; prev = prev->next_thing1) {
                            if (prev->next_thing1 == existing) {
                                prev->next_thing1 = existing->next_thing1;
                                break;
                            }
                        }
                    }
                    do_delete_thing(existing);
                }
                thing_name_flags[actor_index] &= ~2; /* Clear loaded flag */
                check_actor_loaded_by_index(actor_index);
                selected_thing = thing_tab[actor_index];
                if (selected_thing)
                    selected_thing->actor_behavior = BH_JOYSTICK;
            }
            break;
        }

        case CT_LOAD_ACTOR: {
            int16_t actor_index = get_value_from_token(tokens[pc++]);
            if (actor_index < THING_TAB_SIZE) {
                check_actor_loaded_by_index(actor_index);
            }
            break;
        }

        case CT_STRENGTH_FACTOR:
            if (actor)
                actor->actor_strength_factor = unpack_token(tokens[pc]);
            if (actor && !actor->name_index)
                update_game_icons();
            pc++;
            break;
        case CT_HIT_FACTOR:
            if (actor)
                actor->actor_hit_factor = unpack_token(tokens[pc]);
            pc++;
            break;
        case CT_ARMOUR_FACTOR:
            armour_factor = unpack_token(tokens[pc]);
            pc++;
            break;
        case CT_MAGIC_FACTOR:
            if (actor)
                actor->actor_magic_factor = unpack_token(tokens[pc]);
            pc++;
            break;

        case CT_JUMP:
            /* v = (arg3, -arg2, arg1); velocity = actor->matrix33_2 * v, so the
             * jump follows the actor's facing, not the view. */
            if (actor) {
                vector_t input_vec, dst_vec;
                set_vector(&input_vec,
                    unpack_token(tokens[pc + 2]),
                    -unpack_token(tokens[pc + 1]),
                    unpack_token(tokens[pc]));
                matrix_vector(&input_vec, &dst_vec, &actor->matrix33_2);
                copy_vector(&actor->actor_velocity, &dst_vec);
            }
            pc += 3;
            break;

        case CT_REMOVE_THIS_ACTOR:
            if (actor) {
                remove_actor_from_world(actor);
            }
            break;

        case CT_REMOVE_ACTOR: {
            int16_t actor_index = get_value_from_token(tokens[pc++]);
            if (actor_index < THING_TAB_SIZE) {
                if (thing_tab[actor_index])
                    remove_actor_from_world(thing_tab[actor_index]);
                else
                    thing_name_flags[actor_index] &= ~2;
            }
            break;
        }

        case CT_TREASURE:
            treasure_count += unpack_token(tokens[pc++]);
            break;

        case CT_SPAWN_LIVE: {
            /* SpawnActor(g_execute_part, ai, action, 1, 0); no-op outside
             * execute_part_code. */
            int16_t actor_index = get_value_from_token(tokens[pc++]);
            int16_t action_index = get_value_from_token(tokens[pc++]);
            part_t *part1 = g_execute_part;
            if (part1 && actor_index < THING_TAB_SIZE && action_index < ACTION_TAB_SIZE)
                spawn_actor(part1, actor_index, action_index, 1, 0);
            break;
        }

        case CT_RANDOMIZE:
            my_rand();
            break;

        case CT_ADD_MAGIC:
            if (actor)
                adjust_magic(actor, unpack_token(tokens[pc]));
            pc++;
            break;
        case CT_ADD_MAGIC_TO: {
            int16_t actor_index = get_value_from_token(tokens[pc++]);
            int16_t magic = unpack_token(tokens[pc++]);
            if (actor_index < THING_TAB_SIZE && thing_tab[actor_index])
                adjust_magic(thing_tab[actor_index], magic);
            break;
        }

        case CT_MAGIC_STOP_ACTION:
            if (actor)
                actor->magic_stop_action = get_value_from_token(tokens[pc]);
            pc++;
            break;

        case CT_WANDERERS_ON:
            no_wanderers = 0;
            break;

        case CT_REMOVE_TIMED: {
            int16_t actor_index = get_value_from_token(tokens[pc++]);
            int16_t taction_index = get_value_from_token(tokens[pc++]);
            if (actor_index < THING_TAB_SIZE) {
                actor_t *target = thing_tab[actor_index];
                if (target && taction_index < ACTION_TAB_SIZE) {
                    while (target->tactions_list && taction_index == target->tactions_list->taction_index) {
                        taction_t *old = target->tactions_list;
                        target->tactions_list = old->next;
                        old->taction_index = -1;
                    }
                    int found;
                    do {
                        found = 0;
                        taction_t *prev = target->tactions_list;
                        if (prev) {
                            while (prev->next) {
                                if (taction_index == prev->next->taction_index) {
                                    taction_t *old = prev->next;
                                    old->taction_index = -1;
                                    prev->next = old->next;
                                    found = 1;
                                    break;
                                }
                                prev = prev->next;
                            }
                        }
                    } while (found);
                }
            }
            break;
        }

        case CT_SMART_BOMB: {
            /* SmartBomb(actor, range, damage) — 2 unpacked int16 params */
            int range = unpack_token(tokens[pc++]);
            int damage = unpack_token(tokens[pc++]);
            if (actor)
                smart_bomb(actor, range, damage);
            break;
        }

        case CT_AMBIANT_SOUND: {
            /* AmbiantName[] is int[] storing sound_index
             * (asm `ambiant_name_AC493C[eax]` 4-byte stride). */
            int16_t sound_index = get_value_from_token(tokens[pc++]);
            int ambiant_freq_val = unpack_token(tokens[pc++]);
            int ambiant_rand_val = unpack_token(tokens[pc++]);
            int ambiant_volume = unpack_token(tokens[pc++]);
            if (sound_index >= 0 && sound_index < 700 && num_ambients < 19) {
                int idx;
                for (idx = 0; idx < num_ambients; ++idx)
                    if (ambiant_name[idx] == sound_index) break;
                if (idx == num_ambients) {
                    ambiant_name[idx] = sound_index;
                    num_ambients = idx + 1;
                }
                if (idx < 20) {
                    ambiant_freq[idx] = ambiant_freq_val;
                    ambiant_vol[idx] = ambiant_volume;
                    ambiant_rand[idx] = ambiant_rand_val;
                }
            }
            break;
        }

        case CT_CLEAR_AMBIANT:
            num_ambients = 0;
            break;

        case CT_FADE_TO_BLACK:
        case CT_FADE_TO_WHITE:
        case CT_FADE_IN: {
            fade_time = unpack_token(tokens[pc++]);
            if (opcode == CT_FADE_TO_BLACK) fade_to_black = 1;
            else if (opcode == CT_FADE_TO_WHITE) fade_to_white = 1;
            else if (opcode == CT_FADE_IN && (fade_to_black || fade_to_white)) fade_in = 1;
            fade_start = my_time();
            last_fade_factor = -1;
            /* Only FADE_IN with duration 0 applies at once. FADE_TO_BLACK/WHITE
             * keep their flag set so the following FADE_IN sees an active
             * fade-out; otherwise the view palette stays zeroed. */
            if (!fade_time && opcode == CT_FADE_IN)
                check_fade();
            break;
        }

        case CT_END_OF_INTRO:
            intro_flag = 0;
            remove_all_graphics();
            if (!no_icons)
                update_game_icons();
            break;

        case CT_QUIT_TO_DOS:
            DBG_LOG(1, "[EXEC] QuitToDos in code=%d\n", code->index_code);
            program_up_and_running = 0;
            break;

        default:
            DBG_LOG(1, "[EXEC] UNHANDLED opcode=%d in code=%d pc=%d\n", opcode, code->index_code, pc-1);
            break;
        }
    }
}

/* game_find_empty_actor_slot  E1: ? | E2P: 0x426128 */
UNUSED_ATTR
static int find_empty_actor_slot(void) {
    for (int i = 1; i < ACTOR_POOL_SIZE; i++) {
        if (!(actor_flags[i] & ACTOR_FLAG_ACTIVE)) {
            return i;
        }
    }
    return -1;
}

/* game_try_to_add_actor_to_world_44DCA4
 * Wanderer spawner. Picks template per map element's wanderer_spawn byte,
 * places at random offset near selected_thing, ground-snapped, with
 * bounding-box collision check and 9-point ground-flatness gate. */
void try_to_add_actor_to_world(void) {
    vector_t position[9];
    if (!selected_thing) return;

    vector_t spawn_point;
    spawn_point.X = (int16_t)(selected_thing->position_vector.X + ((10 * (my_rand() - 0x4000)) >> 5));
    spawn_point.Z = (int16_t)(selected_thing->position_vector.Z + ((10 * (my_rand() - 0x4000)) >> 5));
    spawn_point.Y = selected_thing->position_vector.Y;
    spawn_point.Y = find_height_now(&spawn_point, NULL);

    int spawn_type_index = 0;
    if (position_is_visible(&spawn_point)) {
        spawn_type_index = 0;
    } else {
        int element_index = find_map_element(&spawn_point);
        if (element_index >= 0)
            spawn_type_index = map_elements[element_index].wanderer_spawn;
    }

    actor_t *new_wanderer = NULL;
    switch (spawn_type_index) {
        case 1:  new_wanderer = load_wanderer(52, 3);  break;  /* w_blob */
        case 2:  new_wanderer = load_wanderer(55, 3);  break;  /* w_slim */
        case 3:  new_wanderer = load_wanderer(16, 3);  break;  /* w_bhold */
        case 4:  new_wanderer = load_wanderer(13, 3);  break;  /* w_gost */
        case 5:  new_wanderer = load_wanderer(9,  4);  break;  /* w_gob1 */
        case 6:  new_wanderer = load_wanderer(19, 3);  break;  /* w_spidl1 */
        case 7:  new_wanderer = load_wanderer(37, 15); break;  /* w_bar1 */
        case 8:  new_wanderer = load_wanderer(31, 3);  break;  /* w_bxorc1 */
        case 9:  new_wanderer = load_wanderer(34, 3);  break;  /* w_strol */
        case 10: new_wanderer = load_wanderer(58, 3);  break;  /* w_cyclp */
        case 11: new_wanderer = load_wanderer(61, 3);  break;  /* w_zomb */
        case 12:
            /* Singleton spawn for w_samuel1 (idx 64). */
            if (!(thing_name_flags[64] & 0x2)) {
                check_actor_loaded(thing_names[64].field_0);
                new_wanderer = thing_tab[64];
            }
            break;
        case 13: new_wanderer = load_wanderer(22, 3);  break;  /* w_spidl2 */
        case 14: new_wanderer = load_wanderer(25, 3);  break;  /* w_spids1 */
        case 15: new_wanderer = load_wanderer(28, 3);  break;  /* w_spids2 */
        case 16: new_wanderer = load_wanderer(68, 3);  break;  /* w_monk */
        case 17: new_wanderer = load_wanderer(71, 3);  break;  /* w_sprit */
        case 18: new_wanderer = load_wanderer(74, 3);  break;  /* w_trol */
        case 19: new_wanderer = load_wanderer(77, 3);  break;  /* w_stond */
        case 20: new_wanderer = load_wanderer(80, 3);  break;  /* w_dorc2 */
        case 21: new_wanderer = load_wanderer(83, 3);  break;  /* w_beye */
        case 22: new_wanderer = load_wanderer(86, 3);  break;  /* w_demon */
        case 23: new_wanderer = load_wanderer(89, 3);  break;  /* w_gagol */
        case 24: new_wanderer = load_wanderer(92, 3);  break;  /* w_wolf */
        case 25: new_wanderer = load_wanderer(95, 3);  break;  /* w_hunbac */
        case 26: new_wanderer = load_wanderer(98, 3);  break;  /* w_skel */
        case 27: new_wanderer = load_wanderer(101, 3); break;  /* w_bnite */
        case 28: new_wanderer = load_wanderer(104, 3); break;  /* w_gnite */
        case 29: new_wanderer = load_wanderer(107, 3); break;  /* w_creep */
        case 30: new_wanderer = load_wanderer(110, 3); break;  /* w_fairy */
        case 31: new_wanderer = load_wanderer(113, 3); break;  /* w_fairy4 */
        case 32: new_wanderer = load_wanderer(116, 3); break;  /* w_fish */
        case 33: new_wanderer = load_wanderer(119, 8); break;  /* w_amaz1 */
        case 34: new_wanderer = load_wanderer(127, 3); break;  /* w_bgel1 */
        case 35: new_wanderer = load_wanderer(130, 3); break;  /* w_mummy */
        case 36: new_wanderer = load_wanderer(133, 3); break;  /* w_mush */
        case 37: new_wanderer = load_wanderer(136, 3); break;  /* w_sgel1 */
        case 38:
            new_wanderer = load_wanderer((scene_name_flags[5] & 0x8) ? 65 : 61, 3);
            break;
        case 39:
            new_wanderer = load_wanderer((scene_name_flags[5] & 0x8) ? 65 : 136, 3);
            break;
        case 40:
            new_wanderer = load_wanderer((scene_name_flags[5] & 0x8) ? 65 : 127, 3);
            break;
        case 41:
            new_wanderer = load_wanderer((scene_name_flags[5] & 0x8) ? 65 : 58, 3);
            break;
        case 42:
            new_wanderer = load_wanderer((scene_name_flags[5] & 0x8) ? 65 : 92, 3);
            break;
        case 43:
            new_wanderer = load_wanderer((scene_name_flags[5] & 0x8) ? 65 : 107, 3);
            break;
        default: break;
    }

    if (!new_wanderer) return;

    new_wanderer->position_vector = spawn_point;
    for (int i = 0; i < 9; i++) position[i] = spawn_point;

    int16_t delta = new_wanderer->actor_box_size;

    for (actor_t *actor = root_thing; actor; actor = actor->next_in_display_list) {
        if (actor == new_wanderer) continue;
        int bounding_box = new_wanderer->actor_box_size + actor->actor_box_size;
        int delta_x = abs(actor->position_vector.X - new_wanderer->position_vector.X);
        int delta_z = abs(actor->position_vector.Z - new_wanderer->position_vector.Z);
        if (delta_x < bounding_box && delta_z < bounding_box) {
            remove_from_display_list(new_wanderer);
            thing_name_flags[new_wanderer->name_index] &= (int16_t)~0x2;
            return;
        }
    }

    /* 3x3 grid of probe points around spawn for ground-flatness. */
    position[0].X -= delta; position[0].Z -= delta;
    position[1].Z -= delta;
    position[2].X += delta; position[2].Z -= delta;
    position[3].X -= delta;
    position[5].X += delta;
    position[6].X -= delta; position[6].Z += delta;
    position[7].Z += delta;
    position[8].X += delta; position[8].Z += delta;

    int16_t hmin = 32767;
    int16_t hmax = -32768;
    for (int i = 0; i < 9; i++) {
        int16_t h = find_height_now(&position[i], NULL);
        if (h < hmin) hmin = h;
        if (h > hmax) hmax = h;
    }

    int16_t ai = new_wanderer->name_index;
    if (hmin == hmax) {
        /* Flat ground — wanderer accepted. asm: flags = (old & 0x70) | 0x08. */
        thing_name_flags[ai] = (int16_t)((thing_name_flags[ai] & 0x70) | ACTOR_FLAG_VISIBLE);
        actor_rep_name[ai] = -2;
        actor_magic[ai] = 0;
        actor_hit_points[ai] = 0;
        actor_position[ai].X = 0; actor_position[ai].Y = 0; actor_position[ai].Z = 0;
        actor_orientation[ai].X = 0; actor_orientation[ai].Y = 0; actor_orientation[ai].Z = 0;
        actor_held_by_part[ai] = -1;
        actor_held_by_actor[ai] = -1;
        actor_last_act[ai] = 0;
        initialise_actor(new_wanderer);
        new_wanderer->flags |= 8;
        thing_name_flags[ai] |= 0x2; /* Wanderer */
        add_to_display_list(new_wanderer);
    } else {
        remove_from_display_list(new_wanderer);
        thing_name_flags[ai] &= (int16_t)~0x2;
    }
}

/* game_remove_actor_from_world_44E238 — saves the actor's state, drops it from
 * the display list, releases its rep and held actors, detaches it from its
 * holder, and marks its scene finished if it was the last script actor. */
void remove_actor_from_world(actor_t *actor) {
    if (!actor) return;

    remove_from_display_list(actor);
    int16_t idx = actor->name_index;
    if (idx >= 0 && idx < THING_TAB_SIZE)
        thing_name_flags[idx] &= (int16_t)~0x2;

    /* Refcount-clear reperture: clear bit 1; restore if another actor in
     * root_thing still references the same reperture. */
    rephead_t *rep = actor->actor_reperture;
    if (rep) {
        rep->rep_use_flag &= (uint16_t)~0x2;
        for (actor_t *a = root_thing; a; a = a->next_in_display_list) {
            if (a->actor_reperture == rep) {
                rep->rep_use_flag |= 0x2;
                break;
            }
        }
        actor->actor_reperture = NULL;
    }

    for (part_t *part = actor->actor_parts_list; part; part = part->next_in_display_list) {
        if (part->actor_2_held)
            remove_actor_from_world(part->actor_2_held);
    }

    if (actor->part_heap_link) {
        actor->part_heap_link->actor_2_held = NULL;
        actor->part_heap_link = NULL;
    }

    if (idx >= 0 && idx < THING_TAB_SIZE) {
        actor_held_by_part[idx] = -1;
        actor_held_by_actor[idx] = -1;
    }

    /* Check if this was the last active script in the actor's scene. */
    scene_t *sc = actor->actor_scene;
    if (sc) {
        bool any_active = false;
        for (script_t *scr = sc->scene_script_list; scr; scr = scr->next_script) {
            int16_t ai = scr->script_actor_index;
            if (ai < 0 || ai >= THING_TAB_SIZE) continue;
            actor_t *script_actor = thing_tab[ai];
            if (!script_actor) continue;
            if (&scr->script_action != script_actor->actor_act.act_action) continue;
            if (!(thing_name_flags[ai] & 0x2)) continue;
            any_active = true;
            break;
        }
        if (!any_active && sc->scene_index >= 0 && sc->scene_index < SCENE_TAB_SIZE)
            scene_name_flags[sc->scene_index] |= 0x4;
    }
}

/* game_check_encounter  E1: 0x442ED0 | E2: 0x44E2A4 */
void check_encounter(void) {
    static int32_t prev_time = 0;

    /* E1 0x442ED0 only retires far-off actors, every frame, and never spawns.
     * It keys on the actor's own flags rather than thing_name_flags: an E1
     * werewolf left behind by its scene carries actor flag 2 only, and if it is
     * never retired CT_CHECK_ACTOR stays false everywhere, so every hot spot
     * waiting for it to leave (the church altar, the were* ambushes) is dead. */
    if (game_version == GAME_VERSION_E1) {
        if (!selected_thing)
            return;
        actor_t *next;
        for (actor_t *actor = root_thing; actor; actor = next) {
            next = actor->next_in_display_list;
            if (actor == selected_thing
                || !(actor->flags & 0x4002)
                || actor->part_heap_link
                || (actor->flags & 8)
                || actor->actor_behavior == BH_DEAD
                || actor->actor_behavior == BH_DYING)
                continue;
            int16_t direction, distance;
            find_direction_and_distance(
                &direction, &distance,
                actor->position_vector.X - selected_thing->position_vector.X,
                actor->position_vector.Z - selected_thing->position_vector.Z);
            if (distance <= 5120)
                continue;
            action_t *action = actor->actor_act.act_action;
            if (action && (action->action_flags & 2))
                continue;
            int16_t ai = actor->name_index;
            actor_position[ai] = actor->position_vector;
            actor_orientation[ai] = actor->rotate_vector;
            actor_rep_name[ai] = actor->actor_rep_index;
            actor_hit_points[ai] = actor->actor_hitpoints;
            remove_actor_from_world(actor);
        }
        return;
    }

    if (!prev_time || game_time - prev_time >= 50) {
        prev_time = game_time;

        if (poison_time)
            draw_life_bar();
        if (!no_wanderers) {
            for (actor_t *actor = thing_list; actor; actor = actor->next_thing1) {
                int16_t actor_flag = thing_name_flags[actor->name_index];
                if ((actor_flag & 2)
                    && actor != selected_thing
                    && ((actor_flag & 8) || (actor->flags & 0x4000))
                    && !actor->part_heap_link
                    && !(actor->flags & 8)
                    && actor->actor_behavior != BH_DYING) {
                    int16_t direction, distance;
                    find_direction_and_distance(
                        &direction, &distance,
                        actor->position_vector.X - selected_thing->position_vector.X,
                        actor->position_vector.Z - selected_thing->position_vector.Z);
                    if (distance > 5120) {
                        action_t *action = actor->actor_act.act_action;
                        if (!action || !(action->action_flags & 2)) {
                            /* Persist wanderer state before yank — respawn later
                             * needs pos/rot/rep/hp/magic (asm 0x44E4B0..0x44E4F1). */
                            int16_t ai = actor->name_index;
                            actor_position[ai] = actor->position_vector;
                            actor_orientation[ai] = actor->rotate_vector;
                            actor_rep_name[ai] = actor->actor_rep_index;
                            actor_hit_points[ai] = actor->actor_hitpoints;
                            actor_magic[ai] = (int16_t)(int8_t)actor->actor_magic;
                            remove_actor_from_world(actor);
                        }
                    }
                }
            }

            int count = 0;
            for (actor_t *actor = root_thing; actor; actor = actor->next_in_display_list) {
                if ((thing_name_flags[actor->name_index] & 8) && actor->actor_behavior != BH_DEAD)
                    count++;
            }
            if (count < 4)
                try_to_add_actor_to_world();
        }
    }
}

/* game_try_to_remove_actor  E1: 0x447988 | E2: 0x452FB4
 * Evicts the oldest actor that is neither displayed nor held by selected_thing. */
void try_to_remove_actor(void) {
    /* asm uses bit 0 of flags as transient "in_display_list" mark, but C
     * has overloaded that bit as ACTOR_FLAG_ACTIVE. Use side-table to
     * avoid clobbering. Indexed by actor->name_index. */
    static bool in_display_mark[THING_TAB_SIZE];
    actor_t *sel = selected_thing;
    int32_t now = game_time;

    for (actor_t *a = thing_list; a; a = a->next_thing1) {
        if (a->name_index >= 0 && a->name_index < THING_TAB_SIZE)
            in_display_mark[a->name_index] = false;
    }
    for (actor_t *a = root_thing; a; a = a->next_in_display_list) {
        if (a->name_index >= 0 && a->name_index < THING_TAB_SIZE)
            in_display_mark[a->name_index] = true;
    }

    actor_t *victim = NULL;
    int32_t oldest = 0;
    for (actor_t *a = thing_list; a; a = a->next_thing1) {
        if (a->name_index < 0 || a->name_index >= THING_TAB_SIZE) continue;
        if (in_display_mark[a->name_index]) continue;
        if (a->part_heap_link && a->part_heap_link->parent_actor == sel)
            continue;
        int32_t age = now - a->time_actor;
        if (age > oldest) { oldest = age; victim = a; }
    }

    selected_thing = sel;
    game_time = now;

    if (victim)
        remove_actor(victim);
}

/* game_try_to_remove_scene  E1: 0x44773C | E2: 0x452D54 */
void try_to_remove_scene(void) {
    scene_t *oldest = NULL;
    int32_t oldest_time = 0;
    for (scene_t *s = scene_list; s; s = s->scene_next) {
        if (!(s->scene_use_flag & 1) && game_time - s->scene_time > oldest_time) {
            oldest = s;
            oldest_time = game_time - s->scene_time;
        }
    }
    if (oldest)
        remove_scene(oldest);
}

/* game_try_to_remove_action  E1: 0x447868 | E2: 0x452E80 */
void try_to_remove_action(void) {
    int32_t save_time = game_time;

    for (action_t *a = action_list; a; a = a->next)
        a->action_flags &= 0xF7FFu;

    for (actor_t *actor = thing_list; actor; actor = actor->next_thing1) {
        act_t *act = &actor->actor_act;
        if (act && !(act->flags & 2))
            act->flags |= 0x0800u;
    }

    action_t *oldest = NULL;
    int32_t oldest_time = -1;
    for (action_t *a = action_list; a; a = a->next) {
        if (!(a->action_flags & 0x08FF) && save_time - a->action_time > oldest_time) {
            oldest = a;
            oldest_time = save_time - a->action_time;
        }
    }

    game_time = save_time;
    if (oldest)
        remove_action(oldest);
}

/* game_try_to_remove_scene_or_action  E1: 0x447798 | E2: 0x452DB0 */
void try_to_remove_scene_or_action(void) {
    int32_t save_time = game_time;

    scene_t *oldest_scene = NULL;
    int32_t oldest_time = 0;
    for (scene_t *s = scene_list; s; s = s->scene_next) {
        if (!(s->scene_use_flag & 1) && (game_time - s->scene_time) > oldest_time) {
            oldest_scene = s;
            oldest_time = game_time - s->scene_time;
        }
    }

    for (action_t *a = action_list; a; a = a->next)
        a->action_flags &= 0xF7FFu;

    for (actor_t *actor = thing_list; actor; actor = actor->next_thing1) {
        action_t *a = actor->actor_act.act_action;
        if (a && !(a->action_flags & 2))
            a->action_flags |= 0x0800u;
    }

    action_t *oldest_action = NULL;
    for (action_t *a = action_list; a; a = a->next) {
        if (!(a->action_flags & 0x0800) && (save_time - a->action_time) > oldest_time) {
            oldest_action = a;
            oldest_time = save_time - a->action_time;
        }
    }

    game_time = save_time;
    if (oldest_action)
        remove_action(oldest_action);
    else if (oldest_scene)
        remove_scene(oldest_scene);
}

/* game_try_to_remove_rep  E1: 0x4478FC | E2: 0x452F14 */
void try_to_remove_rep(void) {
    int32_t save_time = game_time;
    int32_t oldest_time = -1;

    for (rephead_t *r = repertoire_list; r; r = r->next_rep)
        r->rep_use_flag &= 0xFFFDu;

    for (actor_t *actor = root_thing; actor; actor = actor->next_in_display_list) {
        if (actor->actor_reperture)
            actor->actor_reperture->rep_use_flag |= 2u;
    }

    rephead_t *oldest = NULL;
    for (rephead_t *r = repertoire_list; r; r = r->next_rep) {
        if (!(r->rep_use_flag & 2) && save_time - r->rep_time > oldest_time) {
            oldest = r;
            oldest_time = save_time - r->rep_time;
        }
    }

    game_time = save_time;
    if (oldest)
        remove_rep(oldest);
}

/* game_try_to_remove_sound  E1: 0x447A28 | E2: 0x453054 */
void try_to_remove_sound(void) {
    sound_t *oldest = NULL;
    int32_t oldest_time = -1;
    for (sound_t *s = sound_list; s; s = s->next) {
        if (!(s->use_flag & 1) && game_time - s->_time > oldest_time) {
            oldest = s;
            oldest_time = game_time - s->_time;
        }
    }
    if (oldest)
        remove_sound(oldest);
}

/* game_remove_texture  E1: 0x44734C | E2: 0x45292C */
void remove_texture(texture_t *texture_to_remove) {
    if (!texture_to_remove) return;

    if (texture_to_remove->textur_index < 0 ||
        texture_to_remove->textur_index >= TEXTURE_TAB_SIZE) {
        quit("Can't remove Texture - name out of bounds");
        return;
    }
    if (texture_to_remove != texture_tab[texture_to_remove->textur_index]) {
        quit("Can't remove Texture not in TextureTab");
        return;
    }

    if (texture_to_remove == texture_list) {
        texture_list = texture_list->next;
    } else {
        for (texture_t *t = texture_list; t; t = t->next) {
            if (texture_to_remove == t->next) {
                t->next = texture_to_remove->next;
                break;
            }
        }
    }

    texture_tab[texture_to_remove->textur_index] = NULL;

    /* Each texture is its own allocation here, not a slab region, so there is
     * nothing to compact. */
    if (texture_to_remove->texture_data) {
        free(texture_to_remove->texture_data);
        texture_to_remove->texture_data = NULL;
    }

    texture_to_remove->use_flag = 0x8000u;
}

/* game_try_to_remove_texture  E1: 0x447A7C | E2: 0x4530A8 */
void try_to_remove_texture(void) {
    int32_t oldest_age = -1;
    texture_t *candidate = NULL;

    for (texture_t *t = texture_list; t; t = t->next) {
        int32_t age = game_time - t->textur_time;
        if (age > oldest_age) {
            candidate = t;
            oldest_age = age;
        }
    }

    if (candidate)
        remove_texture(candidate);
}

/* game_load_wanderer_44DB80 — picks a random free wanderer slot in
 * [base_type, base_type + num_variations). */
actor_t *load_wanderer(int base_type, int num_variations) {
    if (num_variations <= 0) return NULL;
    int r = my_rand() % num_variations;
    for (int i = 0; i < num_variations; i++) {
        int idx = base_type + (i + r) % num_variations;
        if (idx < 0 || idx >= THING_TAB_SIZE) continue;
        /* Wanderer bit = 0x2. Skip if already a live wanderer in this slot. */
        if (!(thing_name_flags[idx] & 0x2)) {
            thing_name_flags[idx] |= ACTOR_FLAG_VISIBLE;
            check_actor_loaded(thing_names[idx].field_0);
            return thing_tab[idx];
        }
    }
    return NULL;
}

/* game_check_fade  E1: ? | E2: 0x4576A4 */
void check_fade(void) {
    int32_t now = my_time();
    static int fade_log = 0;

    /* fade_to_black/white: keep flag SET even after animation completes so a
     * following FADE_IN can trigger fade_in. Flag is cleared only when
     * FADE_IN completes (below). Rendering interpolates toward target each
     * frame; when past fade_time, hold at fully-black/white. */
    if (fade_to_black) {
        int32_t elapsed = now - fade_start;
        int factor;
        if (fade_time > 0 && elapsed < fade_time)
            factor = (int)((elapsed * 64) / fade_time);
        else
            factor = 64;  /* fully black */
        for (int i = 0; i < 256; i++) {
            view_cmap[i].R = (uint8_t)((fade_cmap[i].R * (64 - factor)) >> 6);
            view_cmap[i].G = (uint8_t)((fade_cmap[i].G * (64 - factor)) >> 6);
            view_cmap[i].B = (uint8_t)((fade_cmap[i].B * (64 - factor)) >> 6);
        }
        set_palette_flag = 1;
    }

    if (fade_to_white) {
        int32_t elapsed = now - fade_start;
        int factor;
        if (fade_time > 0 && elapsed < fade_time)
            factor = (int)((elapsed * 64) / fade_time);
        else
            factor = 63;  /* fully white */
        for (int i = 0; i < 256; i++) {
            view_cmap[i].R = (uint8_t)(fade_cmap[i].R + ((63 - fade_cmap[i].R) * factor) / 64);
            view_cmap[i].G = (uint8_t)(fade_cmap[i].G + ((63 - fade_cmap[i].G) * factor) / 64);
            view_cmap[i].B = (uint8_t)(fade_cmap[i].B + ((63 - fade_cmap[i].B) * factor) / 64);
        }
        set_palette_flag = 1;
    }

    if (fade_in) {
        int32_t elapsed = now - fade_start;
        if (fade_log < 5) {
            DBG_LOG(2, "[FADE] fade_in: now=%d start=%d elapsed=%d time=%d\n",
                (int)now, (int)fade_start, (int)elapsed, (int)fade_time);
            fade_log++;
        }
        if (fade_time > 0 && elapsed < fade_time) {
            int factor = (int)((elapsed * 64) / fade_time);
            for (int i = 0; i < 256; i++) {
                view_cmap[i].R = (uint8_t)((fade_cmap[i].R * factor) >> 6);
                view_cmap[i].G = (uint8_t)((fade_cmap[i].G * factor) >> 6);
                view_cmap[i].B = (uint8_t)((fade_cmap[i].B * factor) >> 6);
            }
            set_palette_flag = 1;
        } else {
            memcpy(view_cmap, fade_cmap, sizeof(view_cmap));
            set_palette_flag = 1;
            fade_in = 0;
            fade_to_black = 0;
            fade_to_white = 0;
            if (fade_log < 10) {
                fade_log++;
            }
        }
    }
}

/* game_do_fade_to_black  E1: ? | E2: 0x457820 */
void do_fade_to_black(int fade_factor) {
    for (int i = 0; i < 256; i++) {
        view_cmap[i].R = (uint8_t)((fade_cmap[i].R * (64 - fade_factor)) >> 6);
        view_cmap[i].G = (uint8_t)((fade_cmap[i].G * (64 - fade_factor)) >> 6);
        view_cmap[i].B = (uint8_t)((fade_cmap[i].B * (64 - fade_factor)) >> 6);
    }
    set_palette(view_cmap);
}

/* game_do_fade_to_white  E1: ? | E2: 0x4578C0 */
void do_fade_to_white(int fade_factor) {
    for (int i = 0; i < 256; i++) {
        view_cmap[i].R = (uint8_t)(fade_cmap[i].R + ((63 - fade_cmap[i].R) * fade_factor) / 64);
        view_cmap[i].G = (uint8_t)(fade_cmap[i].G + ((63 - fade_cmap[i].G) * fade_factor) / 64);
        view_cmap[i].B = (uint8_t)(fade_cmap[i].B + ((63 - fade_cmap[i].B) * fade_factor) / 64);
    }
    set_palette(view_cmap);
}

/* game_do_fade_in  E1: ? | E2P: 0x426A10 */
void do_fade_in(void) {
    fade_in = 1;
    fade_start = my_time();
    fade_time = 1000;
}

/* game_get_camera_position  E1: ? | E2P: 0x426AF0 */
void get_camera_position(void) {
    if (active_camera) {
        copy_vector(&view_pos, &active_camera->view_pos);
        copy_vector(&view_rot, &active_camera->view_rot);
        zoom_factor = active_camera->zoom_factor << 12;
        calculate_view_matrices();
    }
}

/* game_chase_camera  E1: ? | E2P: 0x426B60 */
void chase_camera(void) {
    if (!(actor_flags[0] & ACTOR_FLAG_ACTIVE)) return;

    vector_t target;
    target.X = actor_position[0].X;
    target.Y = actor_position[0].Y - 200;  /* Above head */
    target.Z = actor_position[0].Z - 500;  /* Behind */

    view_pos.X += (target.X - view_pos.X) / 4;
    view_pos.Y += (target.Y - view_pos.Y) / 4;
    view_pos.Z += (target.Z - view_pos.Z) / 4;

    calculate_view_matrices();
}

/* game_check_hit  E1: ? | E2P: 0x42CD68 */
int check_hit(actor_t *attacker, actor_t *target) {
    if (!attacker || !target) return 0;

    int dx = attacker->position_vector.X - target->position_vector.X;
    int dy = attacker->position_vector.Y - target->position_vector.Y;
    int dz = attacker->position_vector.Z - target->position_vector.Z;

    int32_t dist_sq = (int32_t)dx * dx + (int32_t)dy * dy + (int32_t)dz * dz;
    return (dist_sq < 40000);  /* Hit radius ~200 units */
}

/* game_inflict_damage  E1: ? | E2P: 0x42CE38 */
void inflict_damage(int actor_index, int damage) {
    if (actor_index < 0 || actor_index >= THING_TAB_SIZE) return;

    actor_hit_points[actor_index] -= (int16_t)damage;
    if (actor_hit_points[actor_index] <= 0) {
        actor_hit_points[actor_index] = 0;
        if (thing_tab[actor_index])
            make_dead(thing_tab[actor_index]);
    }
}

/* game_force_timed  E1: 0x449BF4 | E2: 0x457B7C */
void force_timed(int actor_index, int taction_index, int ticks) {
    if (actor_index < 0 || actor_index >= THING_TAB_SIZE) return;
    actor_t *actor = thing_tab[actor_index];
    if (!actor) return;
    if (taction_index >= 2000) return;

    if (taction_index == 5)
        poison_time = 7 * ticks;

    int32_t taction_time = game_time + 7 * ticks;
    taction_t *new_ta = find_free_t_action();
    if (!new_ta) return;
    new_ta->taction_index = (int16_t)taction_index;
    new_ta->taction_time = taction_time;
    new_ta->next = NULL;

    /* Insert sorted by time into the actor's tactions list */
    taction_t *list = actor->tactions_list;
    if (!list) {
        actor->tactions_list = new_ta;
        return;
    }

    if (taction_time - list->taction_time <= 0) {
        new_ta->next = list;
        actor->tactions_list = new_ta;
        return;
    }

    taction_t *prev = list;
    while (prev->next) {
        if (taction_time - prev->next->taction_time <= 0) {
            new_ta->next = prev->next;
            prev->next = new_ta;
            return;
        }
        prev = prev->next;
    }
    prev->next = new_ta;
}

/* game_do_timed  E1: ? | E2P: 0x42D038 */
void do_timed(actor_t *actor) {
    if (!actor) return;

    while (actor->tactions_list) {
        taction_t *ta = actor->tactions_list;
        if (ta->taction_time > game_time)
            break;

        actor->tactions_list = ta->next;

        int16_t idx = ta->taction_index;
        if (idx >= 0 && idx < ACTION_TAB_SIZE) {
            action_t *action = action_tab[idx];
            if (action)
                force_action(actor, action, 0);
        }

        free_t_action(ta);
    }
}

/* game_find_distance  E1: ? | E2P: 0x42D0A8 */
int32_t find_distance(vector_t *a, vector_t *b) {
    int32_t dx = a->X - b->X;
    int32_t dy = a->Y - b->Y;
    int32_t dz = a->Z - b->Z;
    return dx * dx + dy * dy + dz * dz;
}

/* game_find_xz_distance  E1: ? | E2P: 0x42D118 */
int32_t find_xz_distance(vector_t *a, vector_t *b) {
    int32_t dx = a->X - b->X;
    int32_t dz = a->Z - b->Z;
    return dx * dx + dz * dz;
}

/* game_find_relative_rot_vector_42D188
 * Extract Euler angles (Y, X, Z order) from a rotation matrix into `v`, with
 * a two-of-three-quadrant flip fixup at the end. */
void find_relative_rot_vector(vector_t *v, const matrix3x3_t *m) {
    matrix3x3_t tmp = *m;

    int16_t y_angle = arctan(tmp._13, tmp._33);
    v->Y = y_angle;
    if (y_angle != 0) pre_rotate_about_y(&tmp, (int16_t)-y_angle);

    int16_t x_angle = arctan((int16_t)-tmp._23, tmp._33);
    v->X = x_angle;
    if (x_angle != 0) pre_rotate_about_x(&tmp, (int16_t)-x_angle);

    int16_t z_angle = arctan(tmp._21, tmp._11);
    v->Z = z_angle;

    int counter = 0;
    if (((int32_t)v->X + 0x4000) & 0x8000) counter++;
    if (((int32_t)v->Y + 0x4000) & 0x8000) counter++;
    if (((int32_t)v->Z + 0x4000) & 0x8000) counter++;
    if (counter >= 2) {
        v->X = (int16_t)(0x8000 - v->X);
        v->Y = (int16_t)(v->Y + 0x8000);
        v->Z = (int16_t)(v->Z + 0x8000);
    }
}

static bool scene_load_tried[SCENE_TAB_SIZE];
static bool action_load_tried[ACTION_TAB_SIZE];
static bool rep_load_tried[REPERTOIRE_TAB_SIZE];
static bool actor_load_tried[THING_TAB_SIZE];
static bool actor_load_tried2[THING_TAB_SIZE];

/* Called wherever the *_tab arrays are emptied. load_game empties them without
 * going through initialise_game, and a latch left set then keeps an asset that
 * was loaded earlier in the session (the intro scene an actor's last act points
 * at, say) from ever loading again. */
void reset_load_tried(void) {
    memset(scene_load_tried, 0, sizeof(scene_load_tried));
    memset(action_load_tried, 0, sizeof(action_load_tried));
    memset(rep_load_tried, 0, sizeof(rep_load_tried));
    memset(actor_load_tried, 0, sizeof(actor_load_tried));
    memset(actor_load_tried2, 0, sizeof(actor_load_tried2));
}

/* The "already attempted" latch stops a failed load from being retried every
 * frame. Deleting an action, scene or repertoire clears its latch, so one
 * evicted from a full pool can load again when it is next needed. */
void set_action_load_tried(int16_t index, bool tried) {
    if (index >= 0 && index < ACTION_TAB_SIZE)
        action_load_tried[index] = tried;
}

/* menu_initialise_game_43A1FC — full game state reset */
void initialise_game(void) {
    new_game();
    free_all_heaps();

    root_thing = NULL;
    stuck_thing_list = NULL;
    root_scene = NULL;
    selected_thing = NULL;
    game_timer_start = 0;
    game_timer = 0;

    /* Clear scene name flags bits 1,2,3 and scene_tab */
    for (int i = 0; i < SCENE_TAB_SIZE; i++) {
        scene_name_flags[i] &= (int16_t)0xFFF1;
        scene_tab[i] = NULL;
    }
    reset_load_tried();

    for (int i = 0; i < THING_TAB_SIZE; i++) {
        actor_hit_points[i] = 0;
        thing_name_flags[i] &= (int16_t)0xFFF9;  /* clear bits 1,2 */
        actor_magic[i] = 0;
        actor_rep_name[i] = -2;
        actor_position[i].X = 0;
        actor_position[i].Y = 0;
        actor_position[i].Z = 0;
        actor_orientation[i].X = 0;
        actor_orientation[i].Y = 0;
        actor_orientation[i].Z = 0;
        actor_last_act[i] = 0;
        actor_held_by_part[i] = -1;
        actor_held_by_actor[i] = -1;
    }

    for (actor_t *t = thing_list; t; t = t->next_thing1)
        initialise_actor(t);

    memset(cameras_viewed, 0, sizeof(cameras_viewed));
}

/* menu_start_game_medium  E1: 0x430184 | E2: 0x43A31C */
static bool is_startup_code(const code_t *c) {
    if (c->index_code < 0) return false;
    const char *name = file_find_code_name(c->index_code);
    return name && strlen(name) == 11 && strcmp(name + 4, "StartUp") == 0;
}

/* Saves before v9 come from builds that ran only the first xxx:StartUp, so
 * the scenes the others InitScene (the stable horse, the sewer guard, ...)
 * were never put in the world. Replay those blocks, skipping any scene the
 * save already has started. Only InitScene lines live in them; the first
 * block, which also sets MapAreaHeight, already ran and is left alone. */
void replay_skipped_startups(void) {
    if (game_version != GAME_VERSION_E2) return;
    bool first = true;
    startup_repair = true;
    for (code_t *c = code_list; c; c = c->next_code) {
        if (!is_startup_code(c)) continue;
        if (!first) execute_code(c, NULL);
        first = false;
    }
    startup_repair = false;
}

void start_game_medium(int notUsed1, int notUsed2) {

    stop_the_clock = true;
    break_do_movement = 0;

    if (selected_thing)
        thing_name_flags[selected_thing->name_index] |= 0x0002;

    initialise_game();

    if (game_version == GAME_VERSION_E1) {
        int actor_idx = female ? 1 : 0;
        if (load_by_offset) {
            int32_t offset = actor_offset[actor_idx];
            fseek(file_pointer, offset, SEEK_SET);
            merge_sought_file(file_pointer, false);
        } else {
            search_actor_dirs_and_load(thing_names[actor_idx].field_0);
        }
        present_delay(0);

        check_scene_loaded(notUsed1);
        present_delay(0);
        if (notUsed1 >= 0 && notUsed1 < SCENE_TAB_SIZE && scene_tab[notUsed1]) {
            active_camera = NULL;
            check_actors_in_scene_loaded(scene_tab[notUsed1]);
            present_delay(0);
            start_scene(scene_tab[notUsed1]);
        } else {
            DBG_LOG(1, "[GAME] scene_tab[%d] is NULL — start_scene skipped!\n", notUsed1);
        }
        if (selected_thing)
            selected_thing->actor_behavior = 1;
    }

    no_wanderers = true;
    intro_flag = true;

    if (game_version == GAME_VERSION_E2) {
        /* 0x43A3A3: every author's xxx:StartUp runs (mar, ken, dav, nea, gre),
         * not just the first. ken:StartUp holds the InitScene list that puts
         * the stable horse, the sewer guard and the like into the world. */
        for (code_t *c = code_list; c; c = c->next_code)
            if (is_startup_code(c))
                execute_code(c, NULL);

        active_camera = NULL;

        static const char *start_names[] = {
            "StartGame", "StartF1", "StartF2", "StartF3", "StartF4",
            "StartF5", "StartF6", "StartF7", "StartF8", "StartF9",
            "StartF10", "StartF11", "StartF12"
        };
        const char *start_code_name = NULL;
        if (notUsed2 >= 0 && notUsed2 <= 12)
            start_code_name = start_names[notUsed2];

        if (start_code_name) {
            bool found = false;
            for (code_t *c = code_list; c; c = c->next_code) {
                if (c->index_code < 0) continue;
                char *name = file_find_code_name(c->index_code);
                if (!name || !*name) continue;
                int len = (int)strlen(name);
                if (len > 5) {
                    const char *suffix = name + len - (int)strlen(start_code_name);
                    if (suffix >= name && strcmp(suffix, start_code_name) == 0) {
                        execute_code(c, NULL);
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                DBG_LOG(1, "[GAME] Can't find start code '%s'\n", start_code_name);
                do_info2_req("Can't find start code", start_code_name);
            }
        }

        set_palette(all_black_cmap);

        if (mode_svga && !chosen_svga)
            go_vga();
        else if (!mode_svga && chosen_svga)
            go_svga();

        set_palette(all_black_cmap);
    }

    memset(cameras_viewed, 0, sizeof(cameras_viewed));
    armour_factor = 100;
    if (game_version == GAME_VERSION_E2) {
        kill_count = 0;
        treasure_count = 0;
    }
    hero_material = 0;
}

/* game_put_a_graphic_4555B0 — load (if needed) and display a named graphic overlay */
void put_a_graphic(const char *name, int pos_x, int pos_y, int intro_graphic) {
    int idx;

    for (idx = 0; idx < GRAPHICS_MAX; idx++) {
        if (strcmp(name, graphic_name_arr[idx].field_0) == 0)
            break;
    }

    if (idx == GRAPHICS_MAX) {
        char source[14];
        snprintf(source, sizeof(source), "%s.RAW", name);

        int size_x = 0, size_y = 0;
        char *pixels = load_raw_graphic(source, &size_x, &size_y);
        if (!pixels) {
            char str[64];
            snprintf(str, sizeof(str), "Can't load graphic '%.20s'", name);
            do_info_req(str);
            return;
        }

        /* Remap palette indices 0 and 1 to transparent (-1) */
        for (int i = 0; i < size_x * size_y; i++) {
            if (pixels[i] == 0 || pixels[i] == 1)
                pixels[i] = (char)-1;
        }

        for (idx = 0; idx < GRAPHICS_MAX; idx++) {
            if (!graphic_name_arr[idx].field_0[0])
                break;
        }
        if (idx >= GRAPHICS_MAX) {
            do_info_req("Too many graphics at once!");
            return;
        }

        strncpy(graphic_name_arr[idx].field_0, name, 8);
        graphic_name_arr[idx].field_0[8] = '\0';
        graphic_data[idx] = pixels;
        graphic_size_x[idx] = (int16_t)size_x;
        graphic_size_y[idx] = (int16_t)size_y;
        graphic_flag[idx] = 0;  /* None */
    }

    /* Re-find by name (handles both cached and freshly-loaded) */
    for (idx = 0; idx < GRAPHICS_MAX; idx++) {
        if (strcmp(name, graphic_name_arr[idx].field_0) == 0)
            break;
    }
    if (idx == GRAPHICS_MAX) {
        char str[64];
        snprintf(str, sizeof(str), "Graphic '%.20s' not loaded!", name);
        do_info_req(str);
        return;
    }

    /* Widescreen: intro art stays on the centred original view; HUD pieces
     * keep to the edge (or the centre) they were placed against. */
    int margin = wide_margin();
    int x, y;
    if (intro_graphic) {
        x = pos_x * (screen_width - 2 * margin) / 320 - graphic_size_x[idx] / 2 + margin;
        y = pos_y * screen_height / 200 - graphic_size_y[idx] / 2;
    } else {
        int centre = pos_x + graphic_size_x[idx] / 2;
        x = pos_x + (centre < 640 / 3 ? 0 : centre > 2 * 640 / 3 ? 2 * margin : margin);
        y = pos_y;
    }
    graphic_x[idx] = (int16_t)x;
    graphic_y[idx] = (int16_t)y;

    if (graphic_flag[idx] != 1) {  /* not already Drawn */
        graphic_flag[idx] = 2;     /* NeedToDraw */
        need_draw_graphics = 1;
    }
}

/* game_clear_a_graphic_455940 — a drawn graphic (flag 1) becomes NeedToClear
 * (3) so clear_graphics restores the background under it next frame. */
int16_t need_clear_graphics;
void clear_a_graphic(const char *name) {
    for (int idx = 0; idx < GRAPHICS_MAX; idx++) {
        if (strcmp(name, graphic_name_arr[idx].field_0) == 0) {
            if (graphic_data[idx]) {
                free(graphic_data[idx]);
                graphic_data[idx] = NULL;
            }
            if (graphic_flag[idx] == 1) {
                graphic_flag[idx] = 3;   /* NeedToClear */
                need_clear_graphics = 1;
            } else {
                graphic_name_arr[idx].field_0[0] = '\0';
            }
            return;
        }
    }
}

/* game_draw_magic_bar_456F94 — 6-icon hero magic gauge.
 * Each icon has 3 states: magic1_series=full, magic2_series=~half,
 * magic3_series=~quarter. Magic (0..24) → full = magic/4 fully-drawn icons,
 * remainder 0..3 chooses partial sprite at position `full`. SVGA sprites
 * "magic1..magic1f, magic2..magic2f, magic3..magic3f" at (223..385, y=10)
 * stride 27; VGA "lmagic..." at (111..181, y=7) stride 14. */
void draw_magic_bar(void) {
    static const char *magic_a_svga[6] = {"magic1","magic1b","magic1c","magic1d","magic1e","magic1f"};
    static const char *magic_b_svga[6] = {"magic2","magic2b","magic2c","magic2d","magic2e","magic2f"};
    static const char *magic_c_svga[6] = {"magic3","magic3b","magic3c","magic3d","magic3e","magic3f"};
    static const char *magic_a_vga[6]  = {"lmagic1","lmagic1b","lmagic1c","lmagic1d","lmagic1e","lmagic1f"};
    static const char *magic_b_vga[6]  = {"lmagic2","lmagic2b","lmagic2c","lmagic2d","lmagic2e","lmagic2f"};
    static const char *magic_c_vga[6]  = {"lmagic3","lmagic3b","lmagic3c","lmagic3d","lmagic3e","lmagic3f"};

    /* Magic bar is E2-only; magic1..magic3 sprites not in E1 assets. */
    if (game_version == GAME_VERSION_E1) return;
    if (!thing_tab[0] || no_icons || intro_flag) return;

    int magic = thing_tab[0]->actor_magic;
    bool svga = (screen_width > 320);
    const char **A = svga ? magic_a_svga : magic_a_vga;
    const char **B = svga ? magic_b_svga : magic_b_vga;
    const char **C = svga ? magic_c_svga : magic_c_vga;

    if (magic <= 0) {
        for (int i = 0; i < 6; i++) {
            clear_a_graphic(A[i]);
            clear_a_graphic(B[i]);
            clear_a_graphic(C[i]);
        }
        return;
    }

    if (magic > 24) magic = 24;
    int full = magic / 4;
    int partial = magic - full * 4;

    int x0     = svga ? 0xDF : 0x6F;
    int y      = svga ? 0x0A : 0x07;
    int stride = svga ? 0x1B : 0x0E;

    for (int i = 0; i < full; i++)
        put_a_graphic(A[i], x0 + i * stride, y, 0);
    for (int i = full; i < 6; i++)
        clear_a_graphic(A[i]);
    for (int i = 0; i < full; i++) {
        clear_a_graphic(B[i]);
        clear_a_graphic(C[i]);
    }

    if (partial == 1) {
        if (full < 6)
            put_a_graphic(C[full], x0 + full * stride, y, 0);
        for (int i = full; i < 6; i++) clear_a_graphic(B[i]);
        for (int i = full + 1; i < 6; i++) clear_a_graphic(C[i]);
    } else if (partial == 2 || partial == 3) {
        if (full < 6)
            put_a_graphic(B[full], x0 + full * stride, y, 0);
        for (int i = full + 1; i < 6; i++) clear_a_graphic(B[i]);
        for (int i = full; i < 6; i++) clear_a_graphic(C[i]);
    } else {
        for (int i = full; i < 6; i++) clear_a_graphic(B[i]);
        for (int i = full; i < 6; i++) clear_a_graphic(C[i]);
    }
}

/* game_draw_weapon_magic_457410 — right-arm held weapon's magic gauge.
 * Bar is width = 5 * magic / 2 clamped to [1, 50] (halved in VGA).
 * SVGA: (566..616, y=22..30). VGA: (287..312, y=11..15). */
void draw_weapon_magic(void) {
    if (!thing_tab[0] || no_icons) return;
    part_tab_t *pt = thing_tab[0]->_PartTab;
    if (!pt) return;
    part_t *arm = pt->field_0[8];
    if (!arm) return;
    actor_t *weapon = arm->actor_2_held;
    if (!weapon) return;

    int magic = weapon->actor_magic;
    int width = 5 * magic / 2;
    if (width == 0) return;
    if (width > 50) width = 50;

    if (screen_width > 320) {
        for (int plane = 0; plane < 3; plane++) {
            draw_mode[plane] = 2;
            for (int y = 22; y < 31; y++) {
                move_pen(plane, 566, (int16_t)y);
                if (width >= 0) {
                    a_pen_colour = 11;
                    draw(plane, 566 + width, (int16_t)y);
                }
                a_pen_colour = 13;
                draw(plane, 616, (int16_t)y);
            }
        }
    } else {
        int w = width / 2;
        for (int plane = 0; plane < 3; plane++) {
            draw_mode[plane] = 2;
            for (int y = 11; y < 16; y++) {
                move_pen(plane, 287, (int16_t)y);
                if (w >= 0) {
                    a_pen_colour = 11;
                    draw(plane, 287 + w, (int16_t)y);
                }
                a_pen_colour = 13;
                draw(plane, 312, (int16_t)y);
            }
        }
    }
}

/* game_clear_graphics_455294 — ClearGraphics.
 * For each slot with flag == NeedToClear (3): blit bg-source plane 3 →
 * plane 2 to restore original bg, clear_background(1-db,...) to restore
 * on active frame, clip_mask twice, append region to clear_tab[1-db].
 * Then wipe the name so slot's reusable. */
void clear_graphics(void) {
    for (int i = 0; i < GRAPHICS_MAX; ++i) {
        if (graphic_name_arr[i].field_0[0] && graphic_flag[i] == 3) {
            int x = graphic_x[i];
            int y = graphic_y[i];
            int w = graphic_size_x[i] + 1;
            int h = graphic_size_y[i] + 1;
            clip_blit(3, x, y, 2, x, y, w, h, 0);
            clear_background(1 - db, x, y, w, h);
            clip_mask(2, 0, x, y, graphic_size_x[i], graphic_size_y[i]);
            clip_mask(2, 1, x, y, graphic_size_x[i], graphic_size_y[i]);
            if (number_to_clear[1 - db] >= 150) {
                beep_message("ClearTab overflow!");
            } else {
                subarea_t *a = &clear_tab[1 - db][number_to_clear[1 - db]++];
                a->left = x;
                a->top = y;
                a->right = x + w;
                a->bottom = y + h;
            }
            graphic_name_arr[i].field_0[0] = '\0';
        }
    }
    need_clear_graphics = 0;
}

/* game_draw_graphics_45516C — DrawGraphics.
 * Iterate GRAPHICS_MAX slots. For each slot with flag == NeedToDraw (2)
 * and non-empty name and non-null data: put_graphic to plane 1-db AND
 * plane 2 (background store so background-restore preserves it), set
 * flag = Drawn (1), append dirty rect to clear_tab[1-db]. Special cases
 * for "lifeN"/"llifeN" (calls draw_life_bar) and "magibar1"/"lmbar"
 * (calls draw_weapon_magic) are E2 HUD overlays. */
void draw_graphics(void) {
    for (int i = 0; i < GRAPHICS_MAX; ++i) {
        if (graphic_name_arr[i].field_0[0] &&
                graphic_flag[i] == 2 &&
                graphic_data[i]) {
            put_graphic(graphic_data[i], 1 - db,
                graphic_x[i], graphic_y[i],
                graphic_size_x[i], graphic_size_y[i]);
            put_graphic(graphic_data[i], 2,
                graphic_x[i], graphic_y[i],
                graphic_size_x[i], graphic_size_y[i]);
            graphic_flag[i] = 1;

            size_t nl = strlen(graphic_name_arr[i].field_0);
            if ((nl == 5 && strncasecmp(graphic_name_arr[i].field_0, "life", 4) == 0) ||
                    (nl == 6 && strncasecmp(graphic_name_arr[i].field_0, "llife", 5) == 0))
                draw_life_bar();
            if (strcasecmp(graphic_name_arr[i].field_0, "magibar1") == 0 ||
                    strcasecmp(graphic_name_arr[i].field_0, "lmbar") == 0)
                draw_weapon_magic();

            if (number_to_clear[1 - db] >= 150) {
                beep_message("ClearTab overflow!");
            } else {
                subarea_t *a = &clear_tab[1 - db][number_to_clear[1 - db]++];
                a->left   = graphic_x[i];
                a->top    = graphic_y[i];
                a->right  = graphic_x[i] + graphic_size_x[i] + 1;
                a->bottom = graphic_y[i] + graphic_size_y[i] + 1;
            }
        }
    }
    need_draw_graphics = 0;
}

/* game_load_a_graphic_4558C0 — pre-load a named graphic without displaying it */
void load_a_graphic(const char *name) {
    put_a_graphic(name, 0, 0, 0);
    for (int idx = 0; idx < GRAPHICS_MAX; idx++) {
        if (strcmp(name, graphic_name_arr[idx].field_0) == 0) {
            graphic_flag[idx] = 0;
            break;
        }
    }
}

void invalidate_drawn_graphics(void) {
    for (int idx = 0; idx < GRAPHICS_MAX; idx++) {
        if (graphic_flag[idx] == 1) {
            graphic_flag[idx] = 2;
            need_draw_graphics = 1;
        }
    }
}

/* game_remove_all_graphics_455A10 — remove all graphic overlays */
void remove_all_graphics(void) {
    for (int idx = 0; idx < GRAPHICS_MAX; idx++) {
        graphic_name_arr[idx].field_0[0] = '\0';
        if (graphic_data[idx]) {
            free(graphic_data[idx]);
            graphic_data[idx] = NULL;
        }
        graphic_flag[idx] = 0;
    }
}

/* game_update_game_icons  E1: 0x449BF0 | E2: 0x455848 */
void update_game_icons(void) {
    if (game_version == GAME_VERSION_E1) return;
    if (intro_flag) return;
    actor_t *hero = thing_tab[0];
    if (!hero) return;

    static const char *gem_svga[5] = {"bar1","bar2","bar3","bar4","bar5"};
    static const char *gem_vga[5]  = {"lbar1","lbar2","lbar3","lbar4","lbar5"};

    int16_t full_hp = hero->full_actor_hp;
    if (mode_svga) {
        if (full_hp == 100) {
            put_a_graphic("life1", 0x19, 8, 0);
            clear_a_graphic("life2");
            clear_a_graphic("life3");
        } else if (full_hp == 200) {
            put_a_graphic("life2", 0x19, 8, 0);
            clear_a_graphic("life1");
            clear_a_graphic("life3");
        } else {
            put_a_graphic("life3", 0x19, 8, 0);
            clear_a_graphic("life2");
            clear_a_graphic("life1");
        }
    } else {
        if (full_hp == 100) {
            put_a_graphic("llife1", 0x0C, 4, 0);
            clear_a_graphic("llife2");
            clear_a_graphic("llife3");
        } else if (full_hp == 200) {
            put_a_graphic("llife2", 0x0C, 4, 0);
            clear_a_graphic("llife1");
            clear_a_graphic("llife3");
        } else {
            put_a_graphic("llife3", 0x0C, 4, 0);
            clear_a_graphic("llife2");
            clear_a_graphic("llife1");
        }
    }

    int16_t armor = hero->actor_strength_factor;
    if (armor > 200) {
        if (mode_svga) {
            put_a_graphic("armour3", 0x18B, 0x0F, 0);
            clear_a_graphic("armour2");
        } else {
            put_a_graphic("larmour3", 0xC6, 7, 0);
            clear_a_graphic("larmour2");
        }
    } else if (armor > 100) {
        if (mode_svga) {
            put_a_graphic("armour2", 0x18B, 0x0F, 0);
            clear_a_graphic("armour3");
        } else {
            put_a_graphic("larmour2", 0xC6, 7, 0);
            clear_a_graphic("larmour3");
        }
    } else {
        if (mode_svga) {
            clear_a_graphic("armour2");
            clear_a_graphic("armour3");
        } else {
            clear_a_graphic("larmour2");
            clear_a_graphic("larmour3");
        }
    }

    part_tab_t *pt = hero->_PartTab;
    int rh_slot = (game_version == GAME_VERSION_E1) ? 1 : 8;
    part_t *right_hand = (pt && pt->field_0[rh_slot]) ? pt->field_0[rh_slot] : NULL;
    if (!right_hand) return;

    actor_t *held = right_hand->actor_2_held;
    if (!held) {
        /* No item held — show empty hand icon, clear weapon/gem/magic icons */
        if (mode_svga) {
            clear_a_graphic("rodicon1");
            clear_a_graphic("swdicon1");
            for (int i = 0; i < 5; i++)
                clear_a_graphic(gem_svga[i]);
            put_a_graphic("hndicon1", 0x213, 7, 0);
            clear_a_graphic("magibar1");
        } else {
            clear_a_graphic("lrod1");
            clear_a_graphic("lsword1");
            for (int i = 0; i < 5; i++)
                clear_a_graphic(gem_vga[i]);
            put_a_graphic("lhand2", 0x10D, 1, 0);
            clear_a_graphic("lmbar");
        }
        return;
    }

    /* Held item: rod (flags & 0x10) or sword */
    if (held->flags & 0x10) {
        if (mode_svga) {
            clear_a_graphic("hndicon1");
            clear_a_graphic("swdicon1");
            put_a_graphic("rodicon1", 0x213, 7, 0);
        } else {
            clear_a_graphic("lhand2");
            clear_a_graphic("lsword1");
            put_a_graphic("lrod1", 0x10D, 1, 0);
        }
    } else {
        if (mode_svga) {
            clear_a_graphic("hndicon1");
            clear_a_graphic("rodicon1");
            put_a_graphic("swdicon1", 0x213, 7, 0);
        } else {
            clear_a_graphic("lhand2");
            clear_a_graphic("lrod1");
            put_a_graphic("lsword1", 0x10D, 1, 0);
        }
    }

    int16_t power = held->actor_magic ? held->actor_magic_factor : held->actor_strength_factor;
    if (power > 100) {
        int gem_idx = power / 100 - 1;
        if (gem_idx > 4) gem_idx = 4;
        if (mode_svga)
            put_a_graphic(gem_svga[gem_idx], 0x1D4, 0x14, 0);
        else
            put_a_graphic(gem_vga[gem_idx], 0xEE, 0x0A, 0);
        for (int i = 0; i < 5; i++) {
            if (i == gem_idx) continue;
            clear_a_graphic(mode_svga ? gem_svga[i] : gem_vga[i]);
        }
    } else {
        for (int i = 0; i < 5; i++)
            clear_a_graphic(mode_svga ? gem_svga[i] : gem_vga[i]);
    }

    if (held->actor_magic) {
        if (mode_svga)
            put_a_graphic("magibar1", 0x235, 0x15, 0);
        else
            put_a_graphic("lmbar", 0x11E, 0x0A, 0);
    } else {
        if (mode_svga)
            clear_a_graphic("magibar1");
        else
            clear_a_graphic("lmbar");
    }
}

/* game_clear_game_icons  E2: 0x455D10 */
void clear_game_icons(void) {
    if (mode_svga) {
        clear_a_graphic("life1");
        clear_a_graphic("life2");
        clear_a_graphic("life3");
        clear_a_graphic("armour2");
        clear_a_graphic("armour3");
        clear_a_graphic("hndicon1");
        clear_a_graphic("swdicon1");
        clear_a_graphic("rodicon1");
        clear_a_graphic("magic2");
        clear_a_graphic("magic3");
    } else {
        clear_a_graphic("llife1");
        clear_a_graphic("llife2");
        clear_a_graphic("llife3");
        clear_a_graphic("larmour2");
        clear_a_graphic("larmour3");
        clear_a_graphic("lhndicon1");
        clear_a_graphic("lswdicon1");
        clear_a_graphic("lrodicon1");
        clear_a_graphic("lmagic2");
        clear_a_graphic("lmagic3");
    }
}

/* game_put_a_temp_graphic  E2: 0x4556DC */
void put_a_temp_graphic(const char *name, int x, int y) {
    char source[64];
    snprintf(source, sizeof(source), "%s.RAW", name);

    int sx = 0, sy = 0;
    char *pixels = load_raw_graphic(source, &sx, &sy);
    if (!pixels) return;

    for (int row = 0; row < sy; row++)
        for (int col = 0; col < sx; col++)
            if (pixels[row * sx + col] == 1)
                pixels[row * sx + col] = (char)0xFF;

    put_graphic(pixels, db, x, y, sx, sy);
    free(pixels);
}

/* game_draw_icon_page_graphics  E2: 0x456054 */
static void draw_icon_page_graphics(void) {
    struct { const char *name; int x; int y; } items[] = {
        {"ringicon", 0x7B, 0xE0},
        {"amuicon1", 0x79, 0x133},
        {"lampicon", 0xBB, 0xE2},
        {"shdicon1", 0xC4, 0x133},
        {"shdicon2", 0x110, 0xE1},
        {"scrlicon", 0x10E, 0x133},
        {"amuicon2", 0x154, 0xE0},
        {"kingicon", 0x15A, 0x132},
        {"elder1",   0x7D, 0x7A},
        {"elder2",   0xA0, 0x8E},
        {"elder3",   0xCB, 0x7A},
        {"elder4",   0xCB, 0x46},
        {"elder5",   0x9F, 0x3E},
        {"elder6",   0x7D, 0x45},
        {"elder7",   0xA7, 0x66},
    };
    for (int i = 0; i < (int)(sizeof(items)/sizeof(items[0])); i++) {
        int16_t idx = find_scene_name_index(items[i].name);
        if (idx >= 0 && (scene_name_flags[idx] & 8))
            put_a_temp_graphic(items[i].name, items[i].x + wide_margin(), items[i].y);
    }
}

/* game_show_icon_page  E2: 0x455E74 */
void show_icon_page(void) {
    if (intro_flag) return;

    int was_svga = mode_svga;
    if (!mode_svga)
        go_svga();

    selected_camera = 0;
    load_palette(NULL);

    FILE *f = fopen_ci("graphics/iconpage.raw", "rb");
    if (!f) return;

    char header[32];
    uint8_t raw_pal[768];
    fread_ignore(header, 1, 32, f);
    fread_ignore(raw_pal, 1, 768, f);

    palette_entry_t icon_palette[256];
    for (int i = 0; i < 256; i++) {
        icon_palette[i].R = raw_pal[i * 3 + 0] >> 2;
        icon_palette[i].G = raw_pal[i * 3 + 1] >> 2;
        icon_palette[i].B = raw_pal[i * 3 + 2] >> 2;
    }

    read_screen_image(f, bitmap[3]);
    fclose(f);

    set_palette(all_black_cmap);
    clip_blit(3, 0, 0, 0, 0, 0, screen_width, screen_height, 0xC0);
    clip_blit(3, 0, 0, 1, 0, 0, screen_width, screen_height, 0xC0);

    /* Force HUD visible on inventory page regardless of no_icons */
    int16_t saved_no_icons = no_icons;
    no_icons = 0;

    remove_all_graphics();
    update_game_icons();
    if (need_draw_graphics) draw_graphics();
    draw_magic_bar();
    draw_life_bar();
    draw_icon_page_graphics();
    clip_blit(1 - db, 0, 0, db, 0, 0, screen_width, screen_height, 0xC0);

    no_icons = saved_no_icons;

    set_palette(icon_palette);

    for (;;) {
        get_mouse();
        if (key_esc_was_pressed) {
            key_esc_was_pressed = false;
            break;
        }
        if (key_return_was_pressed) {
            key_return_was_pressed = false;
            break;
        }
        flip_win95();
        platform_delay(16);
    }

    set_palette(all_black_cmap);

    if (!was_svga) {
        go_vga();
        remove_all_graphics();
        update_game_icons();
        draw_magic_bar();
    } else {
        remove_all_graphics();
        if (!no_icons)
            update_game_icons();
    }

    a_pen_colour = 0;
    rect_fill(0, 0, 0, screen_width, screen_height);
    a_pen_colour = 0;
    rect_fill(1, 0, 0, screen_width, screen_height);
    a_pen_colour = 0;
    rect_fill(3, 0, 0, screen_width, screen_height);

    active_camera = NULL;
    clear_keys_pressed();
}

/* game_adjust_magic  E1: ? | E2: 0x4567B0 */
void adjust_magic(actor_t *actor, int amount) {
    if (!actor) return;
    actor->actor_magic += (int16_t)amount;
    if (actor->actor_magic < 0) actor->actor_magic = 0;
    if (actor->actor_magic > 100) actor->actor_magic = 100;
}

/* game_beep_message  E1: ? | E2P: 0x42D988 */
void beep_message(const char *msg) {
    if (!msg) return;

    int len = (int)strlen(msg);
    move_pen(db, (int16_t)(screen_width / 2 - len * tx_w / 2), (int16_t)(screen_height - 20));
    a_pen_colour = 1;  /* White */
    text(db, msg, len);
}

    /* A resolution change invalidates every pending dirty rect: clear_tab and
     * sub_area_to_clear still hold coordinates from the old screen size, and
     * clear_masking/clear_parts would mask and restore at those stale rects.
     * prepare_parts only ever zeroes number_to_clear[db], so the other side
     * would keep them indefinitely. */
static void reset_dirty_rects(void) {
    number_to_clear[0] = 0;
    number_to_clear[1] = 0;
    for (int i = 0; i < 20; i++)
        sub_area_to_clear[i].left = sub_area_to_clear[i].right =
        sub_area_to_clear[i].top = sub_area_to_clear[i].bottom = 0;
}

/* menu_go_vga  E2: 0x43AC6C */
void go_vga(void) {
    bool was_on = mouse_pointer_on;
    flush_backgrounds();
    turn_mouse_pointer_off();
    mode_svga = 0;
    reset_dirty_rects();
    active_camera = NULL;
    selected_camera = -1;
    set_vga_constants();
    init_gadgets();
    db = 0;
    set_palette(colour_map);
    if (was_on) turn_mouse_pointer_on();
    make_game_screen();
}

/* menu_go_svga  E2: 0x43ABE0 */
void go_svga(void) {
    /* E1 DOS ships only 320x200 backgrounds in VIEWS — there is no hi-res mode
     * to switch to, and letting the constants go to 640x480 desynchronises the
     * renderer from the platform blit size. */
    if (low_res_only) return;

    bool was_on = mouse_pointer_on;
    flush_backgrounds();
    turn_mouse_pointer_off();
    mode_svga = 1;
    reset_dirty_rects();
    active_camera = NULL;
    selected_camera = -1;
    set_svga_constants();
    init_gadgets();
    db = 0;
    set_palette(colour_map);
    if (was_on) turn_mouse_pointer_on();
    make_game_screen();
}

/* Switch the presentation set between the original 320x200 assets and the
 * enhanced 640x480 ones, mid-session.
 *
 * Only the asset search order and the render constants change — the loaded
 * database (FAN, OFFSETS, FILES) stays put, so actors, scenes, camera and
 * inventory carry across untouched. go_vga/go_svga clear active_camera, which
 * makes the next check_view reload the background from the new root. */
/* E1's database is the 320x200 one, so its 640x480 mode needs the HIRES set.
 * E2 runs at 640x480 without it: a camera with no HIRES view is loaded from
 * VIEWS and doubled by copy_vga_to_svga(). Either way the display has to
 * reach 640x480, which low_res_only rules out. */
int enhanced_graphics_available(void) {
    if (game_version == GAME_VERSION_E1)
        return hires_available;
    return !low_res_only;
}

static void apply_resolution(int enabled);

void set_enhanced_graphics(int enabled) {
    if (!enhanced_graphics_available()) return;
    if ((mode_svga != 0) == (enabled != 0)) return;
    apply_resolution(enabled);
}

/* The 640x480 mode is rebuilt at the new width, the same way a resolution
 * switch rebuilds it. */
void set_aspect_ratio(int index) {
    if (index == aspect_ratio) return;
    int before = aspect_ratio_in_use();
    aspect_ratio = index;
    if (mode_svga && aspect_ratio_in_use() != before) apply_resolution(1);
}

static void apply_resolution(int enabled) {
    /* go_vga/go_svga reach window_proc through make_game_screen, so a hotkey
     * held across the switch would otherwise recurse back into here. */
    static int switching = 0;
    if (switching) return;
    switching = 1;

    /* go_vga/go_svga reset selected_camera to -1; keep it so the view can be
     * reloaded for the same camera afterwards. */
    /* game_up_and_running goes true before the title screen is even shown, so
     * it cannot tell "in a scene" from "on a menu". An active camera can:
     * everything that needs rebuilding — icons, magic bar, actors, the
     * background — only exists once a view is loaded. */
    int16_t prev_camera = selected_camera;
    bool have_view = (prev_camera > 0);

    if (have_view)
        remove_all_graphics();

    /* Set before the mode switch: make_game_screen() loads art on the way. */
    enhanced_graphics = enabled ? 1 : 0;

    if (enabled)
        go_svga();
    else
        go_vga();

    chosen_svga = mode_svga;

    if (have_view) {
        remove_all_graphics();
        update_game_icons();
        draw_magic_bar();

        /* Reload the background for the camera we were already on. Must be
         * check_view and not check_camera: check_camera re-runs the map
         * lookup, and a lookup that misses yields camera 0, which triggers
         * play_dead_scene(7) — dropping the player into the dragon scene. */
        check_view(prev_camera);

        /* check_view sets background_status = 2, which prepare_parts consumes
         * one draw buffer per frame. The settings menu never calls
         * prepare_parts, so without running the sequence for both sides here
         * the buffer that is not current keeps the old-resolution image and
         * shows through on the next flip. */
        for (int side = 0; side < 2; side++) {
            prepare_parts();
            draw_stuck_parts();
            draw_parts();
            show_parts();
        }
    } else {
        /* Title / menu screens: nothing to rebuild but the backdrop, which is
         * resolution-specific (TITLE_S.RAW at 320, TSCREEN.RAW at 640). */
        load_background_title();
    }

    switching = 0;
}

/* game_init_gadgets  E1: ? | E2P: 0x42DB58 */
void init_gadgets(void) {
    /* The port has its own menu system instead of the original requesters. */
}

/* game_init_graphics  E1: 0x446F84 | E2: 0x452564 */
void init_graphics(void) {
    for (int i = 0; i < GRAPHICS_MAX; i++) {
        graphic_name_arr[i].field_0[0] = '\0';
    }
}

/* game_play_sound_win95  E1: ? | E2P: 0x42E2C8 */
void play_sound_win95(sound_t *sound, int volume) {
    if (!sound) return;
    if (!sound_is_on || !sound_fx_on) return;
    if (!sound->audio_ptr || sound->sound_length <= 0) return;
    int rate = sound->sample_rate > 0 ? sound->sample_rate : 22050;
    platform_audio_play_pcm(sound->audio_ptr, sound->sound_length,
                            rate, volume, 0, false);
}

/* game_release_sound_buffer_win95  E1: ? | E2P: 0x42E338 */
void release_sound_buffer_win95(sound_t *sound) {
    if (!sound) return;
    sound->audio_ptr = NULL;
    sound->sound_length = 0;
}

/* game_remove_sound_driver_win95  E1: ? | E2P: 0x42E3A8 */
void remove_sound_driver_win95(void) {
    for (int i = 0; i < SOUND_POOL_SIZE; i++) {
        if (!(sound_heap_arr[i].use_flag & 0x8000))
            release_sound_buffer_win95(&sound_heap_arr[i]);
    }
    sound_is_on = false;
}

/* game_load_palette_and_set_background  E1: ? | E2P: 0x42E5D8 */
void load_palette_and_set_background(const char *filename) {
    if (!filename) return;

    FILE *f = fopen_ci(filename, "rb");
    if (!f) return;

    char header[32];
    fread_ignore(header, 1, 32, f);

    unsigned char pal[768];
    fread_ignore(pal, 1, 768, f);
    for (int i = 0; i < 256; i++) {
        colour_map[i].R = pal[i * 3 + 0] >> 2;
        colour_map[i].G = pal[i * 3 + 1] >> 2;
        colour_map[i].B = pal[i * 3 + 2] >> 2;
    }

    read_screen_image(f, bitmap[2]);
    fclose(f);

    clip_blit(2, 0, 0, 0, 0, 0, screen_width, screen_height, 0xC0);
    clip_blit(2, 0, 0, 1, 0, 0, screen_width, screen_height, 0xC0);

    memcpy(view_cmap, colour_map, 256 * sizeof(palette_entry_t));
    /* Update fade target so subsequent CT_FADE_IN fades to this background's palette */
    memcpy(fade_cmap, colour_map, 256 * sizeof(palette_entry_t));
    set_palette_flag = 1;
}

/* game_add_to_display_list  E1: ? | E2P: 0x42E878 */
void add_to_display_list(actor_t *actor) {
    if (!actor) return;

    for (actor_t *curr = root_thing; curr; curr = curr->next_in_display_list) {
        if (curr == actor)
            return;
    }

    actor->actor_act.act_action = NULL;
    actor->actor_scene = NULL;
    actor->actor_reperture = NULL;
    actor->next_in_display_list = root_thing;
    actor->event_timer = 0;
    actor->flags |= 9;      /* 0x0001 | 0x0008 */
    actor->time_actor = game_time;
    root_thing = actor;
}

/* game_turn_actor  E1: ? | E2P: 0x42E9C8 */
UNUSED_ATTR static void game_turn_actor(actor_t *actor) {
    if (!actor) return;

    /* Rotate actor's Y axis towards its last facing direction */
    int16_t target = actor->last_actor_direction;
    if (target < 0) return;

    int16_t diff = target - actor->rotate_vector.Y;
    if (diff > 512)
        turn_actor(actor, 512);
    else if (diff < -512)
        turn_actor(actor, -512);
    else
        turn_actor(actor, diff);
}

/* game_remove_scene  E1: 0x446FF0 | E2: 0x4525D0 */
void remove_scene(scene_t *scene) {
    if (!scene) { quit("Can't remove NULL scene"); return; }

    /* Safety check: scene must not be in active (RootScene) list */
    for (scene_t *s = root_scene; s; s = s->next_scene) {
        if (s == scene) { quit("Can't remove Scene in active list"); return; }
    }
    if (scene->scene_index < 0) { quit("Can't remove Scene without name"); return; }
    if (scene != scene_tab[scene->scene_index]) { quit("Can't remove Scene not in SceneTab"); return; }
    if (game_time == scene->scene_time) { quit("Can't remove Scene with current Time"); return; }

    if (scene == scene_list)
        scene_list = scene_list->scene_next;
    else {
        for (scene_t *s = scene_list; s; s = s->scene_next) {
            if (scene == s->scene_next) { s->scene_next = scene->scene_next; break; }
        }
    }
    scene_tab[scene->scene_index] = NULL;
    do_delete_scene(scene);
}

/* game_remove_action  E1: 0x44750C | E2: 0x452B08 */
void remove_action(action_t *action) {
    if (!action) return;
    if (action->action_index < 0) { quit("Can't remove Action without name"); return; }
    if (action != action_tab[action->action_index]) { quit("Can't remove Action not in ActionTab"); return; }

    for (actor_t *a = root_thing; a; a = a->next_in_display_list) {
        if (action == a->actor_act.act_action) { quit("Can't remove Action in use by actor"); return; }
    }
    if (game_time == action->action_time) { quit("Can't remove Action with current Time"); return; }

    if (action == action_list)
        action_list = action_list->next;
    else {
        for (action_t *a = action_list; a; a = a->next) {
            if (action == a->next) { a->next = action->next; break; }
        }
    }
    action_tab[action->action_index] = NULL;
    do_delete_action(action);
}

/* game_remove_sound  E1: 0x44724C | E2: 0x45282C */
void remove_sound(sound_t *sound) {
    if (!sound) return;
    int16_t idx = sound->sound_name_index;
    if (idx < 0) { quit("Can't remove Sound without name"); return; }
    if (idx >= SOUND_TAB_SIZE) { quit("Can't remove Sound - name out of bounds"); return; }
    if (sound != sound_tab[idx]) return;

    if (sound == sound_list)
        sound_list = sound_list->next;
    else {
        for (sound_t *s = sound_list; s; s = s->next) {
            if (sound == s->next) { s->next = sound->next; break; }
        }
    }
    sound_tab[idx] = NULL;
    top_of_sound_data -= sound->sound_length;
    release_sound_buffer_win95(sound);
    free_sound(sound);
}

/* game_remove_rep  E1: 0x447424 | E2: 0x452A08 */
void remove_rep(rephead_t *rep) {
    if (!rep) return;
    if (rep->rep_index < 0) { quit("Can't remove Rep. without name"); return; }
    if (rep != repertoire_tab[rep->rep_index]) { quit("Can't remove Rep. not in repertoireTab"); return; }

    for (actor_t *a = root_thing; a; a = a->next_in_display_list) {
        if (rep == a->actor_reperture) { quit("Can't remove Rep. in use by actor"); return; }
        if (a->actor_reperture && a->actor_reperture->rep_index == rep->rep_index) {
            quit("Can't remove Rep. named in actor"); return;
        }
    }
    if (game_time == rep->rep_time) { quit("Can't remove Rep with current Time"); return; }

    if (rep == repertoire_list)
        repertoire_list = repertoire_list->next_rep;
    else {
        for (rephead_t *r = repertoire_list; r; r = r->next_rep) {
            if (rep == r->next_rep) { r->next_rep = rep->next_rep; break; }
        }
    }
    repertoire_tab[rep->rep_index] = NULL;
    rep->rep_use_flag = 0x8000u;
}

/* game_remove_actor  E1: 0x4470E4 | E2: 0x4526C4 */
void remove_actor(actor_t *actor) {
    if (!actor) return;
    char str[50];
    sprintf(str, "Actor '%d'", actor->name_index);

    for (actor_t *a = root_thing; a; a = a->next_in_display_list) {
        if (a == actor) { quit2("Can't remove Actor in active list", str); return; }
    }
    if (actor->name_index < 0) { quit2("Can't remove Actor without name", str); return; }
    if (actor == selected_thing) { quit2("Can't remove main Actor", str); return; }
    if (actor != thing_tab[actor->name_index]) { quit2("Can't remove Actor not in ThingTab", str); return; }
    if (game_time == actor->time_actor) { quit2("Can't remove Actor with current Time", str); return; }

    for (part_t *part = actor->actor_parts_list; part; part = part->next_in_display_list) {
        if (part->actor_2_held) {
            part->actor_2_held->part_heap_link = NULL;
            remove_actor(part->actor_2_held);
        }
    }
    if (actor->part_heap_link) {
        actor->part_heap_link->actor_2_held = NULL;
        remove_actor(actor->part_heap_link->parent_actor);
    }

    if (actor == thing_list)
        thing_list = thing_list->next_thing1;
    else {
        for (actor_t *a = thing_list; a; a = a->next_thing1) {
            if (actor == a->next_thing1) { a->next_thing1 = actor->next_thing1; break; }
        }
    }
    thing_tab[actor->name_index] = NULL;
    do_delete_thing(actor);
}

/* Allocation hints for the big pools. Allocators are first-fit, and scanning
 * from slot 0 every time made loading an action or actor quadratic in its
 * event, key and part count — the load spikes on a PSP. Every slot below a
 * hint is in use unless it was freed since, and the free_* functions pull the
 * hint back, so the search picks the same slot a scan from zero would. The
 * second pass is a guard for any slot freed behind the allocator's back. */
static int event_hint, key_hint, part_hint, point_hint;

#define POOL_SEARCH(arr, size, hint, IS_FREE, out) do { \
    (out) = -1; \
    for (int pass_ = 0; pass_ < 2 && (out) < 0; pass_++) \
        for (int i_ = pass_ ? 0 : (hint); i_ < (size); i_++) \
            if (IS_FREE(arr[i_])) { (out) = i_; break; } \
    if ((out) >= 0) (hint) = (out) + 1; \
} while (0)

/* Bounds-checked: the editor's delete_triangle hands free_event a tri_t. */
#define POOL_RELEASE(arr, hint, ptr) do { \
    if ((ptr) >= (arr) && (ptr) < (arr) + sizeof(arr) / sizeof((arr)[0])) { \
        int i_ = (int)((ptr) - (arr)); \
        if (i_ < (hint)) (hint) = i_; \
    } \
} while (0)

#define EVENT_IS_FREE(e) ((e).event_type & (int16_t)0x8000)
#define KEY_IS_FREE(k)   ((k).field_E & 0x80)
#define PART_IS_FREE(p)  ((p).type & 0x8000)
#define POINT_IS_FREE(p) ((p).point_use_flag & 1)

void reset_pool_hints(void) {
    event_hint = key_hint = part_hint = point_hint = 0;
}

void free_event(event_t *event) {
    if (!event) return;
    event->event_type = (int16_t)0x8000;
    POOL_RELEASE(event_heap_arr, event_hint, event);
}

void free_action(action_t *action) {
    if (action) action->action_flags = (int16_t)0x8000;
}

void free_script(script_t *script) {
    if (script) script->script_actor_index = -1;
}

void free_part(part_t *part) {
    if (!part) return;
    part->type = 0x8000u;
    POOL_RELEASE(part_heap_arr, part_hint, part);
}

void free_scene(scene_t *scene) {

    if (scene) scene->scene_use_flag = 0x8000u;
}

void free_rep(rephead_t *rep) {
    if (rep) rep->rep_use_flag = 0x8000;
}

void free_point(point_t *point) {
    if (!point) return;
    point->point_use_flag = 1;
    POOL_RELEASE(point_heap_arr, point_hint, point);
}

void free_key(key_state_t *key) {
    if (!key) return;
    key->field_E = 0x80u;
    POOL_RELEASE(key_heap_arr, key_hint, key);
}

void free_sound(sound_t *sound) {
    if (sound) sound->use_flag = 0x8000;
}

void free_t_action(taction_t *ta) {
    if (ta) ta->taction_index = -1;
}

/* Find-free functions — scan heap for unused slot, try to evict if full */
/* Like the action and key pools, a full event pool first evicts the oldest
 * unused action or scene, whose keys hold the events. Quitting at once ended
 * long sessions on the handhelds with "Event heap overflow". Quits only when
 * an eviction pass frees nothing. */
static int resident_actions_and_scenes(void) {
    int n = 0;
    for (action_t *a = action_list; a; a = a->next) n++;
    for (scene_t *s = scene_list; s; s = s->scene_next) n++;
    return n;
}

event_t *find_free_event(void) {
    for (;;) {
        event_t *event = look_for_free_event();
        if (event) return event;

        int before = resident_actions_and_scenes();
        try_to_remove_scene_or_action();
        if (resident_actions_and_scenes() == before) {
            quit("Event heap overflow");
            return NULL;
        }
        DBG_LOG(2, "[EVENT] pool full, evicted an action or scene\n");
    }
}

event_t *look_for_free_event(void) {
    int i;
    POOL_SEARCH(event_heap_arr, EVENT_POOL_SIZE, event_hint, EVENT_IS_FREE, i);
    if (i < 0) return NULL;
    memset(&event_heap_arr[i], 0, sizeof(event_t));
    return &event_heap_arr[i];
}

action_t *find_free_action(void) {
    action_t *free_slot = NULL;
    do {
        for (int i = 0; i < ACTION_POOL_SIZE; i++) {
            if (action_heap_arr[i].action_flags & (int16_t)0x8000) {
                memset(&action_heap_arr[i], 0, sizeof(action_t));
                free_slot = &action_heap_arr[i];
                break;
            }
        }
        if (!free_slot) { try_to_remove_scene_or_action(); }
    } while (!free_slot);
    return free_slot;
}

script_t *find_free_script(void) {
    /* Only script_actor_index == -1 marks a free slot: 0 is the hero. */
    for (int i = 0; i < SCRIPT_SIZE; i++) {
        if (script_arr[i].script_actor_index == -1) {
            memset(&script_arr[i], 0, sizeof(script_t));
            script_arr[i].script_actor_index = -1;
            return &script_arr[i];
        }
    }
    quit("Script heap overflow");
    return NULL;
}

actor_t *find_free_actor(void) {
    actor_t *free_slot = NULL;
    do {
        for (int i = 0; i < ACTOR_POOL_SIZE; i++) {
            if (actor_heap_arr[i].flags & 0x8000) {
                memset(&actor_heap_arr[i], 0, sizeof(actor_t));
                free_slot = &actor_heap_arr[i];
                free_slot->_PartTab = &part_tab_heap_arr[i];
                free_slot->_PointTab = &point_tab_heap_arr[i];
                free_slot->_TriangleTab = &triangle_tab_heap_arr[i];
                memset(free_slot->_PartTab, 0, sizeof(part_tab_t));
                memset(free_slot->_PointTab, 0, sizeof(point_tab_t));
                memset(free_slot->_TriangleTab, 0, sizeof(triangle_tab_t));
                break;
            }
        }
        if (!free_slot) { try_to_remove_actor(); }
    } while (!free_slot);
    return free_slot;
}

part_t *find_free_part(void) {
    part_t *free_slot = NULL;
    do {
        int i;
        POOL_SEARCH(part_heap_arr, PART_POOL_SIZE, part_hint, PART_IS_FREE, i);
        if (i >= 0) {
            memset(&part_heap_arr[i], 0, sizeof(part_t));
            free_slot = &part_heap_arr[i];
        }
        if (!free_slot) { try_to_remove_actor(); }
    } while (!free_slot);
    return free_slot;
}

scene_t *find_free_scene(void) {
    scene_t *free_slot = NULL;
    do {
        for (int i = 0; i < SCENE_POOL_SIZE; i++) {
            if (scene_heap_arr[i].scene_use_flag & 0x8000) {
                memset(&scene_heap_arr[i], 0, sizeof(scene_t));
                free_slot = &scene_heap_arr[i];
                break;
            }
        }
        if (!free_slot) { try_to_remove_scene(); }
    } while (!free_slot);
    return free_slot;
}

rephead_t *find_free_rep(void) {
    rephead_t *free_slot = NULL;
    do {
        for (int i = 0; i < REP_POOL_SIZE; i++) {
            if (rep_heap_arr[i].rep_use_flag & 0x8000) {
                memset(&rep_heap_arr[i], 0, sizeof(rephead_t));
                free_slot = &rep_heap_arr[i];
                break;
            }
        }
        if (!free_slot) { try_to_remove_rep(); }
    } while (!free_slot);
    return free_slot;
}

point_t *find_free_point(void) {
    point_t *free_slot = NULL;
    do {
        int i;
        POOL_SEARCH(point_heap_arr, POINT_POOL_SIZE, point_hint, POINT_IS_FREE, i);
        if (i >= 0) {
            memset(&point_heap_arr[i], 0, sizeof(point_t));
            free_slot = &point_heap_arr[i];
        }
        if (!free_slot) { try_to_remove_actor(); }
    } while (!free_slot);
    return free_slot;
}

tri_t *find_free_tri(void) {
    tri_t *free_slot = NULL;
    do {
        for (int i = 0; i < TRI_SIZE; i++) {
            if (tri_arr[i].tri_use_flag & 0x8000) {
                memset(&tri_arr[i], 0, sizeof(tri_t));
                free_slot = &tri_arr[i];
                break;
            }
        }
        if (!free_slot) { try_to_remove_actor(); }
    } while (!free_slot);
    return free_slot;
}

key_state_t *find_free_key(void) {
    key_state_t *free_slot = NULL;
    do {
        int i;
        POOL_SEARCH(key_heap_arr, KEY_POOL_SIZE, key_hint, KEY_IS_FREE, i);
        if (i >= 0) {
            memset(&key_heap_arr[i], 0, sizeof(key_state_t));
            free_slot = &key_heap_arr[i];
        }
        if (!free_slot) { try_to_remove_scene_or_action(); }
    } while (!free_slot);
    return free_slot;
}

sound_t *find_free_sound(void) {
    sound_t *free_slot = NULL;
    do {
        for (int i = 0; i < SOUND_POOL_SIZE; i++) {
            if (sound_heap_arr[i].use_flag & 0x8000) {
                memset(&sound_heap_arr[i], 0, sizeof(sound_t));
                free_slot = &sound_heap_arr[i];
                break;
            }
        }
        if (!free_slot) { try_to_remove_sound(); }
    } while (!free_slot);
    return free_slot;
}

texture_t *find_free_texture(void) {
    texture_t *free_slot = NULL;
    do {
        for (int i = 0; i < TEXTURE_POOL_SIZE; i++) {
            if (texture_heap_arr[i].use_flag & 0x8000) {
                memset(&texture_heap_arr[i], 0, sizeof(texture_t));
                free_slot = &texture_heap_arr[i];
                break;
            }
        }
        if (!free_slot) { quit("Texture heap overflow"); return NULL; }
    } while (!free_slot);
    return free_slot;
}

taction_t *find_free_t_action(void) {
    for (int i = 0; i < TACTION_POOL_SIZE; i++) {
        if (taction_heap_arr[i].taction_index < 0) {
            memset(&taction_heap_arr[i], 0, sizeof(taction_t));
            return &taction_heap_arr[i];
        }
    }
    quit("Timed Action heap overflow");
    return NULL;
}

/* menu_delete_key  E1: 0x4307E0 | E2: 0x43AAB8 */
void delete_key(key_state_t *key) {
    if (!key) return;

    for (event_t *ev = key->key_event_list; ev; ) {
        event_t *next = ev->next;
        free_event(ev);
        ev = next;
    }

    /* Free all ellipses (these are malloc'd, not heap-managed) */
    for (ellipse_t *el = key->ellipses_list; el; ) {
        ellipse_t *next = el->next;
        free(el);
        el = next;
    }

    free_key(key);
}

/* menu_do_delete_thing  E1: 0x4304A8 | E2: 0x43A780 */
void do_delete_thing(actor_t *actor) {
    if (!actor) return;

    for (actor_t *a = root_thing; a; a = a->next_in_display_list) {
        if (a == actor) { quit("do_delete_thing: actor still in display list"); return; }
    }

    if (actor == thing_list)
        thing_list = thing_list->next_thing1;
    else {
        for (actor_t *a = thing_list; a; a = a->next_thing1) {
            if (actor == a->next_thing1) { a->next_thing1 = actor->next_thing1; break; }
        }
    }

    for (part_t *part = actor->actor_parts_list; part; ) {
        part_t *next_part = part->next_in_display_list;
        for (point_t *pt = part->points_list; pt; ) {
            point_t *next_pt = pt->next;
            free_point(pt);
            pt = next_pt;
        }
        free_part(part);
        part = next_part;
    }

    for (tri_t *tri = actor->polygone_tri_list; tri; ) {
        tri_t *next_tri = tri->next;
        free_event((event_t *)tri);
        tri = next_tri;
    }

    if (actor->name_index >= 0 && actor->name_index < THING_TAB_SIZE)
        thing_tab[actor->name_index] = NULL;

    /* Free the actor itself (actors stored in event heap) */
    free_event((event_t *)actor);
}

/* menu_do_delete_action  E1: 0x4306BC | E2: 0x43A994 */
void do_delete_action(action_t *action) {
    if (!action) return;

    if (action == action_list)
        action_list = action_list->next;
    else {
        for (action_t *a = action_list; a; a = a->next) {
            if (action == a->next) { a->next = action->next; break; }
        }
    }

    for (key_state_t *key = action->key_list; key; ) {
        key_state_t *next = key->next;
        delete_key(key);
        key = next;
    }

    action_tab[action->action_index] = NULL;
    set_action_load_tried(action->action_index, false);
    free_action(action);
}

/* menu_do_delete_scene  E1: 0x430740 | E2: 0x43AA18 */
void do_delete_scene(scene_t *scene) {
    if (!scene) return;

    if (scene == scene_list)
        scene_list = scene_list->scene_next;
    else {
        for (scene_t *s = scene_list; s; s = s->scene_next) {
            if (scene == s->scene_next) { s->scene_next = scene->scene_next; break; }
        }
    }

    for (script_t *scr = scene->scene_script_list; scr; ) {
        script_t *next = scr->next_script;
        for (key_state_t *key = scr->script_action.key_list; key; ) {
            key_state_t *next_key = key->next;
            delete_key(key);
            key = next_key;
        }
        free_script(scr);
        scr = next;
    }

    if (scene->scene_index >= 0 && scene->scene_index < SCENE_TAB_SIZE)
        scene_load_tried[scene->scene_index] = false;
    free_scene(scene);
}

/* file_do_delete_rep  E1: 0x43A4F4 | E2: 0x4447C0 */
void do_delete_rep(rephead_t *rep) {
    if (!rep) return;
    if (rep->rep_index < 0) { quit("do_delete_rep: rep has no name"); return; }
    if (rep != repertoire_tab[rep->rep_index]) { quit("do_delete_rep: rep not in tab"); return; }

    for (actor_t *a = root_thing; a; a = a->next_in_display_list) {
        if (a->actor_reperture == rep) a->actor_reperture = NULL;
    }

    if (rep == repertoire_list)
        repertoire_list = repertoire_list->next_rep;
    else {
        for (rephead_t *r = repertoire_list; r; r = r->next_rep) {
            if (rep == r->next_rep) { r->next_rep = rep->next_rep; break; }
        }
    }

    repertoire_tab[rep->rep_index] = NULL;
    if (rep->rep_index < REPERTOIRE_TAB_SIZE)
        rep_load_tried[rep->rep_index] = false;
    free_rep(rep);
}

/* game_check_action_loaded_no_msg  E1: 0x4461CC | E2: 0x4517A4 */
int check_action_loaded_no_msg(int16_t index) {
    actor_t *save = selected_thing;
    int result = 0;
    if (index >= 0 && index < ACTION_TAB_SIZE) {
        if (!action_tab[index] && !action_load_tried[index]) {
            action_load_tried[index] = true;
            stop_the_clock = true;
            if (load_by_offset) {
                if (action_offset[index] >= 0) {
                    fseek(file_pointer, action_offset[index], SEEK_SET);
                    merge_sought_file(file_pointer, 1);
                    result = 0;
                } else {
                    result = 1;
                }
            } else {
                search_action_dirs_and_load(action_names[index].field_0);
                result = 0;
            }
        }
    }
    selected_thing = save;
    return result;
}

/* game_check_action_loaded  E1: 0x446140 | E2: 0x451718 */
int check_action_loaded(int16_t index) {
    int result = check_action_loaded_no_msg(index);
    if (result == 1) {
        do_info2_req("Can't load action", "");
    } else if (result == 2) {
        do_info2_req("Action name not in directory", "");
    }
    return result;
}

/* game_check_rep_loaded  E1: 0x446258 | E2: 0x451830 */
int check_rep_loaded(int16_t index) {
    actor_t *save = selected_thing;
    if (index >= 0 && index < REPERTOIRE_TAB_SIZE) {
        if (!repertoire_tab[index] && !rep_load_tried[index]) {
            rep_load_tried[index] = true;
            stop_the_clock = true;
            if (load_by_offset) {
                if (repertoire_offset[index] >= 0) {
                    fseek(file_pointer, repertoire_offset[index], SEEK_SET);
                    merge_sought_file(file_pointer, 1);
                } else {
                    do_info2_req("Can't find rep", "");
                }
            } else {
                search_rep_dirs_and_load(repertoire_names[index].field_0);
            }
        }
    }
    selected_thing = save;
    return 0;
}

/* game_check_actor_loaded_by_index — like reference CheckActorLoaded(int) */
void check_actor_loaded_by_index(int16_t actor_index) {
    if (actor_index < 0 || actor_index >= THING_TAB_SIZE) return;

    actor_t *save = selected_thing;
    actor_t *actor = thing_tab[actor_index];
    if (!actor && !actor_load_tried[actor_index]) {
        actor_load_tried[actor_index] = true;
        if (load_by_offset) {
            if (actor_offset[actor_index] >= 0) {
                fseek(file_pointer, actor_offset[actor_index], SEEK_SET);
                merge_sought_file(file_pointer, 1);
            } else {
                do_info2_req("Can't find actor", thing_names[actor_index].field_0);
            }
        } else {
            /* 0x451F82: load from the directory when load_by_offset is 0. */
            search_actor_dirs_and_load(thing_names[actor_index].field_0);
        }
        actor = thing_tab[actor_index];
        if (actor) {
            initialise_actor(actor);
            if (thing_name_flags[actor_index] & 2) {
                copy_vector(&actor->position_vector, &actor_position[actor_index]);
                copy_vector(&actor->rotate_vector, &actor_orientation[actor_index]);
                actor->actor_hitpoints = actor_hit_points[actor_index];
                actor->actor_magic = actor_magic[actor_index];
                if (thing_name_flags[actor_index] & 4)
                    actor->actor_behavior = BH_DEAD;
            }
        } else {
            do_info2_req("Actor not found", thing_names[actor_index].field_0);
        }
    }
    actor = thing_tab[actor_index];
    if (actor) {
        if (actor_rep_name[actor_index] != -2)
            actor->actor_rep_index = actor_rep_name[actor_index];
        add_to_display_list(actor);
        if (!(thing_name_flags[actor_index] & 2)) {
            thing_name_flags[actor_index] |= 2;
            actor->full_actor_hp = 100;
            actor->actor_hitpoints = 100;
            if (actor->actor_init_code >= 0) {
                code_t *code = code_tab[actor->actor_init_code];
                if (code)
                    do_execute_code(code, actor);
            }
        }
    }
    selected_thing = save;
}

/* game_check_actor_loaded  E1: 0x44681C | E2: 0x451DF4 */
int check_actor_loaded(const char *name) {
    int16_t actor_index = find_thing_name_index(name);
    if (actor_index < 0 || actor_index >= THING_TAB_SIZE) return 0;

    actor_t *save = selected_thing;
    actor_t *actor = thing_tab[actor_index];
    if (!actor && !actor_load_tried2[actor_index]) {
        actor_load_tried2[actor_index] = true;
        if (load_by_offset) {
            if (actor_offset[actor_index] >= 0) {
                fseek(file_pointer, actor_offset[actor_index], SEEK_SET);
                merge_sought_file(file_pointer, 1);
            } else {
                do_info2_req("Can't find actor", name);
            }
        } else {
            search_actor_dirs_and_load(name);
        }
        actor = thing_tab[actor_index];
        if (actor) {
            initialise_actor(actor);
            actor->flags |= 8;
            if (thing_name_flags[actor_index] & 2) {
                copy_vector(&actor->position_vector, &actor_position[actor_index]);
                copy_vector(&actor->rotate_vector, &actor_orientation[actor_index]);
                actor->actor_hitpoints = actor_hit_points[actor_index];
                actor->actor_magic = actor_magic[actor_index];
                if (thing_name_flags[actor_index] & 4)
                    actor->actor_behavior = BH_DEAD;
            }
        } else {
            do_info2_req("Actor not found", name);
        }
    }
    actor = thing_tab[actor_index];
    if (actor) {
        if (actor_rep_name[actor_index] != -2)
            actor->actor_rep_index = actor_rep_name[actor_index];
        add_to_display_list(actor);
        if (!(thing_name_flags[actor_index] & 2)) {
            thing_name_flags[actor_index] |= 2;
            actor->full_actor_hp = 100;
            actor->actor_hitpoints = 100;
            if (actor->actor_init_code >= 0) {
                code_t *code = code_tab[actor->actor_init_code];
                if (code)
                    do_execute_code(code, actor);
            }
        }
    }
    selected_thing = save;
    return 0;
}

/* game_check_scene_loaded  E1: 0x446BD0 | E2: 0x4521D4 */
void check_scene_loaded(int16_t scene_index) {
    if (scene_index < 0 || scene_index >= SCENE_TAB_SIZE) return;
    actor_t *save = selected_thing;
    if (!scene_tab[scene_index] && !scene_load_tried[scene_index]) {
        scene_load_tried[scene_index] = true;
        stop_the_clock = true;
        if (load_by_offset) {
            if (scene_offset[scene_index] >= 0) {
                fseek(file_pointer, scene_offset[scene_index], SEEK_SET);
                merge_sought_file(file_pointer, 1);
            } else {
                do_info2_req("Can't find scene",
                             file_find_scene_name(scene_index));
            }
        } else {
            char buf[9];
            const char *sname = file_find_scene_name(scene_index);
            if (sname) {
                strncpy(buf, sname, 8);
                buf[8] = '\0';
                search_scene_dirs_and_load(buf);
            }
        }
    }
    selected_thing = save;
}

/* game_check_sound_loaded  E1: 0x446314 | E2: 0x4518EC */
void check_sound_loaded(int16_t sound_index) {
    if (sound_index < 0 || sound_index >= SOUND_TAB_SIZE) return;
    if (sound_tab[sound_index]) return;

    actor_t *save = selected_thing;
    stop_the_clock = true;

    if (load_by_offset) {
        int32_t offset = sound_offset[sound_index];
        if (offset >= 0 && file_pointer) {
            fseek(file_pointer, offset, SEEK_SET);
            merge_sought_file(file_pointer, 1);
        } else if (offset < 0 && offset != -1 && file2_pointer) {
            fseek(file2_pointer, offset & 0x7FFFFFFF, SEEK_SET);
            merge_sought_file(file2_pointer, 1);
        }
    } else if (remaster_layout) {
        file_load_remaster_sound(sound_index);
    } else if (sound_names && sound_names[sound_index].field_0[0]) {
        char path[128];
        snprintf(path, sizeof(path), "sounds/%s.fan", sound_names[sound_index].field_0);
        merge_a_file(path, 1);
    }

    selected_thing = save;
}

/* game_execute_thing_code  E1: 0x443D30 | E2: 0x44F1A8 */
void execute_thing_code(actor_t *actor, int16_t code_index) {
    if (!actor) return;
    if (code_index < 0 || code_index >= CODE_TAB_SIZE) return;
    code_t *code = code_tab[code_index];
    if (!code) return;
    execute_with_sub_obj(code, actor, actor, NULL);
}

part_t *get_execute_part(void) { return g_execute_part; }

/* game_execute_part_code_425640
 * Runs a code stream with a part in context so InteractType events and
 * CT_SPAWN_LIVE resolve their source part. */
void execute_part_code(part_t *part, int16_t code_index) {
    if (!part || !part->parent_actor) return;
    if (code_index < 0 || code_index >= CODE_TAB_SIZE) return;
    code_t *code = code_tab[code_index];
    if (!code) return;
    part_t *prev = g_execute_part;
    g_execute_part = part;
    execute_with_sub_obj(code, part->parent_actor, part->parent_actor, NULL);
    g_execute_part = prev;
}

/* game_force_action  E1: 0x44612C | E2: 0x451704 */
void force_action(actor_t *actor, action_t *action, int set_some_flag) {
    if (!actor) return;
    actor->force_action_to_execute = action;
    if (set_some_flag)
        actor->state_flags |= 0x10;   /* ActorUpdateFlags::Unknown_0010 */
}

/* game_spawn_actor_452D9C
 * Called by CT_SPAWN_LIVE (live=1, copy_rot=0) and InteractType::SpawnActor
 * (live=0, copy_rot=0) / SpawnActor2 (live=1, copy_rot=1). Spawns actor_index with
 * action_index positioned at `part` in the world.
 *
 * NB: part->holding_actor field at offset 0x44 is typed actor_t*
 * in our struct but the asm walks it as a part_t* chain pointer
 * (same offset in both structs). We cast through it. */
void spawn_actor(part_t *part, int actor_index, int action_index, int live, int copy_rot) {
    if (!part) return;
    actor_t *owner_actor = part->parent_actor;
    if (!owner_actor) return;

    /* Chain 1: walk owner's held_by_part chain, set next_in_path back-pointers. */
    part_t *held_by_part = owner_actor->part_heap_link;
    if (held_by_part) {
        part_t *cursor = held_by_part;
        if (cursor->holding_actor) {
            do {
                part_t *nxt = (part_t *)cursor->holding_actor;
                nxt->next_in_path = cursor;
                cursor = nxt;
            } while (cursor->holding_actor);
        }
        held_by_part->next_in_path = NULL;
        if (held_by_part->parent_actor)
            find_positions_on_path(held_by_part->parent_actor);
    }

    /* Chain 2: walk from `part` via holding_actor, set next_in_path back-pointers. */
    for (part_t *i = part; i->holding_actor; i = (part_t *)i->holding_actor) {
        ((part_t *)i->holding_actor)->next_in_path = i;
    }
    part->next_in_path = NULL;
    find_positions_on_path(part->parent_actor);

    if (actor_index < 0 || actor_index >= THING_TAB_SIZE) return;
    if (action_index < 0 || action_index >= ACTION_TAB_SIZE) return;

    restore_actor(actor_index);
    check_actor_loaded_by_index((int16_t)actor_index);
    check_action_loaded((int16_t)action_index);

    actor_t *new_actor = thing_tab[actor_index];
    if (!new_actor) return;
    action_t *action = action_tab[action_index];
    if (!action) return;

    if (!live) {
        actor_t *parent = part->parent_actor;
        if (parent->part_heap_link)
            parent = parent->part_heap_link->parent_actor;
        new_actor->spawner_index = parent->name_index;
    }
    new_actor->flags |= 8;   /* Visible */
    if (live)
        new_actor->state_flags |= 0x08;   /* ActorUpdateFlags::Unknown_0008 */

    copy_vector(&new_actor->position_vector, &part->ellipse_center);
    if (copy_rot) {
        find_relative_rot_vector(&new_actor->rotate_vector, &part->matrix_1);
    } else {
        copy_vector(&new_actor->rotate_vector, &part->parent_actor->rotate_vector);
    }

    add_to_display_list(new_actor);
    new_actor->actor_act.act_action = NULL;
    force_action(new_actor, action, live ? 0 : 1);
}

/* game_draw_life_bar  E1: ? | E2: 0x4568C8 */
void draw_life_bar(void) {
    /* Life bar HUD is E2-only; graphics not in E1 assets. */
    if (game_version == GAME_VERSION_E1) return;
    if (!thing_tab[0] || no_icons) return;

    int hit_points_poison = 100;
    if (poison_time) {
        taction_t *taction = thing_tab[0]->tactions_list;
        while (taction) {
            if (taction->taction_index == 5 || poison_time) {
                hit_points_poison = 100 * (taction->taction_time - game_time) / poison_time;
                if (hit_points_poison < -1) hit_points_poison = -1;
                break;
            }
            taction = taction->next;
        }
        if (!taction) poison_time = 0;
    }

    int hp_mult = thing_tab[0]->full_actor_hp / 100;
    if (hp_mult < 1) hp_mult = 1;
    int hit_points;
    if (thing_tab[0]->actor_hitpoints < 0)
        hit_points = -1;
    else
        hit_points = thing_tab[0]->actor_hitpoints / hp_mult;

    int dim1 = 0, dim2 = 0;
    if (hp_mult == 2)      { dim1 = 1; dim2 = 0; }
    else if (hp_mult == 3) { dim1 = 2; dim2 = 2; }

    int y_start = 24 - dim2;
    int y_end   = y_start + 2 * dim1 + 3;

    if (hit_points_poison < hit_points) {
        for (int plane = 0; plane < 3; plane++) {
            draw_mode[plane] = 2;
            for (int y = y_start; y < y_end; ++y) {
                move_pen(plane, 69, (int16_t)y);
                if (hit_points_poison >= 0) {
                    a_pen_colour = 11;
                    draw(plane, (int16_t)(hit_points_poison + 69), (int16_t)y);
                }
                if (hit_points >= 0) {
                    a_pen_colour = 12;
                    draw(plane, (int16_t)(hit_points + 69), (int16_t)y);
                }
                a_pen_colour = 15;
                draw(plane, 169, (int16_t)y);
            }
        }
    } else {
        for (int plane = 0; plane < 3; plane++) {
            draw_mode[plane] = 2;
            for (int y = y_start; y < y_end; ++y) {
                move_pen(plane, 69, (int16_t)y);
                if (hit_points >= 0) {
                    a_pen_colour = 11;
                    draw(plane, (int16_t)(hit_points + 69), (int16_t)y);
                }
                a_pen_colour = 13;
                draw(plane, (int16_t)(hit_points_poison + 69), (int16_t)y);
                if (hit_points_poison < 100) {
                    a_pen_colour = 15;
                    draw(plane, 169, (int16_t)y);
                }
            }
        }
    }
}

/* game_check_actors_in_scene_loaded  E1: 0x4469E8 | E2: 0x451FD8 */
void check_actors_in_scene_loaded(scene_t *scene) {
    actor_t *save = selected_thing;
    if (!scene) return;

    for (script_t *script = scene->scene_script_list; script; script = script->next_script) {
        if (script->script_action.action_flags & 0x20)
            continue;
        int16_t actor_index = script->script_actor_index;
        if (actor_index < 0 || actor_index >= THING_TAB_SIZE) continue;

        actor_t *actor = thing_tab[actor_index];
        if (!actor) {
            if (load_by_offset) {
                if (actor_offset[actor_index] >= 0) {
                    fseek(file_pointer, actor_offset[actor_index], SEEK_SET);
                    merge_sought_file(file_pointer, 1);
                } else {
                    do_info2_req("Can't find actor",
                                 thing_names[actor_index].field_0);
                }
            } else {
                search_actor_dirs_and_load(thing_names[actor_index].field_0);
            }
            actor = thing_tab[actor_index];
            if (actor) {
                initialise_actor(actor);
                if (thing_name_flags[actor_index] & 2) {
                    copy_vector(&actor->position_vector, &actor_position[actor_index]);
                    copy_vector(&actor->rotate_vector, &actor_orientation[actor_index]);
                    if (actor_rep_name[actor_index] != -2)
                        actor->actor_rep_index = actor_rep_name[actor_index];
                    actor->actor_hitpoints = actor_hit_points[actor_index];
                    actor->actor_magic = actor_magic[actor_index];
                    if (thing_name_flags[actor_index] & 4)
                        actor->actor_behavior = BH_DEAD;
                }
            } else {
                do_info2_req("Actor not found",
                             thing_names[actor_index].field_0);
            }
        }

        actor = thing_tab[actor_index];
        if (actor) {
            add_to_display_list(actor);
            if (!(thing_name_flags[actor_index] & 2)) {
                thing_name_flags[actor_index] |= 2;
                actor->full_actor_hp = 100;
                actor->actor_hitpoints = 100;
                if (actor->actor_init_code >= 0) {
                    code_t *code = code_tab[actor->actor_init_code];
                    if (code)
                        do_execute_code(code, actor);
                }
            }
        }
    }
    selected_thing = save;
}

/* game_check_scene_ok_to_start  E2: 0x446C84
 * Returns 1 if scene can start, 0 if any script actor has
 * thing_name_flags bit 2 (loaded) WITHOUT bit 4 set.
 * Prevents starting scenes whose actors are in a dead/invalid state. */
int check_scene_ok_to_start(scene_t *scene) {
    if (!scene) return 1;
    int blocked = 0;
    for (script_t *s = scene->scene_script_list; s; s = s->next_script) {
        if (s->script_action.action_flags & 0x20) continue;
        int16_t ai = s->script_actor_index;
        if (ai < 0 || ai >= THING_TAB_SIZE) continue;
        if (thing_name_flags[ai] & 4) {
            blocked = 1;
            break;
        }
    }
    return blocked ? 0 : 1;
}

/* game_start_scene  E1: 0x446CC8 | E2: 0x452290 */
void start_scene(scene_t *scene) {
    if (!scene) return;

    scene_name_flags[scene->scene_index] |= 2;
    scene_name_flags[scene->scene_index] &= ~4;

    scene_t *cur;
    for (cur = root_scene; cur && cur != scene; cur = cur->next_scene) {}
    if (!cur) {
        scene->next_scene = root_scene;
        root_scene = scene;
        scene->scene_use_flag |= 1;
    }

    /* Pass 1: Assign actors to scene */
    for (script_t *script = scene->scene_script_list; script; script = script->next_script) {
        DBG_LOG(1, "[SS]   scene=%d P1 script actor=%d '%s' aflags=0x%x in_tab=%d %s\n",
                scene->scene_index,
                script->script_actor_index,
                (script->script_actor_index >= 0 && script->script_actor_index < THING_TAB_SIZE && thing_names) ? thing_names[script->script_actor_index].field_0 : "?",
                script->script_action.action_flags,
                (script->script_actor_index >= 0 && script->script_actor_index < THING_TAB_SIZE) ? (thing_tab[script->script_actor_index] != NULL) : -1,
                (script->script_action.action_flags & 0x20) ? "SKIP" : "run");
        if (!(script->script_action.action_flags & 0x20) && script->script_actor_index >= 0) {
            actor_t *actor = thing_tab[script->script_actor_index];
            if (actor) {
                actor->actor_scene = scene;
                actor->actor_act.act_action = &script->script_action;
            }
        }
    }

    /* Pass 2: Check view for selected_thing */
    for (script_t *script = scene->scene_script_list; script; script = script->next_script) {
        if (!(script->script_action.action_flags & 0x20) && script->script_actor_index >= 0) {
            actor_t *actor = thing_tab[script->script_actor_index];
            if (actor && actor == selected_thing)
                check_view(scene->camera_index);
        }
    }

    /* Pass 3: Full actor setup */
    for (script_t *script = scene->scene_script_list; script; script = script->next_script) {
        if (script->script_action.action_flags & 0x20) continue;
        int16_t actor_index = script->script_actor_index;
        if (actor_index < 0) continue;
        if (actor_index >= THING_TAB_SIZE) continue;

        actor_t *actor = thing_tab[actor_index];
        if (!actor) { DBG_LOG(1, "[SS]   actor %d NOT FOUND in thing_tab\n", actor_index); continue; }

        update_thing(actor);
        actor->flags |= 8;
        add_to_display_list(actor);

        for (act_t *act = actor->actor_act_list; act; act = act->next)
            act->flags = 0x400;
        free_spent_acts(actor);

        /* Initialize from first key if at position 0 */
        key_state_t *key = script->script_action.key_list;
        if (key && !key->KEY_position) {
            int16_t save_rep = actor->actor_rep_index;
            copy_defaults_to_actual_not_flags(actor);
            actor->actor_rep_index = save_rep;
            actor->anchored_part = actor->actor_parts_list;
            set_vector(&actor->rotate_vector, 0, 0, 0);
            make_identity(&actor->matrix33_2);
        }

        actor->actor_act.act_action = &script->script_action;
        actor->actor_scene = scene;
        initialise_act(&actor->actor_act);

        if (key && key->key_event_list)
            actor_last_act[actor->name_index] = (scene->scene_index + 1) | 0x8000;

        update_act(&actor->actor_act, actor, 0);
    }
}

/* game_check_hero_rep_4575C4 — select hero repertoire index based on
 * (a) whether hero is holding a magic item in slot 8 with bit 0x10 set,
 * (b) whether hero is high enough (Y >= 0x28 << height_shift) when material==7 (water),
 * (c) whether hero->actor_magic is nonzero.
 * After picking rep_idx, conditionally force-load action 3 or 4 based on
 * scene_name_flags[7] bit 8 (ducking/crouch flag). */
void check_hero_rep(void) {
    /* Rep-index magic-state scheme is E2 only. Repertoire tables differ in E1;
     * writing 9..0x10 corrupts hero->actor_reperture → hero disappears. */
    if (game_version == GAME_VERSION_E1) return;
    actor_t *hero = selected_thing;
    if (!hero) return;
    if (hero->name_index != 0) return;

    part_t *held_slot = NULL;
    if (hero->_PartTab)
        held_slot = hero->_PartTab->field_0[8];
    actor_t *held_actor = held_slot ? held_slot->actor_2_held : NULL;

    int16_t rep_idx;
    int32_t threshold = (int32_t)0x28 << height_shift;
    /* asm reads dword[edx+84h] = position.X|Y as 32-bit, sar 16 → Y sign-extended.
     * vector_t layout X@0,Y@2,Z@4: high 16 of [+84h] dword = Y. */
    bool high = (hero_material == 7) && ((int32_t)hero->position_vector.Y >= threshold);

    bool held_magic_active = held_actor && (held_actor->flags & 0x10) && (held_actor->actor_magic > 0);

    if (held_magic_active) {
        if (high)                       rep_idx = 0xA;
        else if (hero->actor_magic)     rep_idx = 0x10;
        else                            rep_idx = 0xF;
    } else if (held_actor) {
        if (high)                       rep_idx = 0xA;
        else if (hero->actor_magic)     rep_idx = 0xB;
        else                            rep_idx = 9;
    } else {
        if (high)                       rep_idx = 0xD;
        else if (hero->actor_magic)     rep_idx = 0xE;
        else                            rep_idx = 0xC;
    }

    hero->actor_rep_index = rep_idx;

    /* Scene 6 (SceneGlobalFlags::Flagged), not 7. */
    bool ducking = (scene_name_flags[6] & 0x8) != 0;
    action_t *force = NULL;

    if (rep_idx != 0xA && rep_idx != 0xD) {
        if (ducking) {
            check_action_loaded(4);
            force = action_tab[4];
            if (force && force != hero->actor_act.act_action)
                hero->force_action_to_execute = force;
        }
    } else {
        if (!ducking) {
            check_action_loaded(3);
            force = action_tab[3];
            if (force && force != hero->actor_act.act_action)
                hero->force_action_to_execute = force;
        }
    }
}

/* game_my_rand_45FC10 — wraps C library rand(), returns uint16 */
int my_rand(void) {
    return (uint16_t)rand();
}

/* game_free_t_action_4538B8 — marks a taction slot as free */
void free_taction(taction_t *taction) {
    if (taction)
        taction->taction_index = -1;
}

static void try_merge_with_case(const char *dir_lower, const char *dir_upper,
                                const char *name, const char *ext) {
    char path[128];
    (void)dir_lower;
    snprintf(path, sizeof(path), "%s/%s.%s", dir_upper, name, ext);
    FILE *f = fopen_ci(path, "rb");
    if (f) {
        fclose(f);
        merge_a_file(path, 1);
    }
}

/* game_search_scene_dirs_and_load  E1: 0x446484 | E2: 0x451A5C */
void search_scene_dirs_and_load(const char *name) {
    last_scene_dir = -1;
    try_merge_with_case("scenes", "SCENES", name, "fan");
}

/* game_search_rep_dirs_and_load */
void search_rep_dirs_and_load(const char *name) {
    try_merge_with_case("rep", "REP", name, "fan");
}

/* game_search_actor_dirs_and_load */
void search_actor_dirs_and_load(const char *name) {
    last_actor_dir = -1;
    try_merge_with_case("actors", "ACTORS", name, "fan");
}

/* game_search_action_dirs_and_load */
void search_action_dirs_and_load(const char *name) {
    char path[128];
    if (file_find_action_file(name, path, sizeof(path)))
        merge_a_file(path, 1);
    else
        try_merge_with_case("actions", "ACTIONS", name, "fan");
}

/* game_play_ambients_457780 */
void play_ambients(void) {
    if (!sound_fx_on || !sound_is_on) return;
    for (int i = 0; i < num_ambients; ++i) {
        if (game_time - ambiant_last[i] > 7 * ambiant_freq[i]) {
            ambiant_last[i] = game_time;
            if ((my_rand() * 100 / 32768) < ambiant_rand[i])
                play_sound_ecstatica(NULL, ambiant_name[i], 0, ambiant_vol[i]);
        }
    }
}

/* game_save_vector  E1: 0x449284 | E2: 0x454E30 */
void save_vector(int16_t *vec, FILE *f) {
    for (int i = 0; i < 3; i++)
        putwLoHi(vec[i], f);
}

/* game_load_vector  E1: 0x4492A4 | E2: 0x454E50 */
void load_vector(int16_t *vec, FILE *f) {
    for (int i = 0; i < 3; i++)
        vec[i] = getwLoHi(f);
}

/* game_load_word  E1: 0x449364 | E2: 0x454F10 */
void load_word(int16_t *val, FILE *f) {
    *val = getwLoHi(f);
}

/* game_check_saved_game  E1: 0x448A64 | E2: 0x4542C8 */
int check_saved_game(int slot) {
    if (slot >= platform_save_slot_count())
        quit("load game no. out of range");
    char filename[32];
    platform_save_path(filename, sizeof(filename), slot, game_version);
    FILE *f = fopen(filename, "rb");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

/* game_save_matrix  E1: 0x44929C | E2: 0x454E70 */
void save_matrix(matrix3x3_t *m, FILE *f) {
    int16_t *p = (int16_t *)m;
    for (int i = 0; i < 9; i++)
        putwLoHi(p[i], f);
}

/* game_load_matrix  E1: 0x4492C4 | E2: 0x454EC0 */
void load_matrix(matrix3x3_t *m, FILE *f) {
    int16_t *p = (int16_t *)m;
    for (int i = 0; i < 9; i++)
        p[i] = getwLoHi(f);
}

/* game_remove_all_sounds  E1: 0x449678 | E2: 0x4552B0 */
void remove_all_sounds(void) {
    for (int i = 0; i < SOUND_POOL_SIZE; i++) {
        if (!(sound_heap_arr[i].use_flag & 0x8000))
            release_sound_buffer_win95(&sound_heap_arr[i]);
    }
    for (int i = 1; i < SOUND_TAB_SIZE; i++)
        sound_tab[i] = NULL;
    for (int i = 0; i < SOUND_POOL_SIZE; i++)
        free_sound(&sound_heap_arr[i]);
    top_of_sound_data = 0;
    sound_list = NULL;
}

static uint8_t texture_load_tried[TEXTURE_TAB_SIZE];

/* game_check_texture_loaded  E1: 0x446EA8
 * Called from put_a_cuboid before texture_tab is indexed. Without it an
 * unloaded texture silently degrades to a flat fill.
 *
 * The original retries on every call because its load always succeeds. The
 * port keeps texture data in the FAN stream, so a miss here is permanent, and
 * retrying would re-set stop_the_clock every frame — which zeroes
 * local_game_time (move.c) and freezes all action playback. Hence the
 * once-per-texture gate, cleared whenever the texture table is. */
void check_texture_loaded(int16_t texture_index) {
    if (texture_index < 0 || texture_index >= TEXTURE_TAB_SIZE) return;
    if (texture_tab[texture_index]) return;
    if (texture_load_tried[texture_index]) return;
    if (!texture_names || !texture_names[texture_index].field_0[0]) return;
    texture_load_tried[texture_index] = 1;

    char name[9];
    strncpy(name, texture_names[texture_index].field_0, 8);
    name[8] = '\0';

    char path[128];
    snprintf(path, sizeof(path), "TEXTURES/%s.fan", name);
    FILE *f = fopen_ci(path, "rb");
    if (!f) return;
    fclose(f);

    actor_t *save = selected_thing;
    stop_the_clock = true;
    merge_a_file(path, 1);
    selected_thing = save;
}

/* game_remove_all_textures  E1: 0x4496E0 | E2: 0x455318 */
void remove_all_textures(void) {
    memset(texture_load_tried, 0, sizeof(texture_load_tried));
    for (int i = 1; i < TEXTURE_TAB_SIZE; i++)
        texture_tab[i] = NULL;
    for (int i = 0; i < TEXTURE_POOL_SIZE; i++)
        texture_heap_arr[i].use_flag = 0x8000;
    top_of_texture_data = 0;
    texture_list = NULL;
}

/* game_check_lightning  E1: N/A | E2: 0x457964 */
void check_lightning(void) {
    if (lightning == 0) return;
    if (fade_to_black) return;
    if (fade_to_white) return;

    int32_t now = my_time();
    if (now == check_time) return;

    int32_t elapsed = now - check_time;
    lightning -= elapsed;
    if (lightning <= 0) {
        lightning = 0;
        set_palette(view_cmap);
        check_time = now;
        return;
    }

    /* Copy palette entries 8-15 from view_cmap to fade_cmap */
    for (int i = 8; i < 16; i++)
        fade_cmap[i] = view_cmap[i];

    if (!(lightning & 4)) {
        set_palette(view_cmap);
        check_time = now;
        return;
    }

    /* Lightning flash: convert palette entries 0-7 to grayscale */
    for (int i = 0; i < 8; i++) {
        uint8_t avg = (uint8_t)((view_cmap[i].R + view_cmap[i].G + view_cmap[i].B) / 3);
        fade_cmap[i].R = avg;
        fade_cmap[i].G = avg;
        fade_cmap[i].B = avg;
    }

    /* Convert palette entries 16-255 to grayscale */
    for (int i = 16; i < 256; i++) {
        uint8_t avg = (uint8_t)((view_cmap[i].R + view_cmap[i].G + view_cmap[i].B) / 3);
        fade_cmap[i].R = avg;
        fade_cmap[i].G = avg;
        fade_cmap[i].B = avg;
    }

    /* Adjust brightness based on grayscale intensity for entries 0-7 */
    for (int i = 0; i < 8; i++) {
        uint8_t *fc = (uint8_t *)&fade_cmap[i];
        uint8_t *vc = (uint8_t *)&view_cmap[i];
        for (int j = 0; j < 3; j++) {
            int val;
            if (fc[j] > 0x20)
                val = vc[j] * 5 / 4;
            else if (fc[j] > 0x10)
                val = vc[j];
            else
                val = vc[j] - 5;
            if (val < 0) val = 0;
            if (val > 63) val = 63;
            fc[j] = (uint8_t)val;
        }
    }

    /* Same brightness adjustment for entries 16-255 */
    for (int i = 16; i < 256; i++) {
        uint8_t *fc = (uint8_t *)&fade_cmap[i];
        uint8_t *vc = (uint8_t *)&view_cmap[i];
        for (int j = 0; j < 3; j++) {
            int val;
            if (fc[j] > 0x20)
                val = vc[j] * 5 / 4;
            else if (fc[j] > 0x10)
                val = vc[j];
            else
                val = vc[j] - 5;
            if (val < 0) val = 0;
            if (val > 63) val = 63;
            fc[j] = (uint8_t)val;
        }
    }

    set_palette(fade_cmap);
    check_time = now;
}

