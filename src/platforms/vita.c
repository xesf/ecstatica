/**
 * platforms/vita.c
 *
 * PlayStation Vita backend (VitaSDK), built by platforms/vita/CMakeLists.txt.
 *
 *   Video     SceDisplay, 960x544 32-bit, triple-buffered in CDRAM. The 8-bit
 *             frame is palette-expanded and scaled on the CPU through per-row
 *             and per-column source tables, by two worker threads on cores 1
 *             and 2 while the game thread (core 0) goes on with the next frame.
 *   Input     SceCtrl buttons and sticks. The front panel is a pointer for
 *             menus; the rear panel halves are the missing second shoulder
 *             row (E1 per-hand pick-up, E2 magic).
 *   Timing    sceKernelGetProcessTimeWide.
 *   Audio     One 44100 Hz stereo BGM port fed by a software mixer thread.
 *             Music is silent (no OS synth).
 *
 * Game data lives in ux0:data/ecstatica, outside the VPK.
 */

#ifdef __vita__

#include "../platform.h"
#include "../asm_f.h"
#include "../types.h"
#include "../prof.h"

#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/display.h>
#include <psp2/audioout.h>
#include <psp2/power.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The archives, pools and backgrounds all live in newlib's heap. */
int _newlib_heap_size_user = 192 * 1024 * 1024;
/* Deep call chains through the renderers, matrices passed by value. */
unsigned int sceUserMainThreadStackSize = 1024 * 1024;

#define SCREEN_W      960
#define SCREEN_H      544
#define FB_COUNT      3
/* CDRAM blocks are allocated in 256 KB units. */
#define FB_BYTES      ((SCREEN_W * SCREEN_H * 4 + 0x3FFFF) & ~0x3FFFF)

#define MAX_RENDER_W  WIDE_SCREEN_MAX_WIDTH
#define MAX_RENDER_H  480

#define PKEY_TABLE_SIZE 256

struct platform_t {
    int render_w, render_h;

    bool keys_now[PKEY_TABLE_SIZE];
    bool keys_prev[PKEY_TABLE_SIZE];
    bool keys_latch[PKEY_TABLE_SIZE];

    int mouse_x, mouse_y;
    int mouse_buttons;

    uint64_t start_us;
};

static platform_t g_plat;

#ifdef ECS_PROFILE
static SceUID s_game_thread = -1;
#endif

/* ── Video ──────────────────────────────────────────────────── */

static SceUID    s_fb_block = -1;
static uint32_t *s_fb[FB_COUNT];
static int       s_fb_next;

/* view_cmap is 6-bit VGA RGB; the panel wants 0xAABBGGRR. */
static uint32_t s_lut[256];
static uint8_t  s_last_pal[768];
static bool     s_pal_valid;

/* Picture area and per-column/row source pixels; rebuilt when the render size
 * or scale mode changes. */
static int      s_dst_x, s_dst_w;
static uint16_t s_col_src[SCREEN_W];
static uint32_t s_row_off[SCREEN_H];
static int      s_map_w, s_map_h, s_map_mode = -1;
static int      s_scale_mode = SCALE_PILLARBOX;

static void build_scale_maps(int sw, int sh) {
    int v0, v1;

    /* A frame wider than 4:3 (the widescreen mode, 16:9 like the panel) fills
     * the width whatever the scale mode; no bars, nothing to crop. */
    int mode = (sw * 3 > sh * 4) ? -1 : s_scale_mode;

    /* The panel is wider than 4:3, so cropping is only ever vertical. */
    switch (mode) {
    case -1:
        s_dst_w = SCREEN_H * sw / sh;
        if (s_dst_w > SCREEN_W) s_dst_w = SCREEN_W;
        s_dst_x = (SCREEN_W - s_dst_w) / 2;
        v0 = 0;
        v1 = sh;
        break;
    case SCALE_CROP: {
        int full_h = SCREEN_W * 3 / 4;
        int off = (full_h - SCREEN_H) / 2;
        s_dst_w = SCREEN_W;
        s_dst_x = 0;
        v0 = off * sh / full_h;
        v1 = (off + SCREEN_H) * sh / full_h;
        break;
    }
    case SCALE_STRETCH:
        s_dst_w = SCREEN_W;
        s_dst_x = 0;
        v0 = 0;
        v1 = sh;
        break;
    default: /* SCALE_PILLARBOX */
        s_dst_w = SCREEN_H * 4 / 3;              /* 725 */
        s_dst_x = (SCREEN_W - s_dst_w) / 2;
        v0 = 0;
        v1 = sh;
        break;
    }

    for (int x = 0; x < s_dst_w; x++)
        s_col_src[x] = (uint16_t)(x * sw / s_dst_w);
    for (int y = 0; y < SCREEN_H; y++)
        s_row_off[y] = (uint32_t)(v0 + y * (v1 - v0) / SCREEN_H) * (uint32_t)sw;
    s_map_w = sw;
    s_map_h = sh;
    s_map_mode = s_scale_mode;
}

