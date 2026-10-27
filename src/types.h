#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* UNUSED_ATTR: suppress unused-variable/function warnings portably.
 * Open Watcom has no __attribute__. */
#if defined(_MSC_VER) || defined(__WATCOMC__)
#  define UNUSED_ATTR
#else
#  define UNUSED_ATTR __attribute__((unused))
#endif

#define FIXED_POINT_SHIFT  14
#define FIXED_POINT_ONE    (1 << FIXED_POINT_SHIFT)  /* 16384 */

/* Skip the start logos (speeds up development) */
/* #define SKIP_START_LOGO */

/* Skip triangle drawing (not yet verified) */
/* #define SKIP_DRAW_TRIANGLE */

/* Disable music playback */
/* #define DOES_NOT_PLAY_MUSIC */

/* Use deterministic timer values (for debugging) */
/* #define MY_TIME_DETERMINISTIC */

/* App continues running when losing focus */
#define APP_ALWAYS_ACTIVE

enum game_version_e {
    GAME_VERSION_E1 = 1,
    GAME_VERSION_E2 = 2
};
extern int game_version;

#define BITMAP_SIZE  0x4B281  /* 640*480 + header overhead */
/* The slack BITMAP_SIZE carries over 640*480. set_up_bitmaps() sizes the
 * planes to the active mode and keeps the same margin. */
#define BITMAP_SLACK (BITMAP_SIZE - 640 * 480)

/* Widest 480-row screen (32:9, as far as the full re-release's wide views
 * reach). The original 640 columns sit in the middle; the wide views fill the
 * sides. A build can lower it to cap the Aspect Ratio setting. */
#ifndef WIDE_SCREEN_MAX_WIDTH
#define WIDE_SCREEN_MAX_WIDTH 1706
#endif

#define ACTOR_POOL_SIZE      200
#define PART_POOL_SIZE      4000
#define ACTION_POOL_SIZE     400
#define SCENE_POOL_SIZE      200
#define SCRIPT_SIZE          600
#define REP_POOL_SIZE        200
#define EVENT_POOL_SIZE    40000
#define KEY_POOL_SIZE       4000
#define TRI_SIZE            1800
#define POINT_POOL_SIZE     1800
#define SOUND_POOL_SIZE      200
#define TEXTURE_POOL_SIZE     20
#define TACTION_POOL_SIZE    100

#define THING_TAB_SIZE      5000
#define ACTION_TAB_SIZE     2000
#define SCENE_TAB_SIZE      2500
#define CODE_TAB_SIZE       2000
#define REPERTOIRE_TAB_SIZE  500
#define SOUND_TAB_SIZE       700

#define E1_THING_TAB_SIZE       500
#define E1_ACTION_TAB_SIZE      1000
#define E1_SCENE_TAB_SIZE       1000
#define E1_REPERTOIRE_TAB_SIZE  150
#define E1_SOUND_TAB_SIZE       500
#define E1_SOUND_DRIVER_COUNT   5
#define E1_TUNE_COUNT           75

/* _PartTab slots for the hero's hands. Right is 0, left is 1 — established by
 * InRightHand/InLeftHand (0x4436DF / 0x443732), RightHandFree/LeftHandFree
 * (0x443CED / 0x443CC2) and by hold_thing_with_part picking the *_left
 * offset set on name_index 1 (0x41D4A2). */
#define E1_RIGHT_HAND_PART      0
#define E1_LEFT_HAND_PART       1
/* E2 numbers the same pair 8/7 (see the flip/roll branch in behaviour). */
#define E2_RIGHT_HAND_PART      8
#define E2_LEFT_HAND_PART       7
#define MAP_AREA_TAB_SIZE       100
#define TEXTURE_TAB_SIZE        200
#define PART_TAB_SIZE           555
#define POINT_TAB_SIZE          500
#define TRIANGLE_TAB_SIZE       500

#define PALETTES_MAX        1200
#define VISIBILITIES_MAX    1200
#define GRAPHICS_MAX        25
#define MAX_OFFSETS         16000

#define SCENE_STARTED   2
#define SCENE_FINISHED  4
#define SCENE_FLAGGED   8

