#ifndef MENU_H
#define MENU_H

#include "types.h"

void draw_sub_items_and_save_area(void);
void clear_sub_items(void);
void show_menu_bar(void);
void initialise_game(void);
void start_game_medium(int not_used1, int not_used2);
void replay_skipped_startups(void);
void play_dead_scene(int scene_index);
void do_delete_thing(actor_t *actor);
void delete_parts(part_t *part);
void delete_triangle(tri_t *tri);
void do_delete_action(action_t *action);
void do_delete_scene(scene_t *scene);
void delete_key(key_state_t *key);
void remove_part(part_t *part);
void go_svga(void);
void go_vga(void);
int enhanced_graphics_available(void);
void set_enhanced_graphics(int enabled);
void set_widescreen(int enabled);
extern bool menu_no_continue;
bool menu_is_active(void);
void do_main_menu(void);
void draw_main_menu(void);

/* Step the graphics mode: asset set and renderer as one setting. Shared by the
 * Enhanced menu row and the G hotkey so the two cannot drift apart. */
void graphics_mode_cycle(int dir);

#endif /* MENU_H */