int platform_crop_inset_y(platform_t *p, int render_h) {
    if (s_scale_mode != SCALE_CROP) return 0;
    if (p && p->render_w * 3 > render_h * 4) return 0;
    int full_h = SCREEN_W * 3 / 4;
    int off = (full_h - SCREEN_H) / 2;
    return off * render_h / full_h;
}

/* ── Blit workers ───────────────────────────────────────────
 * platform_blit snapshots the frame and returns; worker 0 expands the top
 * half, worker 1 the bottom, and worker 0 queues the buffer once both are
 * done. The next platform_blit waits for that before touching any shared
 * state, so at most one frame is in flight. */

#define CPU_CORE(n) (0x10000 << (n))

static uint8_t   __attribute__((aligned(64))) s_stage[MAX_RENDER_W * MAX_RENDER_H];
static SceUID    s_blit_go[2] = { -1, -1 };
static SceUID    s_half_done = -1;
static SceUID    s_frame_done = -1;
static bool      s_blit_threads;
static bool      s_blit_pending;
static uint32_t *s_job_fb;

/* One whole display row per worker, in cached RAM. */
static uint32_t __attribute__((aligned(64))) s_row_scratch[2][SCREEN_W];

/* CDRAM is uncached, and a store per pixel made the expand ~50 ns a pixel.
 * Each source row is expanded once into cached scratch and copied out with
 * memcpy's burst stores, once per display row that repeats it (vertical
 * scaling repeats rows: 200 -> 544 nearly three times each). The scratch
 * row carries the bars, already black. */
static void expand_rows(uint32_t *fb, const uint8_t *framebuffer, int y0, int y1,
                        uint32_t *scratch) {
    const uint16_t *cols = s_col_src;
    const int dw = s_dst_w;
    uint32_t *dst = scratch + s_dst_x;
    uint32_t done_off = 0xFFFFFFFFu;

    memset(scratch, 0, SCREEN_W * sizeof(uint32_t));
    for (int y = y0; y < y1; y++) {
        if (s_row_off[y] != done_off) {
            const uint8_t *src = framebuffer + s_row_off[y];
            for (int x = 0; x < dw; x++)
                dst[x] = s_lut[src[cols[x]]];
            done_off = s_row_off[y];
        }
        memcpy(fb + y * SCREEN_W, scratch, SCREEN_W * sizeof(uint32_t));
    }
}