#define ACTOR_FLAG_ACTIVE       0x0001
#define ACTOR_FLAG_VISIBLE      0x0008
#define ACTOR_FLAG_HIDDEN       0x0080
#define ACTOR_FLAG_PREPARED     0x0080
#define ACTOR_FLAG_STUCK        0x0800
#define ACTOR_FLAG_CANT_BE_HIT  0x2000
#define ACTOR_FLAG_CANT_STOP    0x4000

#define EVENT_FLAGS_PART        0x10
#define EVENT_FLAGS_TRIANGLE    0x20
#define EVENT_FLAGS_POINT       0x40
#define EVENT_FLAGS_NO_SUPRESS  0x100

#define CT_TOKENIZE_END    1
#define CT_TOKENIZE_ERROR  2

struct event_s;
struct ellipse_s;
struct actor_s;
struct taction_s;
struct point_s;
struct part_s;
struct scene_s;
struct script_s;
struct action_s;
struct rephead_s;
struct texture_s;
struct code_s;
struct sound_s;
struct tri_s;
struct line_of_code_s;
struct gadget_s;
struct request_s;
struct map_area_s;
struct key_s;
struct act_s;
struct camera_data_s;

typedef struct event_s event_t;
typedef struct ellipse_s ellipse_t;
typedef struct actor_s actor_t;
typedef struct taction_s taction_t;
typedef struct point_s point_t;
typedef struct part_s part_t;
typedef struct scene_s scene_t;
typedef struct script_s script_t;
typedef struct action_s action_t;
typedef struct rephead_s rephead_t;
typedef struct texture_s texture_t;
typedef struct code_s code_t;
typedef struct sound_s sound_t;
typedef struct tri_s tri_t;
typedef struct line_of_code_s line_of_code_t;
typedef struct gadget_s gadget_t;
typedef struct request_s request_t;
typedef struct map_area_s map_area_t;
typedef struct key_s key_state_t;
typedef struct act_s act_t;
typedef struct camera_data_s camera_data_t;

#pragma pack(push, 1)
typedef struct matrix3x3_s {
    int16_t _11, _12, _13;
    int16_t _21, _22, _23;
    int16_t _31, _32, _33;
} matrix3x3_t;  /* 18 bytes */
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct vector_s {
    union {
        struct {
            int16_t X;
            int16_t Y;
            int16_t Z;
        };
        int16_t data[3];
    };
} vector_t;  /* 6 bytes */
#pragma pack(pop)

static inline void set_vector(vector_t *v, int16_t x, int16_t y, int16_t z) {
    v->X = x; v->Y = y; v->Z = z;
}


#pragma pack(push, 1)
typedef struct long_vector_s {
    int32_t l_X;
    int32_t l_Y;
    int32_t l_Z;
} long_vector_t;  /* 12 bytes */
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct palette_entry_s {
    uint8_t R;
    uint8_t G;
    uint8_t B;
} palette_entry_t;  /* 3 bytes */
#pragma pack(pop)

/* Stands in for the (palette_entry_t){r,g,b} compound literal, which Open
 * Watcom does not accept. */
static inline palette_entry_t pal_rgb(uint8_t r, uint8_t g, uint8_t b) {
    palette_entry_t p;
    p.R = r; p.G = g; p.B = b;
    return p;
}

#pragma pack(push, 1)
typedef struct subarea_s {
    int16_t left;
    int16_t right;
    int16_t top;
    int16_t bottom;
} subarea_t;
#pragma pack(pop)

typedef struct part_tab_s {
    struct part_s *field_0[555];
} part_tab_t;

typedef struct triangle_tab_s {
    struct tri_s *field_0[500];
} triangle_tab_t;

typedef struct point_tab_s {
    struct point_s *field_0[500];
} point_tab_t;

#pragma pack(push, 1)
typedef struct action_dir_s {
    char field_0[26];
} action_dir_t;
#pragma pack(pop)

