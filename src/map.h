#ifndef MAP_H
#define MAP_H

#include "types.h"

#pragma pack(push, 1)
struct map_area_s {
    uint16_t map_area_index;
    uint16_t map_area_element_num[10];
    struct map_area_s *next;
};
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct map_area_element_s {
    uint8_t def_height;
    uint8_t block_config;
    int16_t code_index_p1;
    char wanderer_spawn;
    uint8_t height2;
    char material;
    char height;
    int16_t camera_index;
    int16_t camera_override;
} map_area_element_t;  /* 12 bytes */
#pragma pack(pop)

extern uint16_t new_map[128][128];
extern map_area_element_t map_elements[60000];
/* Each element's second code (the extra word, version >= 10), as code index
 * + 1 like code_index_p1; 0 = none. Kept beside map_elements so the element
 * keeps the original 12-byte layout. Only the background builder reads it:
 * these are the original DrawRoof scripts. */
extern int16_t map_element_code2[60000];
#ifdef ECS_ENABLE_GL
/* E2's seven texture slots per element (texture name index, -1 = none):
 * 0 -Z side, 1 +Z side, 2 -X side, 3 +X side, 4 top, 5 bottom, 6 diagonal
 * side (Ecstatica II.exe, sub_140146430). Only the 3D map draws them. */
extern int16_t map_element_tex[60000][7];
#endif
extern int32_t top_of_map_elements;
/* supress_events_469BF0 — set while an act is wound forward by complete_act */
extern int16_t suppress_events;

void check_view(int camera_index);
void check_hot_spots(void);
void check_camera(void);
void copy_vga_to_svga(void);
void switch_camera(camera_data_t *camera);
void init_map(void);
int  position_is_visible(vector_t *pos);
void check_visibility(actor_t *actor);
void check_thing_name_invis(int actor_index);
void swap_out_actor(actor_t *actor);
void check_thing_name_vis(int actor_index);
void add_to_display_list_held(actor_t *actor);
void swap_in_actor(int actor_index);
void make_invisible(actor_t *actor);
int find_highest_camera_num(void);
void copy_background2to01(void);
void copy_background3to012(void);

#endif /* MAP_H */