static void queue_frame(uint32_t *fb) {
    SceDisplayFrameBuf param;
    memset(&param, 0, sizeof(param));
    param.size = sizeof(param);
    param.base = fb;
    param.pitch = SCREEN_W;
    param.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    param.width = SCREEN_W;
    param.height = SCREEN_H;
    sceDisplaySetFrameBuf(&param, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

static int blit_worker(SceSize args, void *argp) {
    (void)args;
    const int half = *(const int *)argp;
    const int mid = SCREEN_H / 2;

    for (;;) {
        sceKernelWaitSema(s_blit_go[half], 1, NULL);
        uint32_t t0 = PROF_CLOCK();
        if (half) {
            expand_rows(s_job_fb, s_stage, mid, SCREEN_H, s_row_scratch[1]);
            PROF_WORKER(PROF_W_BLIT1, PROF_CLOCK() - t0);
            sceKernelSignalSema(s_half_done, 1);
        } else {
            expand_rows(s_job_fb, s_stage, 0, mid, s_row_scratch[0]);
            PROF_WORKER(PROF_W_BLIT0, PROF_CLOCK() - t0);
            sceKernelWaitSema(s_half_done, 1, NULL);
            queue_frame(s_job_fb);
            sceKernelSignalSema(s_frame_done, 1);
        }
    }
    return 0;
}

static void blit_wait(void) {
    if (s_blit_pending) {
        PROF_BEGIN(PROF_BLITWAIT);
        sceKernelWaitSema(s_frame_done, 1, NULL);
        PROF_END(PROF_BLITWAIT);
        s_blit_pending = false;
    }
}

/* On failure the blit stays on the game thread. */
static void blit_threads_init(void) {
    s_blit_go[0]  = sceKernelCreateSema("ecs_blit_go0", 0, 0, 1, NULL);
    s_blit_go[1]  = sceKernelCreateSema("ecs_blit_go1", 0, 0, 1, NULL);
    s_half_done   = sceKernelCreateSema("ecs_blit_half", 0, 0, 1, NULL);
    s_frame_done  = sceKernelCreateSema("ecs_blit_done", 0, 0, 1, NULL);
    if (s_blit_go[0] < 0 || s_blit_go[1] < 0 || s_half_done < 0 || s_frame_done < 0)
        return;

    for (int i = 0; i < 2; i++) {
        /* Below the raster bands on the same cores: the scheduler does not
         * time-slice equal priorities, so a band queued behind a 13 ms
         * expand held up the game thread for all of it. The expand has a
         * frame of slack; the bands do not. */
        SceUID th = sceKernelCreateThread(i ? "ecs_blit1" : "ecs_blit0", blit_worker,
                                          0x10000100 + 8, 16 * 1024, 0, CPU_CORE(1 + i), NULL);
        if (th < 0)
            return;
        sceKernelStartThread(th, sizeof(i), &i);
    }
    s_blit_threads = true;
    DBG_LOG(1, "[VITA] blit workers on cores 1 and 2\n");
}

static void upload_lut(const uint8_t *palette) {
    for (int i = 0; i < 256; i++) {
        uint32_t r = (uint32_t)(palette[i * 3 + 0] & 0x3F) << 2;
        uint32_t g = (uint32_t)(palette[i * 3 + 1] & 0x3F) << 2;
        uint32_t b = (uint32_t)(palette[i * 3 + 2] & 0x3F) << 2;
        s_lut[i] = 0xFF000000u | (b << 16) | (g << 8) | r;
    }
}

static bool video_init(void) {
    s_fb_block = sceKernelAllocMemBlock("ecs_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                        FB_BYTES * FB_COUNT, NULL);
    if (s_fb_block < 0)
        return false;

    void *base = NULL;
    sceKernelGetMemBlockBase(s_fb_block, &base);
    for (int i = 0; i < FB_COUNT; i++) {
        s_fb[i] = (uint32_t *)((uint8_t *)base + (size_t)i * FB_BYTES);
        /* Bars are never drawn again, so they start black. */
        memset(s_fb[i], 0, SCREEN_W * SCREEN_H * 4);
    }

    SceDisplayFrameBuf param;
    memset(&param, 0, sizeof(param));
    param.size = sizeof(param);
    param.base = s_fb[0];
    param.pitch = SCREEN_W;
    param.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    param.width = SCREEN_W;
    param.height = SCREEN_H;
    sceDisplaySetFrameBuf(&param, SCE_DISPLAY_SETBUF_NEXTFRAME);
    s_fb_next = 1;

    blit_threads_init();
    return true;
}

platform_t *platform_init(const char *title, int fb_width, int fb_height, int scale) {
    (void)title;
    (void)scale;

    platform_t *p = &g_plat;
    memset(p, 0, sizeof(*p));

    /* Cores 1 and 2 belong to the blit workers and the audio mixer. */
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), CPU_CORE(0));
#ifdef ECS_PROFILE
    s_game_thread = sceKernelGetThreadId();
#endif
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);

    p->render_w = fb_width;
    p->render_h = fb_height;
    p->mouse_x = fb_width / 2;
    p->mouse_y = fb_height / 2;
    p->start_us = sceKernelGetProcessTimeWide();

    video_init();
    build_scale_maps(fb_width, fb_height);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);

    return p;
}

bool platform_hires_supported(platform_t *p) {
    (void)p;
    return true;
}

bool platform_boot_low_res(platform_t *p) {
    (void)p;
    return false;
}

bool platform_scale_mode_supported(platform_t *p) {
    (void)p;
    return true;
}

void platform_set_scale_mode(platform_t *p, int mode) {
    (void)p;
    if (mode < SCALE_PILLARBOX || mode > SCALE_STRETCH)
        return;
    s_scale_mode = mode;
}

void platform_set_render_size(platform_t *p, int w, int h) {
    if (!p || w <= 0 || h <= 0)
        return;
    if (w == p->render_w && h == p->render_h)
        return;
    p->render_w = w;
    p->render_h = h;
    if (p->mouse_x >= w) p->mouse_x = w - 1;
    if (p->mouse_y >= h) p->mouse_y = h - 1;
}