typedef enum EVENTS {
    NO_EVENT = 0,
    ROTATE, OFFSET, COLOUR, VECTOR1, VECTOR2, VECTOR3,
    ADD_PART, ADD_THING, TYPE, ADD_PART_TO_THING,
    PSEUDO_ACTION, PSEUDO_KEY, DISP_PNT, FLAGS, MOVE_ACT,
    RAND_ACT, RAND_INFO, ROTATE_THING, MOVE_THING,
    START_POSITION, THING_FLAGS, SCRIPT_MOVE, SCRIPT_TURN,
    SPAWN_ACTION, PSEUDO_SCENE, PSEUDO_SCRIPT, NEXT_SCENE,
    ANCHOR_PART, LOOSEN_JOINT, UNLOOSEN_JOINT, POSITION,
    TWO_PART_LIMB, FIX_PART, UNFIX_PART, UNMAKE_LIMB,
    REORIENT_THING, ADD_ELLIPSE_EVT, ADD_ELLIPSE_TO_KEY_EVT,
    ABSOLUTE_POS, ABSOLUTE_ROT, ADD_POINT, OFFSET_POINT,
    ADD_TRIANGLE, COLOUR_TRIANGLE, TRIANGLE_FLAGS, INTERACT,
    PSEUDO_ACTION_2, POINT_TO_POINT, HELD_OFFSET, HELD_ROTATE,
    BACKGROUND, PSEUDO_SCENE_2, PSEUDO_REP, REP_ENTRY,
    ACTOR_REP, DEF_ROTATE, DEF_OFFSET, DEF_VECTOR1, DEF_VECTOR2,
    DEF_COLOUR, DEF_FLAGS, DEF_POSITION, CUT_PART,
    HELD_OFF_LEFT, HELD_ROT_LEFT, THING_CODE, TRI_SHADE_NAME,
    N68, N69, PART_TEXTURE, END_ACTION, SHADE,
    MAKE_QUAD, TRI_TEX_NAME, TRI_TEXTURE1, TRI_TEXTURE2,
    TRI_TEXTURE3, THING_CODE_2
} EVENTS;

typedef enum BEHAVIORAL {
    BH_STOP = 0,
    BH_CHASE, BH_RETREAT, BH_WANDER, BH_JOYSTICK,
    BH_DYING, BH_SLEEP, BH_ATTACK, BH_GET_HIT,
    BH_RECOVERING, BH_GO_CLOSER, BH_DEAD,
    BH_TURN_LEFT, BH_TURN_RIGHT, BH_EXTERNAL, BH_FOLLOW
} BEHAVIORAL;

