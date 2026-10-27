#ifndef DISPLAY_H
#define DISPLAY_H

#include "types.h"

#pragma pack(push, 1)
typedef struct graphic_name_s {
    char field_0[9];
} graphic_name_t;
#pragma pack(pop)

#pragma pack(push, 1)
struct camera_data_s {
    vector_t view_pos;
    vector_t view_rot;
    int16_t zoom_factor;
    int32_t unused_E;       /* never read; init_map sets 0x12 and 0x14 to -1 */
    int16_t unused_12;
    int16_t unused_14;
    int32_t time;
    int16_t top_clip;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct tri_s {
    int16_t tri_index;
    uint16_t tri_use_flag;
    point_t *point1;
    point_t *point2;
    point_t *point3;
    int16_t tri_color_3;
    int16_t tri_color_4;
    int16_t tri_color_1;
    int16_t tri_color_2;
    int16_t triangle_flags;
    int16_t unused_1A;
    int16_t tri_shade_name;
    struct tri_s *next;
    actor_t *parent_actor;
    int16_t shade_multiplier;
    point_t *quad_point4;
    int16_t texture_name_index;
    /* Per-vertex texture coords; u4/v4 belong to quad_point4. TRI_TEXTURE1..3
     * events carry them three at a time: (u1 v1 u2) (v2 u3 v3) (u4 v4). */
    int16_t u1;
    int16_t v1;
    int16_t u2;
    int16_t v2;
    int16_t u3;
    int16_t v3;
    int16_t u4;
    int16_t v4;
    int16_t unused_3E[3];
};  /* 68 bytes */
#pragma pack(pop)

#pragma pack(push, 1)
struct part_s {
    /* Part-like core (same layout as actor first 80 bytes) */
    int16_t name_index;
    int16_t flags;
    uint16_t type;
    matrix3x3_t matrix_1;
    vector_t joint_position;
    struct part_s *actor_parts_list;
    actor_t *parent_actor;
    vector_t Rotate;
    vector_t Offset;
    matrix3x3_t matrix_2;
    actor_t *holding_actor;
    struct part_s *next_in_path;
    struct part_s *next_in_display_list;