void platform_blit(platform_t *p, const uint8_t *framebuffer, const uint8_t *palette) {
    if (!p || !framebuffer || s_fb_block < 0)
        return;

    int sw = p->render_w, sh = p->render_h;
    if (sw <= 0 || sh <= 0 || sw > MAX_RENDER_W || sh > MAX_RENDER_H)
        return;

    /* The workers read the maps, the LUT and the stage. */
    blit_wait();

    if (sw != s_map_w || sh != s_map_h || s_scale_mode != s_map_mode)
        build_scale_maps(sw, sh);

    if (palette && (!s_pal_valid || memcmp(s_last_pal, palette, 768) != 0)) {
        memcpy(s_last_pal, palette, 768);
        s_pal_valid = true;
        upload_lut(palette);
    }

    /* Triple buffering: this buffer is neither on screen nor queued. */
    uint32_t *fb = s_fb[s_fb_next];
    s_fb_next = (s_fb_next + 1) % FB_COUNT;

    if (!s_blit_threads) {
        expand_rows(fb, framebuffer, 0, SCREEN_H, s_row_scratch[0]);
        queue_frame(fb);
        return;
    }

    /* Menus and dialogs redraw the plane right after a flip, so the workers
     * get a copy rather than the game's plane. */
    memcpy(s_stage, framebuffer, (size_t)sw * (size_t)sh);
    s_job_fb = fb;
    s_blit_pending = true;
    sceKernelSignalSema(s_blit_go[0], 1);
    sceKernelSignalSema(s_blit_go[1], 1);
}

void platform_blit_rgba(platform_t *p, const uint8_t *framebuffer) {
    /* Only the debug overlay uses this, and it is off here. */
    (void)p;
    (void)framebuffer;
}

void platform_set_title(platform_t *p, const char *title) {
    (void)p;
    (void)title;
}

/* ── Input ──────────────────────────────────────────────────── */

/* 0..255 centred at 128, scaled to int16; win.c reads Y as positive-up. */
static int16_t stick_axis(unsigned char raw) {
    int v = ((int)raw - 128) * 258;
    /* Symmetric: the caller negates Y, and -(-32768) does not fit int16. */
    if (v >  32767) v =  32767;
    if (v < -32767) v = -32767;
    return (int16_t)v;
}

static SceCtrlData  s_pad;
static SceTouchData s_front, s_back;
static SceTouchPanelInfo s_front_info, s_back_info;
static bool s_touch_info_valid;

static bool s_rear_left, s_rear_right;

static void pump_pointer(platform_t *p) {
    /* Touching is holding the left button, mapped through the 4:3 picture;
     * the bars clamp to the nearest edge. */
    if (s_front.reportNum > 0) {
        int span_x = s_front_info.maxAaX - s_front_info.minAaX;
        int span_y = s_front_info.maxAaY - s_front_info.minAaY;
        if (span_x <= 0) span_x = SCREEN_W * 2;
        if (span_y <= 0) span_y = SCREEN_H * 2;

        int sx = (s_front.report[0].x - s_front_info.minAaX) * SCREEN_W / span_x;
        int sy = (s_front.report[0].y - s_front_info.minAaY) * SCREEN_H / span_y;

        p->mouse_x = (sx - s_dst_x) * p->render_w / (s_dst_w > 0 ? s_dst_w : 1);
        p->mouse_y = sy * p->render_h / SCREEN_H;
        p->mouse_buttons = PMOUSE_LEFT;
    } else {
        /* Stick-driven pointer, as on the PSP; req.c reads it in menus only. */
        int dx = (int)s_pad.lx - 128;
        int dy = (int)s_pad.ly - 128;
        const int dz = 24;

        if (dx > dz || dx < -dz) p->mouse_x += dx / 16;
        if (dy > dz || dy < -dz) p->mouse_y += dy / 16;
        p->mouse_buttons = (s_pad.buttons & SCE_CTRL_CROSS) ? PMOUSE_LEFT : 0;
    }

    if (p->mouse_x < 0) p->mouse_x = 0;
    if (p->mouse_y < 0) p->mouse_y = 0;
    if (p->mouse_x >= p->render_w) p->mouse_x = p->render_w - 1;
    if (p->mouse_y >= p->render_h) p->mouse_y = p->render_h - 1;
}