typedef enum CODE_TOKENS {
    CT_IF = 3, CT_ELSE, CT_ELSE_IF, CT_END_IF,
    CT_PLAY_SCENE, CT_STARTED, CT_NOT_STARTED,
    CT_FINISHED, CT_NOT_FINISHED, CT_REPEAT_SCENE,
    CT_ANY_KEY_PRESSED, CT_FACING_NORTH, CT_FACING_SOUTH,
    CT_FACING_EAST, CT_FACING_WEST, CT_NOT_PLAYING,
    CT_NO_KEY_PRESSED, CT_DRAW_SCENE, CT_DRAW_ROOF,
    CT_IN_RIGHT_HAND, CT_IN_LEFT_HAND, CT_ACTIVATED_BELOW,
    CT_NOT, CT_CHECK_ACTOR, CT_FORCE_ACTION, CT_SUBTITLE,
    CT_SCENE_FLAGGED, CT_SET_SCENE_FLAG, CT_CLEAR_SCENE_FLAG,
    CT_CLEAR_SUBTITLES, CT_CAMERA_WAS_OFF, CT_REPIS, CT_MAKE_REP,
    CT_MAP_AREA_HEIGHT, CT_START_TUNE, CT_STOP_TUNE,
    CT_PUT_GRAPHIC, CT_FEMALE, CT_CLEAR_GRAPHIC, CT_LOAD_GRAPHIC,
    CT_KEY1_PRESSED, CT_KEY3_PRESSED, CT_KEY1_OR_3_PRESSED,
    CT_SWAP_HANDS, CT_LEFT_HAND_FREE, CT_RIGHT_HAND_FREE,
    CT_RENDER_VIEWS, CT_FADE_OUT_TUNE, CT_CANT_BE_HIT,
    CT_CAN_BE_HIT, CT_HIT_POINTS_ABOVE, CT_MAKE_HIDDEN,
    CT_MAKE_VISIBLE, CT_QUIT_TO_DOS, CT_REMOVE_ALL_GRAPHICS,
    CT_MAKE_DEAD, CT_ADJUST_HIT_POINTS, CT_CAUSE_GET_HIT,
    CT_SET_FULL_HIT_POINTS, CT_EXECUTE_CODE, CT_PART_IS,
    CT_BLOCK_ACTOR, CT_BLOCK_WANDERERS, CT_BLOCK_ALL,
    CT_SET_HIT_POINTS, CT_FORCE_ESCAPE_KEY, CT_SET_HITPT_X100,
    CT_SET_FULL_HITPT_X100, CT_HITPTS_ABOVE_X10,
    CT_PLAY_END_SCENE, CT_EXECUTE_ACTOR_CODE,
    CT_MAKE_ACTOR_DEAD, CT_FOLLOW_ACTOR, CT_ADD_SCENE,
    CT_INIT_SCENE, CT_SPEED_FACTOR, CT_FORCE_FOLLOWING,
    CT_FORCE_TIMED, CT_RECOVER_HIT_POINTS, CT_CANT_STOP,
    CT_CAN_STOP, CT_LOAD_HERO, CT_LOAD_ACTOR, CT_OBJECT_IS,
    CT_STRENGTH_FACTOR, CT_HIT_FACTOR, CT_SPACE_PRESSED,
    CT_JUMP, CT_ARMOUR_FACTOR, CT_REMOVE_THIS_ACTOR,
    CT_REMOVE_ACTOR, CT_ACTOR_IS_DEAD, CT_ACTOR_IS_NEAR,
    CT_DEMO, CT_TREASURE, CT_GAME_TIMER, CT_RANDOM,
    CT_SPAWN_LIVE, CT_RANDOMIZE, CT_ADD_MAGIC, CT_MAGIC_FACTOR,
    CT_ADD_MAGIC_TO, CT_MAGIC_STOP_ACTION, CT_WANDERERS_ON,
    CT_TIMED_EXISTS, CT_REMOVE_TIMED, CT_SMART_BOMB,
    CT_MAGIC_BELOW, CT_AMBIANT_SOUND, CT_CLEAR_AMBIANT,
    CT_FADE_TO_BLACK, CT_FADE_TO_WHITE, CT_FADE_IN,
    CT_BLOCK_AQUATIC, CT_END_OF_INTRO, CT_ENGLISH,
    CT_FRENCH, CT_GERMAN, CT_ITALIAN, CT_SPANISH,
    CT_POLISH, CT_JAPANESE,
    /* Added by the modern re-release. PlaySceneAfterVoice defers the scene
     * until the calling scene's voice clip ends; SkipKeyPressed is a one-shot
     * latch of its skip input, used by both games' intro-skip checks. With no
     * voice playback and no separate skip input they run as PlayScene and
     * AnyKeyPressed, the commands they replaced in the scripts. */
    CT_PLAY_SCENE_AFTER_VOICE, CT_SKIP_KEY_PRESSED,
    CT_MAX
} CODE_TOKENS;

extern int debug_verbose;  /* 0 = quiet, 1 = important, 2 = verbose */
extern FILE *debug_log_file;

/* fflush only gets the bytes as far as DOS. The last thing the game does when
 * it hangs or faults is never a clean close, so the directory entry is left
 * stale and the log reads back empty — which is also what happens when the
 * emulator is killed, since it buffers the host file itself. INT 21h AH=68h
 * commits the file, and DOSBox implements it by flushing to the host. */
#if defined(__WATCOMC__) && defined(__DOS__)
void dbg_log_commit(void);
#else
#define dbg_log_commit() ((void)0)
#endif

/* DOS has nowhere for stderr to go but the screen the game is drawing on: once
 * the video mode is set, every trace line is written straight into the visible
 * framebuffer as text, and it is slow on top of that — each character goes out
 * through the BIOS. The file gets everything; the screen gets nothing. */
#if (defined(__WATCOMC__) && defined(__DOS__)) || defined(__vita__) || defined(__PSP__)
/* The handhelds have no console either; their stderr is a slow kernel write. */
#define DBG_LOG_STDERR(...) ((void)0)
#else
#define DBG_LOG_STDERR(...) fprintf(stderr, __VA_ARGS__)
#endif

/* Writes and flushes each line, except where platform_slow_writes() moved the
 * writing to a background thread (main.c). */
void dbg_log_printf(const char *fmt, ...);
/* Writes out anything still queued; call before exiting. */
void dbg_log_flush(void);

#define DBG_LOG(level, ...) do { if (debug_verbose >= (level)) { \
    DBG_LOG_STDERR(__VA_ARGS__); \
    if (debug_log_file) dbg_log_printf(__VA_ARGS__); \
} } while(0)

#endif /* TYPES_H */