    /* Part-specific fields */
    vector_t world_offset;      /* joint offset rotated into the parent frame */
    vector_t displacement_point;
    int32_t field_5C;
    int32_t field_60;
    int16_t field_64;
    int16_t mask_distanse_;
    vector_t VECTOR_Squash;
    vector_t VECTOR_RelCentre;
    vector_t vector3;           /* VECTOR3 slot; mirrored by def_vector3, never set */
    int16_t field_7A;
    int32_t field_7C;
    vector_t ellipse_center;
    int16_t field_86;
    int32_t field_88;
    int16_t line_x;             /* put_a_line start point */
    int16_t line_y;
    int16_t field_90;
    vector_t vector_persp;
    int32_t field_98;
    int16_t field_9C;
    vector_t def_rotate;
    vector_t def_offset;
    vector_t def_displacement;
    vector_t def_Squash;
    vector_t def_RelCentre;
    vector_t def_vector3;
    int16_t def_type;
    int16_t default_color;
    int16_t work_color;
    int16_t default_flags;
    struct part_s *next;
    int16_t color;
    int16_t color_shade;
    int16_t max_squash;
    vector_t squash_ratio;
    vector_t projected_axes;
    vector_t offset_squash_ratio;
    vector_t rel_offset;
    matrix3x3_t matr_d;
    matrix3x3_t inverse_attitude;
    vector_t AbsPosition;
    vector_t def_position;
    int16_t parent_link_index;
    point_t *points_list;
    vector_t depth_offset;
    vector_t persp_origin;
    point_t *point_to_point;
    struct part_s *blocked_part;
    int16_t position_flags;
    int16_t def_pos_flags;
    actor_t *actor_2_held;
    int32_t filler_13E[6];
    int16_t part_texture_index;
    int16_t filler_158;
    int32_t filler_15A;
};  /* 350 bytes */
#pragma pack(pop)

#pragma pack(push, 1)
struct point_s {
    int16_t point_index;
    int16_t point_use_flag;
    vector_t offset_point;
    int16_t parent_part_index;
    int32_t field_C;
    vector_t world_position;
    vector_t screen_coord;
    vector_t def_offset_point;
    part_t *parent_part;
    struct point_s *next;
};  /* 42 bytes */
#pragma pack(pop)

#pragma pack(push, 1)
struct texture_s {
    int16_t textur_index;
    int16_t x_size;
    int16_t y_size;
    char *texture_data;
    struct texture_s *next;
    uint16_t use_flag;
    int32_t textur_time;
};  /* 20 bytes */
#pragma pack(pop)

extern int16_t screen_width;
extern int16_t screen_height;
extern int16_t hires_width;
extern int16_t hires_height;
extern int16_t screen_centre_x;
/* Width of the original view (320 or 640). The projection scales to it, not to
 * screen_width: a wide screen adds columns either side, not a wider lens. */
extern int16_t view_width;
extern int16_t screen_centre_y;
extern int16_t left_edge;
extern int16_t right_edge;
extern int16_t top_edge;
extern int16_t bottom_edge;
extern int32_t zoom_factor;
extern int32_t near_clip;
extern int16_t top_clip;
extern char *bitmap[6];
extern char *hires_bitmap[6];
extern int16_t *mask_map[3];
extern int16_t draw_mode[6];
extern int16_t db;
extern int16_t a_pen_colour;
extern int16_t b_pen_colour;
extern int16_t pen_position_x[6];
extern int16_t pen_position_y[6];
extern int16_t tx_w;
extern int16_t tx_h;
extern char character_set[256][48];
extern int16_t display_mode;
extern matrix3x3_t view_matrix;
extern vector_t view_rot;
extern vector_t view_pos;
extern camera_data_t camera[1200];
extern camera_data_t *active_camera;
extern int16_t cameras_viewed[150];
extern subarea_t clear_tab[2][100];
extern int16_t number_to_clear[2];
extern int32_t subtitles_time;
extern int16_t clear_subtitles;
extern subarea_t sub_area_to_clear[20];
extern char *subtitle_text[20];
extern int16_t subtitle_length[20];
extern int16_t subtitle_offset[20];
extern int16_t subtitle_status[20];
extern int16_t subtitle_colour[20];
extern int16_t subtitle_scale;   /* 1 = original 6x8 font, 2 = double */
extern int16_t subtitle_hold;    /* 0 = original 420 units, 1 = 3x, 2 = match speech */
#define SUBTITLE_HOLD_TICKS 420  /* 0x1A4 at 0x41D853 */
extern int16_t graphic_flag[GRAPHICS_MAX];
extern char *graphic_data[GRAPHICS_MAX];
extern int16_t graphic_x[GRAPHICS_MAX];
extern int16_t graphic_y[GRAPHICS_MAX];
extern int16_t graphic_size_x[GRAPHICS_MAX];
extern int16_t graphic_size_y[GRAPHICS_MAX];
extern graphic_name_t graphic_name_arr[GRAPHICS_MAX];
extern int16_t background_status;
extern int16_t level_of_detail;
extern int16_t set_palette_flag;

extern part_t part_heap_arr[PART_POOL_SIZE];
extern tri_t tri_arr[TRI_SIZE];
extern point_t point_heap_arr[POINT_POOL_SIZE];
extern texture_t texture_heap_arr[TEXTURE_POOL_SIZE];

void initialise_parts(void);
void restore_actor_entries(int index);
void restore_actor(int actor_index);
void new_game(void);
void initialise_actor(actor_t *actor);
void hold_thing_with_part(actor_t *actor, part_t *part);
void prepare_an_actor(actor_t *actor);
void prepare_parts(void);
void clear_a_stuck_thing(actor_t *actor);
void clear_a_subtitle(int index);
void draw_stuck_parts(void);
void unbake_stuck_actors(void);
void draw_parts(void);
void put_a_circle(part_t *part);
void long_view_trans_ellipse(part_t *part);
void put_a_line(part_t *part);
void show_parts(void);
void clear_expired_subtitles(void);
void draw_subtitles(void);
void show_subtitle(const char *text_str, int duration);
void req_clear_subtitles(void);

/* Matrix / vector operations */
void make_identity(matrix3x3_t *m);
void rotate_about_x(matrix3x3_t *m, int16_t angle);
void rotate_about_y(matrix3x3_t *m, int16_t angle);
void rotate_about_z(matrix3x3_t *m, int16_t angle);
void matrix_inverse(matrix3x3_t *in, matrix3x3_t *out);
void rotate_vector_about_x(vector_t *v, int16_t angle);
void rotate_vector_about_y(vector_t *v, int16_t angle);
void rotate_vector_about_z(vector_t *v, int16_t angle);
void pre_rotate_about_x(matrix3x3_t *m, int16_t angle);
void pre_rotate_about_y(matrix3x3_t *m, int16_t angle);
void pre_rotate_about_z(matrix3x3_t *m, int16_t angle);
void c_matrix_vector(vector_t *out, matrix3x3_t *mtx, vector_t *in);
void make_rot_matrix(matrix3x3_t *mtx, int16_t rx, int16_t ry, int16_t rz);
void add_vector(vector_t *a, vector_t *b);
void subtract_vector(vector_t *a, vector_t *b);
void copy_vector(vector_t *dst, vector_t *src);
void copy_matrix(matrix3x3_t *dst, matrix3x3_t *src);
void calculate_view_matrices(void);
void calculate_rot_matrix(matrix3x3_t *m, vector_t *rot);

/* Transform & projection */
void view_transform(vector_t *out, vector_t *world_pos);
void perspective_transform(vector_t *point);
void xx_long_view_transform(long_vector_t *out, vector_t *world_pos);
void long_view_transform(long_vector_t *out, vector_t *world_pos);
void long_perspective_transform(long_vector_t *point);
void perspec_trans_no_overflow_chk(vector_t *point);

/* Position finding */
void find_position_of_extremity(part_t *part);
void find_positions(actor_t *actor, int skip_first);
void find_a_position(actor_t *actor, part_t *part);
void find_positions_on_path(actor_t *actor);
void find_rotations_on_path(actor_t *actor);
void adjust_for_anchored_part(actor_t *actor);
void find_view_positions(actor_t *actor);

/* Display list management */
void clear_masking(void);
void clear_parts(void);
void put_parts_not_shadows(actor_t *actor);
void put_shadows(actor_t *actor);
void put_smoke(actor_t *actor);
void put_triangles(actor_t *actor);
void put_a_triangle(tri_t *tri);
void view_transform_ellipse(part_t *part);
void put_a_cuboid(part_t *part);
void put_an_ellipse(part_t *part);

/* Debug */

#endif /* DISPLAY_H */