static void pump_rear(void) {
    s_rear_left = s_rear_right = false;
    int mid_x = (s_back_info.minAaX + s_back_info.maxAaX) / 2;
    if (mid_x <= 0) mid_x = SCREEN_W;
    for (unsigned i = 0; i < s_back.reportNum && i < SCE_TOUCH_MAX_REPORT; i++) {
        if (s_back.report[i].x < mid_x) s_rear_left = true;
        else s_rear_right = true;
    }
}

bool platform_pump_events(platform_t *p) {
    if (!p)
        return false;

    if (!s_touch_info_valid) {
        sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &s_front_info);
        sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &s_back_info);
        s_touch_info_valid = true;
    }

    /* Peek: the read calls block until the next sample. */
    sceCtrlPeekBufferPositive(0, &s_pad, 1);
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &s_front, 1);
    sceTouchPeek(SCE_TOUCH_PORT_BACK, &s_back, 1);

    memcpy(p->keys_prev, p->keys_now, sizeof(p->keys_now));

    pump_pointer(p);
    pump_rear();

    /* The PS button suspends at system level; there is no quit request. */
    return true;
}

bool platform_key_down(platform_t *p, int keycode) {
    if (!p || keycode < 0 || keycode >= PKEY_TABLE_SIZE)
        return false;
    return p->keys_now[keycode];
}

bool platform_key_pressed(platform_t *p, int keycode) {
    if (!p || keycode < 0 || keycode >= PKEY_TABLE_SIZE)
        return false;
    return p->keys_now[keycode] && !p->keys_prev[keycode];
}

bool platform_key_hit(platform_t *p, int keycode) {
    if (!p || keycode < 0 || keycode >= PKEY_TABLE_SIZE)
        return false;
    bool hit = p->keys_latch[keycode];
    p->keys_latch[keycode] = false;
    return hit;
}

int platform_mouse_state(platform_t *p, int *out_x, int *out_y) {
    if (!p)
        return 0;
    if (out_x) *out_x = p->mouse_x;
    if (out_y) *out_y = p->mouse_y;
    return p->mouse_buttons;
}

void platform_gamepad_poll(platform_t *p, platform_gamepad_state_t *state) {
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    if (!p)
        return;

    unsigned int b = s_pad.buttons;

    state->connected  = true;
    state->east_cycles_speed = true;
    state->dpad_up    = (b & SCE_CTRL_UP)    != 0;
    state->dpad_down  = (b & SCE_CTRL_DOWN)  != 0;
    state->dpad_left  = (b & SCE_CTRL_LEFT)  != 0;
    state->dpad_right = (b & SCE_CTRL_RIGHT) != 0;

    state->left_x  = stick_axis(s_pad.lx);
    state->left_y  = (int16_t)-stick_axis(s_pad.ly);
    state->right_x = stick_axis(s_pad.rx);
    state->right_y = (int16_t)-stick_axis(s_pad.ry);

    state->btn_south = (b & SCE_CTRL_CROSS)    != 0;
    state->btn_east  = (b & SCE_CTRL_CIRCLE)   != 0;
    state->btn_west  = (b & SCE_CTRL_SQUARE)   != 0;
    state->btn_north = (b & SCE_CTRL_TRIANGLE) != 0;
    state->btn_start = (b & SCE_CTRL_START)    != 0;
    state->btn_lstick = (b & SCE_CTRL_L3) != 0;   /* PS TV pads only */
    state->btn_rstick = (b & SCE_CTRL_R3) != 0;

    /* The Vita's shoulders report as the trigger bits; a PS TV pad's L1/R1
     * as their own. */
    bool l = (b & (SCE_CTRL_LTRIGGER | SCE_CTRL_L1)) != 0;
    bool r = (b & (SCE_CTRL_RTRIGGER | SCE_CTRL_R1)) != 0;
    bool select = (b & SCE_CTRL_SELECT) != 0;

    /* The rear panel halves are the second shoulder row; Select + shoulder
     * does the same, as on the PSP. */
    if (select && (l || r)) {
        state->btn_lt = l;
        state->btn_rt = r;
    } else {
        state->btn_lb = l;
        state->btn_rb = r;
        state->btn_select = select;
    }
    state->btn_lt |= s_rear_left;
    state->btn_rt |= s_rear_right;
}

/* ── Timing ─────────────────────────────────────────────────── */

uint32_t platform_ticks(platform_t *p) {
    uint64_t now = sceKernelGetProcessTimeWide();
    uint64_t start = p ? p->start_us : 0;
    return (uint32_t)((now - start) / 1000u);
}

void platform_delay(uint32_t ms) {
    if (ms)
        sceKernelDelayThread(ms * 1000u);
}

/* ── Threads ────────────────────────────────────────────────── */

struct platform_sem_s {
    SceUID id;
};

typedef struct {
    void (*fn)(void *);
    void *arg;
} thread_start_t;

static int thread_entry(SceSize args, void *argp) {
    (void)args;
    thread_start_t *start = (thread_start_t *)argp;
    start->fn(start->arg);
    return 0;
}

bool platform_thread_start(const char *name, void (*fn)(void *), void *arg,
                           int priority, int core) {
    /* The game thread runs at 0x10000100; a larger number is lower. */
    int prio = priority == PLATFORM_THREAD_BACKGROUND ? 0x10000100 + 16 : 0x10000100;
    /* "Any" must not inherit the game thread's core-0 pin. */
    int mask = (core >= 0 && core < 3) ? CPU_CORE(core) : (CPU_CORE(1) | CPU_CORE(2));
    SceUID th = sceKernelCreateThread(name, thread_entry, prio, 64 * 1024, 0, mask, NULL);
    if (th < 0)
        return false;
    thread_start_t start = { fn, arg };
    /* The kernel copies the arguments onto the new thread's stack. */
    sceKernelStartThread(th, sizeof(start), &start);
    return true;
}

/* Cores 0-2 are the application's; the fourth belongs to the system. */
int platform_cpu_count(void) {
    return 3;
}

/* Memory Stick / memory card writes can stall a thread for 100+ ms. */
bool platform_slow_writes(void) {
    return true;
}

platform_sem_t *platform_sem_create(int initial) {
    platform_sem_t *s = (platform_sem_t *)malloc(sizeof(*s));
    if (!s)
        return NULL;
    s->id = sceKernelCreateSema("ecs_sem", 0, initial, 0x7FFFFFFF, NULL);
    if (s->id < 0) {
        free(s);
        return NULL;
    }
    return s;
}

void platform_sem_wait(platform_sem_t *s) {
    sceKernelWaitSema(s->id, 1, NULL);
}

void platform_sem_post(platform_sem_t *s) {
    sceKernelSignalSema(s->id, 1);
}

#ifdef ECS_PROFILE
uint32_t prof_clock_us(void) {
    return sceKernelGetProcessTimeLow();
}

/* Linked with --wrap; only the game thread's calls are counted, since the
 * prefetch reader's are off the frame. */
SceUID __real_sceIoOpen(const char *file, int flags, SceMode mode);
SceUID __wrap_sceIoOpen(const char *file, int flags, SceMode mode) {
    uint32_t t = sceKernelGetProcessTimeLow();
    SceUID r = __real_sceIoOpen(file, flags, mode);
    if (sceKernelGetThreadId() == s_game_thread)
        prof_io_open(sceKernelGetProcessTimeLow() - t);
    return r;
}

SceSSize __real_sceIoRead(SceUID fd, void *buf, SceSize nbyte);
SceSSize __wrap_sceIoRead(SceUID fd, void *buf, SceSize nbyte) {
    uint32_t t = sceKernelGetProcessTimeLow();
    SceSSize r = __real_sceIoRead(fd, buf, nbyte);
    if (sceKernelGetThreadId() == s_game_thread)
        prof_io_read(sceKernelGetProcessTimeLow() - t, r);
    return r;
}

SceSSize __real_sceIoWrite(SceUID fd, const void *buf, SceSize nbyte);
SceSSize __wrap_sceIoWrite(SceUID fd, const void *buf, SceSize nbyte) {
    uint32_t t = sceKernelGetProcessTimeLow();
    SceSSize r = __real_sceIoWrite(fd, buf, nbyte);
    if (sceKernelGetThreadId() == s_game_thread)
        prof_io_write(sceKernelGetProcessTimeLow() - t);
    return r;
}

SceOff __real_sceIoLseek(SceUID fd, SceOff offset, int whence);
SceOff __wrap_sceIoLseek(SceUID fd, SceOff offset, int whence) {
    if (sceKernelGetThreadId() == s_game_thread)
        prof_io_seek();
    return __real_sceIoLseek(fd, offset, whence);
}
#endif

/* ── Audio ──────────────────────────────────────────────────────
 * Same mixer as the PSP backend: 16 voices point-resampled and summed by a
 * dedicated thread into one 44100 Hz stereo port.
 */

#define VITA_VOICES       16
#define VITA_AUDIO_RATE   44100
#define VITA_AUDIO_FRAMES 1024

typedef struct {
    const uint8_t *data;
    uint32_t       len;
    uint32_t       idx;      /* whole sample index into data */
    uint32_t       frac;     /* fractional position, 0..0xFFFF */
    uint32_t       step;     /* 16.16 increment: rate / VITA_AUDIO_RATE */
    int            vol;      /* 0..127 */
    int            pan;      /* -128..127 */
    bool           loop;
    volatile bool  active;
} vita_voice_t;

static vita_voice_t s_voices[VITA_VOICES];
static SceUID s_voice_sema = -1;
static SceUID s_audio_thread = -1;
static int    s_audio_port = -1;
static volatile bool s_audio_ready;
static volatile bool s_audio_quit;
static int s_sfx_vol = 255;            /* 0..255 master */

static int16_t __attribute__((aligned(64))) s_out[VITA_AUDIO_FRAMES * 2];
static int s_accum[VITA_AUDIO_FRAMES * 2];

static void voices_lock(void) {
    if (s_voice_sema >= 0)
        sceKernelWaitSema(s_voice_sema, 1, NULL);
}

static void voices_unlock(void) {
    if (s_voice_sema >= 0)
        sceKernelSignalSema(s_voice_sema, 1);
}

static void mix_block(void) {
    memset(s_accum, 0, sizeof(s_accum));

    voices_lock();
    for (int v = 0; v < VITA_VOICES; v++) {
        vita_voice_t *vo = &s_voices[v];
        if (!vo->active)
            continue;

        const uint8_t *data = vo->data;
        uint32_t idx = vo->idx, frac = vo->frac, step = vo->step, len = vo->len;

        int scale = (vo->vol * s_sfx_vol * 256) / (127 * 255);
        /* Attenuate the far side only, so centred sounds are not quieter. */
        int scale_l = vo->pan > 0 ? scale * (127 - vo->pan) / 127 : scale;
        int scale_r = vo->pan < 0 ? scale * (128 + vo->pan) / 128 : scale;

        for (int i = 0; i < VITA_AUDIO_FRAMES; i++) {
            if (idx >= len) {
                if (!vo->loop) { vo->active = false; break; }
                idx = 0;
                frac = 0;
            }

            int s = ((int)data[idx] - 128) << 8;
            s_accum[i * 2 + 0] += (s * scale_l) >> 8;
            s_accum[i * 2 + 1] += (s * scale_r) >> 8;

            /* Index and fraction apart: a single 16.16 position caps the
             * sample at 64 KB, and speech lines are longer. */
            frac += step;
            idx += frac >> 16;
            frac &= 0xFFFFu;
        }

        vo->idx = idx;
        vo->frac = frac;
    }
    voices_unlock();

    for (int i = 0; i < VITA_AUDIO_FRAMES * 2; i++) {
        int s = s_accum[i];
        if (s < -32768) s = -32768;
        if (s >  32767) s =  32767;
        s_out[i] = (int16_t)s;
    }
}

static int audio_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    while (!s_audio_quit) {
        mix_block();
        sceAudioOutOutput(s_audio_port, s_out);   /* blocks for one block */
    }
    return 0;
}

void platform_audio_init(void) {
    if (s_audio_ready)
        return;

    memset((void *)s_voices, 0, sizeof(s_voices));

    s_audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM,
                                       VITA_AUDIO_FRAMES, VITA_AUDIO_RATE,
                                       SCE_AUDIO_OUT_MODE_STEREO);
    if (s_audio_port < 0)
        return;

    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(s_audio_port,
                         SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);

    s_voice_sema = sceKernelCreateSema("ecs_voices", 0, 1, 1, NULL);
    if (s_voice_sema < 0) {
        sceAudioOutReleasePort(s_audio_port);
        s_audio_port = -1;
        return;
    }

    s_audio_quit = false;
    /* Shares core 2 with a blit worker, above it so a long blit cannot
     * starve the mixer. */
    s_audio_thread = sceKernelCreateThread("ecs_audio", audio_thread,
                                           0x10000100 - 16, 64 * 1024, 0, CPU_CORE(2), NULL);
    if (s_audio_thread < 0) {
        sceKernelDeleteSema(s_voice_sema);
        s_voice_sema = -1;
        sceAudioOutReleasePort(s_audio_port);
        s_audio_port = -1;
        return;
    }

    s_audio_ready = true;
    sceKernelStartThread(s_audio_thread, 0, NULL);
}

int platform_audio_play_pcm(const void *data, int length, int rate,
                            int volume, int pan, bool loop) {
    if (!s_audio_ready || !data || length <= 0)
        return -1;
    if (rate <= 0)
        rate = 22050;

    voices_lock();

    int slot = -1;
    for (int v = 0; v < VITA_VOICES; v++) {
        if (!s_voices[v].active) { slot = v; break; }
    }
    if (slot < 0)
        slot = 0;                       /* steal slot 0, as documented */

    s_voices[slot].active = false;
    s_voices[slot].data = (const uint8_t *)data;
    s_voices[slot].len = (uint32_t)length;
    s_voices[slot].idx = 0;
    s_voices[slot].frac = 0;
    s_voices[slot].step = (uint32_t)(((uint64_t)rate << 16) / VITA_AUDIO_RATE);
    s_voices[slot].vol = volume < 0 ? 0 : (volume > 127 ? 127 : volume);
    s_voices[slot].pan = pan;
    s_voices[slot].loop = loop;
    s_voices[slot].active = true;

    voices_unlock();
    return slot;
}

void platform_audio_stop_voice(int slot) {
    if (slot < 0 || slot >= VITA_VOICES)
        return;
    s_voices[slot].active = false;
}

void platform_audio_stop_all(void) {
    for (int v = 0; v < VITA_VOICES; v++)
        s_voices[v].active = false;
}

void platform_audio_shutdown(void) {
    if (!s_audio_ready)
        return;
    s_audio_ready = false;

    platform_audio_stop_all();
    s_audio_quit = true;

    if (s_audio_thread >= 0) {
        sceKernelWaitThreadEnd(s_audio_thread, NULL, NULL);
        sceKernelDeleteThread(s_audio_thread);
        s_audio_thread = -1;
    }
    if (s_audio_port >= 0) {
        sceAudioOutReleasePort(s_audio_port);
        s_audio_port = -1;
    }
    if (s_voice_sema >= 0) {
        sceKernelDeleteSema(s_voice_sema);
        s_voice_sema = -1;
    }
}

/* The Vita has no General MIDI synth, so tunes are silent, as on PSP and DOS. */
int platform_midi_play(const void *smf_data, int length, bool loop) {
    (void)smf_data;
    (void)length;
    (void)loop;
    return -1;
}

void platform_midi_stop(void) {
}

void platform_set_sfx_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 255) vol = 255;
    s_sfx_vol = vol;
}

void platform_set_music_volume(int vol) {
    (void)vol;
}

/* ── Capabilities ───────────────────────────────────────────── */

static char s_data_root[256];

/* One VPK per game, each its own LiveArea bubble and data folder. */
#ifndef VITA_GAME_DIR
#define VITA_GAME_DIR "e2"
#endif

void platform_early_init(void) {
    /* app0: is the read-only VPK mount. The bare folder is the fallback for a
     * card with one game. */
    static const char *const candidates[] = {
        "ux0:data/ecstatica/" VITA_GAME_DIR,
        "uma0:data/ecstatica/" VITA_GAME_DIR,
        "ux0:data/ecstatica",
        "uma0:data/ecstatica",
        "app0:data",
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (!file_dir_has_database(candidates[i]))
            continue;
        snprintf(s_data_root, sizeof(s_data_root), "%s", candidates[i]);
        file_set_data_root(s_data_root);
        file_flush_path_cache();
        /* The logs are opened by bare name, and app0: is read-only. */
        chdir(s_data_root);
        return;
    }

    /* No console to print to; let the engine fail with its own message. */
}

int platform_save_slot_count(void) {
    return 11;
}

void platform_save_path(char *buf, int bufsz, int slot, int game_version) {
    (void)game_version;   /* one install per folder, so no need to split */
    if (s_data_root[0])
        snprintf(buf, bufsz, "%s/saved/%04d.ecs", s_data_root, slot);
    else
        snprintf(buf, bufsz, "saved/%04d.ecs", slot);
}

void platform_save_prepare(void) {
    char dir[sizeof(s_data_root) + 8];
    if (s_data_root[0])
        snprintf(dir, sizeof(dir), "%s/saved", s_data_root);
    else
        snprintf(dir, sizeof(dir), "saved");
    sceIoMkdir(dir, 0777);
}

/* ── Shutdown ───────────────────────────────────────────────── */

void platform_shutdown(platform_t *p) {
    (void)p;
    platform_audio_shutdown();
    blit_wait();
    if (s_fb_block >= 0) {
        sceKernelFreeMemBlock(s_fb_block);
        s_fb_block = -1;
    }
    sceKernelExitProcess(0);
}

#endif /* __vita__ */
