#ifdef __linux__

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#ifdef ECS_ENABLE_GL
#include <GL/glx.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <alsa/asoundlib.h>
#include <linux/joystick.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>
#include <dirent.h>
#include <strings.h>
#include <ctype.h>
#include "platform.h"
#include "types.h"

#define MAX_KEYS 256

/* js button/axis indices follow the EV codes each driver declares, so they
 * shift between pads (hid-steam's BTN_TL2/TR2 push BTN_THUMBR from 10 to 12).
 * JSIOCGBTNMAP/JSIOCGAXMAP give the EV code per index, so everything is
 * resolved semantically. Caps are generous for the Deck's extra controls. */
#define JS_MAX_BUTTONS 64
#define JS_MAX_AXES    32

enum {
    GB_SOUTH, GB_EAST, GB_WEST, GB_NORTH, GB_LB, GB_RB,
    GB_SELECT, GB_START, GB_LSTICK, GB_RSTICK, GB_LT2, GB_RT2,
    GB_DUP, GB_DDOWN, GB_DLEFT, GB_DRIGHT, GB_COUNT
};
enum {
    GA_LX, GA_LY, GA_RX, GA_RY, GA_LT, GA_RT, GA_DX, GA_DY, GA_COUNT
};

/* Every pad node is tracked: a Steam Deck has the physical pad, Steam Input's
 * virtual X360 pad and more, and only one of them is live. */
#define JS_MAX_PADS 4

typedef struct {
    int      fd;
    int      index;
    int16_t  axes[JS_MAX_AXES];
    int16_t  axes_rest[JS_MAX_AXES];
    bool     axes_seen[JS_MAX_AXES];
    uint8_t  buttons[JS_MAX_BUTTONS];
    int8_t   btn[GB_COUNT];
    int8_t   ax[GA_COUNT];
} js_pad_t;

struct platform_t {
    int fb_width;
    int fb_height;
    int render_width;
    int render_height;
    int scale;
    uint32_t *rgba_buffer;
    bool quit_requested;

    bool key_state[MAX_KEYS];
    bool key_prev[MAX_KEYS];
    bool key_hit[MAX_KEYS];
    int  mouse_x;
    int  mouse_y;
    int  mouse_buttons;

    struct timespec start_time;

    Display *display;
    Window   window;
    GC       gc;
    XImage  *ximage;
    Atom     wm_delete;
    int      screen;

    /* GLX fixes the visual at window creation, so the GL context lives on a
     * child window over the parent. The parent keeps the event mask and the
     * software blit, so the renderer can switch at runtime. */
#ifdef ECS_ENABLE_GL
    Window   gl_window;
    GLXContext gl_ctx;
#endif
    bool     gl_active;   /* stays false on a software-only build */

    js_pad_t js_pads[JS_MAX_PADS];
    uint32_t js_next_scan;
};

static int x11_keysym_to_pkey(KeySym ks) {
    switch (ks) {
        case XK_Escape:    return PKEY_ESCAPE;
        case XK_Return:    return PKEY_RETURN;
        case XK_space:     return PKEY_SPACE;
        case XK_Up:        return PKEY_UP;
        case XK_Down:      return PKEY_DOWN;
        case XK_Left:      return PKEY_LEFT;
        case XK_Right:     return PKEY_RIGHT;
        case XK_F1:        return PKEY_F1;
        case XK_F2:        return PKEY_F2;
        case XK_F3:        return PKEY_F3;
        case XK_F4:        return PKEY_F4;
        case XK_F5:        return PKEY_F5;
        case XK_F6:        return PKEY_F6;
        case XK_F7:        return PKEY_F7;
        case XK_F8:        return PKEY_F8;
        case XK_F9:        return PKEY_F9;
        case XK_F10:       return PKEY_F10;
        case XK_F11:       return PKEY_F11;
        case XK_F12:       return PKEY_F12;
        case XK_a: case XK_A: return PKEY_A;
        case XK_d: case XK_D: return PKEY_D;
        case XK_s: case XK_S: return PKEY_S;
        case XK_w: case XK_W: return PKEY_W;
        case XK_q: case XK_Q: return PKEY_Q;
        case XK_e: case XK_E: return PKEY_E;
        case XK_g: case XK_G: return PKEY_G;
        case XK_i: case XK_I: return PKEY_I;
        case XK_p: case XK_P: return PKEY_P;
        case XK_c: case XK_C: return PKEY_C;
        case XK_m: case XK_M: return PKEY_M;
        case XK_z: case XK_Z: return PKEY_Z;
        case XK_Control_L: case XK_Control_R: return PKEY_LCTRL;
        case XK_Alt_L:     case XK_Alt_R:     return PKEY_LALT;
        case XK_Shift_L:   return PKEY_LSHIFT;
        case XK_Shift_R:   return PKEY_RSHIFT;
        case XK_KP_1: case XK_KP_End:       return PKEY_NUM1;
        case XK_KP_2: case XK_KP_Down:      return PKEY_NUM2;
        case XK_KP_3: case XK_KP_Page_Down: return PKEY_NUM3;
        case XK_KP_4: case XK_KP_Left:      return PKEY_NUM4;
        case XK_KP_5: case XK_KP_Begin:     return PKEY_NUM5;
        case XK_KP_6: case XK_KP_Right:     return PKEY_NUM6;
        case XK_KP_7: case XK_KP_Home:      return PKEY_NUM7;
        case XK_KP_8: case XK_KP_Up:        return PKEY_NUM8;
        case XK_KP_9: case XK_KP_Page_Up:   return PKEY_NUM9;
        case XK_b: case XK_B: return PKEY_B;
        case XK_f: case XK_F: return PKEY_F;
        case XK_h: case XK_H: return PKEY_H;
        case XK_l: case XK_L: return PKEY_L;
        case XK_n: case XK_N: return PKEY_N;
        case XK_o: case XK_O: return PKEY_O;
        case XK_r: case XK_R: return PKEY_R;
        case XK_t: case XK_T: return PKEY_T;
        case XK_v: case XK_V: return PKEY_V;
        case XK_x: case XK_X: return PKEY_X;
        case XK_Tab:          return PKEY_TAB;
        case XK_minus:        return PKEY_MINUS;
        case XK_equal:        return PKEY_EQUALS;
        case XK_bracketleft:  return PKEY_LBRACKET;
        case XK_bracketright: return PKEY_RBRACKET;
        case XK_comma:        return PKEY_COMMA;
        case XK_period:       return PKEY_PERIOD;
        case XK_Home:         return PKEY_HOME;
        case XK_Prior:        return PKEY_PGUP;
        case XK_End:          return PKEY_END;
        case XK_Next:         return PKEY_PGDN;
        default: return -1;
    }
}

platform_t *platform_init(const char *title, int fb_width, int fb_height, int scale) {
    platform_t *p = (platform_t *)calloc(1, sizeof(platform_t));
    if (!p) return NULL;

    p->fb_width      = fb_width;
    p->fb_height     = fb_height;
    p->render_width  = fb_width;
    p->render_height = fb_height;
    p->scale         = scale;

    clock_gettime(CLOCK_MONOTONIC, &p->start_time);

    p->rgba_buffer = (uint32_t *)calloc(fb_width * fb_height, sizeof(uint32_t));
    if (!p->rgba_buffer) { free(p); return NULL; }

    p->display = XOpenDisplay(NULL);
    if (!p->display) { free(p->rgba_buffer); free(p); return NULL; }

    p->screen = DefaultScreen(p->display);
    int win_w = fb_width * scale;
    int win_h = fb_height * scale;

    p->window = XCreateSimpleWindow(
        p->display,
        RootWindow(p->display, p->screen),
        100, 100, win_w, win_h,
        0,
        BlackPixel(p->display, p->screen),
        BlackPixel(p->display, p->screen)
    );

    XStoreName(p->display, p->window, title);

    XSelectInput(p->display, p->window,
        ExposureMask | KeyPressMask | KeyReleaseMask |
        ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
        StructureNotifyMask);

    p->wm_delete = XInternAtom(p->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(p->display, p->window, &p->wm_delete, 1);

    XSizeHints *hints = XAllocSizeHints();
    if (hints) {
        hints->flags = PMinSize | PMaxSize;
        hints->min_width  = win_w;
        hints->min_height = win_h;
        hints->max_width  = win_w;
        hints->max_height = win_h;
        XSetWMNormalHints(p->display, p->window, hints);
        XFree(hints);
    }

    p->gc = XCreateGC(p->display, p->window, 0, NULL);

    Visual *visual = DefaultVisual(p->display, p->screen);
    int depth = DefaultDepth(p->display, p->screen);

    p->ximage = XCreateImage(
        p->display, visual, depth, ZPixmap, 0,
        (char *)p->rgba_buffer,
        fb_width, fb_height,
        32, fb_width * 4
    );
    if (!p->ximage) {
        XFreeGC(p->display, p->gc);
        XDestroyWindow(p->display, p->window);
        XCloseDisplay(p->display);
        free(p->rgba_buffer);
        free(p);
        return NULL;
    }

    /* XDestroyImage would free the data pointer; we manage it ourselves */

    XMapWindow(p->display, p->window);
    XFlush(p->display);

    XkbSetDetectableAutoRepeat(p->display, True, NULL);

    /* Opened by the first platform_gamepad_poll(), which also resolves the
     * button and axis maps. */
    for (int i = 0; i < JS_MAX_PADS; i++) {
        p->js_pads[i].fd = -1;
        p->js_pads[i].index = -1;
    }

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

/* The window owns its aspect ratio; only PSP and Vita offer fit modes. */
bool platform_scale_mode_supported(platform_t *p) {
    (void)p;
    return false;
}

void platform_set_scale_mode(platform_t *p, int mode) {
    (void)p;
    (void)mode;
}

int platform_crop_inset_y(platform_t *p, int render_h) {
    (void)p;
    (void)render_h;
    return 0;
}

/* The window follows the frame's shape: 4:3 for 640x480 and for 320x200
 * (shown at 4:3, as on a CRT), wider for the widescreen mode. */
void platform_set_render_size(platform_t *p, int w, int h) {
    if (!p) return;
    p->render_width  = w;
    p->render_height = h;

    int fb_w = (h == p->fb_height && w * 3 > h * 4) ? w : p->fb_height * 4 / 3;
    if (fb_w == p->fb_width) return;

    uint32_t *buf = (uint32_t *)calloc((size_t)fb_w * p->fb_height, sizeof(uint32_t));
    if (!buf) return;
    Visual *visual = DefaultVisual(p->display, p->screen);
    int depth = DefaultDepth(p->display, p->screen);
    XImage *img = XCreateImage(p->display, visual, depth, ZPixmap, 0, (char *)buf,
                               fb_w, p->fb_height, 32, fb_w * 4);
    if (!img) { free(buf); return; }

    p->ximage->data = NULL;   /* the buffer is ours, not XDestroyImage's */
    XDestroyImage(p->ximage);
    free(p->rgba_buffer);
    p->ximage = img;
    p->rgba_buffer = buf;
    p->fb_width = fb_w;

    int win_w = fb_w * p->scale;
    int win_h = p->fb_height * p->scale;
    XSizeHints *hints = XAllocSizeHints();
    if (hints) {
        hints->flags = PMinSize | PMaxSize;
        hints->min_width  = hints->max_width  = win_w;
        hints->min_height = hints->max_height = win_h;
        XSetWMNormalHints(p->display, p->window, hints);
        XFree(hints);
    }
    XResizeWindow(p->display, p->window, (unsigned)win_w, (unsigned)win_h);
#ifdef ECS_ENABLE_GL
    if (p->gl_window)
        XResizeWindow(p->display, p->gl_window, (unsigned)win_w, (unsigned)win_h);
#endif
    XFlush(p->display);
}

/* ── Hardware rendering (GLX) ─────────────────────────────── */
#ifdef ECS_ENABLE_GL

typedef GLXContext (*PFN_glXCreateContextAttribsARB)(Display *, GLXFBConfig, GLXContext,
                                                     Bool, const int *);

bool platform_gfx_create(platform_t *p) {
    if (!p || !p->display) return false;
    if (p->gl_ctx) return true;

    int attrs[] = {
        GLX_X_RENDERABLE,  True,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE,   GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
        GLX_RED_SIZE,      8,
        GLX_GREEN_SIZE,    8,
        GLX_BLUE_SIZE,     8,
        GLX_ALPHA_SIZE,    8,
        GLX_DEPTH_SIZE,    24,
        GLX_DOUBLEBUFFER,  True,
        None
    };

    int n = 0;
    GLXFBConfig *cfgs = glXChooseFBConfig(p->display, p->screen, attrs, &n);
    if (!cfgs || n <= 0) return false;
    GLXFBConfig cfg = cfgs[0];

    XVisualInfo *vi = glXGetVisualFromFBConfig(p->display, cfg);
    if (!vi) { XFree(cfgs); return false; }

    /* No event mask on the child, so input still arrives at the parent. */
    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.colormap = XCreateColormap(p->display, p->window, vi->visual, AllocNone);
    swa.background_pixmap = None;
    swa.border_pixel = 0;
    swa.event_mask = 0;

    int win_w = p->fb_width  * p->scale;
    int win_h = p->fb_height * p->scale;

    p->gl_window = XCreateWindow(p->display, p->window, 0, 0, win_w, win_h, 0,
                                 vi->depth, InputOutput, vi->visual,
                                 CWColormap | CWBorderPixel | CWEventMask, &swa);
    XFree(vi);
    if (!p->gl_window) { XFree(cfgs); return false; }

    PFN_glXCreateContextAttribsARB create_ctx =
        (PFN_glXCreateContextAttribsARB)glXGetProcAddressARB(
            (const GLubyte *)"glXCreateContextAttribsARB");

    if (create_ctx) {
        int ctx_attrs[] = {
            GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
            GLX_CONTEXT_MINOR_VERSION_ARB, 3,
            GLX_CONTEXT_PROFILE_MASK_ARB,  GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
            None
        };
        p->gl_ctx = create_ctx(p->display, cfg, NULL, True, ctx_attrs);
    }
    XFree(cfgs);

    if (!p->gl_ctx) {
        /* Without ARB_create_context there is no core profile, and a legacy
         * context cannot compile #version 330. */
        XDestroyWindow(p->display, p->gl_window);
        p->gl_window = 0;
        return false;
    }

    /* Unmapped until platform_gfx_set_active, or it hides the software blit. */
    XSync(p->display, False);
    return true;
}

void platform_gfx_set_active(platform_t *p, bool active) {
    if (!p || !p->gl_window) return;
    if (active) XMapWindow(p->display, p->gl_window);
    else        XUnmapWindow(p->display, p->gl_window);
    XSync(p->display, False);
    p->gl_active = active;
    if (!active) {
        /* Repaint the parent, which has been covered up to now. */
        XClearArea(p->display, p->window, 0, 0, 0, 0, True);
    }
}

void platform_gfx_make_current(platform_t *p) {
    if (!p || !p->gl_ctx) return;
    glXMakeCurrent(p->display, p->gl_window, p->gl_ctx);
}

void platform_gfx_swap(platform_t *p) {
    if (!p || !p->gl_ctx) return;
    glXSwapBuffers(p->display, p->gl_window);
}

void platform_gfx_destroy(platform_t *p) {
    if (!p || !p->gl_ctx) return;
    glXMakeCurrent(p->display, None, NULL);
    glXDestroyContext(p->display, p->gl_ctx);
    p->gl_ctx = NULL;
    if (p->gl_window) {
        XDestroyWindow(p->display, p->gl_window);
        p->gl_window = 0;
    }
    p->gl_active = false;
}

void platform_gfx_drawable_size(platform_t *p, int *w, int *h) {
    if (!p) return;
    /* Fixed-size window and no backing scale in X11. */
    if (w) *w = p->fb_width  * p->scale;
    if (h) *h = p->fb_height * p->scale;
}

void *platform_gl_proc(const char *name) {
    return (void *)glXGetProcAddressARB((const GLubyte *)name);
}

#endif /* ECS_ENABLE_GL */

void platform_blit(platform_t *p, const uint8_t *framebuffer, const uint8_t *palette) {
    if (!p || !framebuffer || !palette) return;

    int rw = p->render_width;
    int rh = p->render_height;
    int fw = p->fb_width;
    int fh = p->fb_height;

    if (rw == fw && rh == fh) {
        int total = fw * fh;
        for (int i = 0; i < total; i++) {
            uint8_t idx = framebuffer[i];
            uint8_t r = (palette[idx * 3 + 0] & 0x3F) << 2;
            uint8_t g = (palette[idx * 3 + 1] & 0x3F) << 2;
            uint8_t b = (palette[idx * 3 + 2] & 0x3F) << 2;
            p->rgba_buffer[i] = (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    } else {
        for (int y = 0; y < fh; y++) {
            int sy = y * rh / fh;
            for (int x = 0; x < fw; x++) {
                int sx = x * rw / fw;
                uint8_t idx = framebuffer[sy * rw + sx];
                uint8_t r = (palette[idx * 3 + 0] & 0x3F) << 2;
                uint8_t g = (palette[idx * 3 + 1] & 0x3F) << 2;
                uint8_t b = (palette[idx * 3 + 2] & 0x3F) << 2;
                p->rgba_buffer[y * fw + x] = (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }
        }
    }

    if (p->scale == 1) {
        XPutImage(p->display, p->window, p->gc, p->ximage,
                  0, 0, 0, 0, fw, fh);
    } else {
        /* XPutImage does not scale: nearest-neighbour in software. */
        int win_w = fw * p->scale;
        int win_h = fh * p->scale;
        uint32_t *scaled = (uint32_t *)malloc(win_w * win_h * 4);
        if (scaled) {
            for (int y = 0; y < win_h; y++) {
                int sy = y / p->scale;
                for (int x = 0; x < win_w; x++) {
                    int sx = x / p->scale;
                    scaled[y * win_w + x] = p->rgba_buffer[sy * fw + sx];
                }
            }
            Visual *visual = DefaultVisual(p->display, p->screen);
            int depth = DefaultDepth(p->display, p->screen);
            XImage *scaled_img = XCreateImage(
                p->display, visual, depth, ZPixmap, 0,
                (char *)scaled, win_w, win_h, 32, win_w * 4
            );
            if (scaled_img) {
                XPutImage(p->display, p->window, p->gc, scaled_img,
                          0, 0, 0, 0, win_w, win_h);
                scaled_img->data = NULL;
                XDestroyImage(scaled_img);
            }
            free(scaled);
        }
    }

    XFlush(p->display);
}

void platform_blit_rgba(platform_t *p, const uint8_t *framebuffer) {
    if (!p || !framebuffer) return;

    memcpy(p->rgba_buffer, framebuffer, p->fb_width * p->fb_height * 4);

    if (p->scale == 1) {
        XPutImage(p->display, p->window, p->gc, p->ximage,
                  0, 0, 0, 0, p->fb_width, p->fb_height);
    } else {
        int fw = p->fb_width;
        int fh = p->fb_height;
        int win_w = fw * p->scale;
        int win_h = fh * p->scale;
        uint32_t *scaled = (uint32_t *)malloc(win_w * win_h * 4);
        if (scaled) {
            for (int y = 0; y < win_h; y++) {
                int sy = y / p->scale;
                for (int x = 0; x < win_w; x++) {
                    int sx = x / p->scale;
                    scaled[y * win_w + x] = p->rgba_buffer[sy * fw + sx];
                }
            }
            Visual *visual = DefaultVisual(p->display, p->screen);
            int depth = DefaultDepth(p->display, p->screen);
            XImage *scaled_img = XCreateImage(
                p->display, visual, depth, ZPixmap, 0,
                (char *)scaled, win_w, win_h, 32, win_w * 4
            );
            if (scaled_img) {
                XPutImage(p->display, p->window, p->gc, scaled_img,
                          0, 0, 0, 0, win_w, win_h);
                scaled_img->data = NULL;
                XDestroyImage(scaled_img);
            }
            free(scaled);
        }
    }

    XFlush(p->display);
}

bool platform_pump_events(platform_t *p) {
    if (!p) return false;

    memcpy(p->key_prev, p->key_state, sizeof(p->key_prev));

    while (XPending(p->display)) {
        XEvent ev;
        XNextEvent(p->display, &ev);

        switch (ev.type) {
            case KeyPress: {
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                int k = x11_keysym_to_pkey(ks);
                if (k >= 0 && k < MAX_KEYS) {
                    if (!p->key_state[k]) p->key_hit[k] = true;
                    p->key_state[k] = true;
                }
                break;
            }
            case KeyRelease: {
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                int k = x11_keysym_to_pkey(ks);
                if (k >= 0 && k < MAX_KEYS) p->key_state[k] = false;
                break;
            }
            case ButtonPress: {
                int rw = p->render_width;
                int rh = p->render_height;
                p->mouse_x = ev.xbutton.x * rw / (p->fb_width * p->scale);
                p->mouse_y = ev.xbutton.y * rh / (p->fb_height * p->scale);
                if (p->mouse_x < 0) p->mouse_x = 0;
                if (p->mouse_y < 0) p->mouse_y = 0;
                if (p->mouse_x >= rw) p->mouse_x = rw - 1;
                if (p->mouse_y >= rh) p->mouse_y = rh - 1;
                if (ev.xbutton.button == Button1) p->mouse_buttons |= PMOUSE_LEFT;
                if (ev.xbutton.button == Button3) p->mouse_buttons |= PMOUSE_RIGHT;
                if (ev.xbutton.button == Button2) p->mouse_buttons |= PMOUSE_MIDDLE;
                break;
            }
            case ButtonRelease: {
                int rw = p->render_width;
                int rh = p->render_height;
                p->mouse_x = ev.xbutton.x * rw / (p->fb_width * p->scale);
                p->mouse_y = ev.xbutton.y * rh / (p->fb_height * p->scale);
                if (p->mouse_x < 0) p->mouse_x = 0;
                if (p->mouse_y < 0) p->mouse_y = 0;
                if (p->mouse_x >= rw) p->mouse_x = rw - 1;
                if (p->mouse_y >= rh) p->mouse_y = rh - 1;
                if (ev.xbutton.button == Button1) p->mouse_buttons &= ~PMOUSE_LEFT;
                if (ev.xbutton.button == Button3) p->mouse_buttons &= ~PMOUSE_RIGHT;
                if (ev.xbutton.button == Button2) p->mouse_buttons &= ~PMOUSE_MIDDLE;
                break;
            }
            case MotionNotify: {
                int rw = p->render_width;
                int rh = p->render_height;
                p->mouse_x = ev.xmotion.x * rw / (p->fb_width * p->scale);
                p->mouse_y = ev.xmotion.y * rh / (p->fb_height * p->scale);
                if (p->mouse_x < 0) p->mouse_x = 0;
                if (p->mouse_y < 0) p->mouse_y = 0;
                if (p->mouse_x >= rw) p->mouse_x = rw - 1;
                if (p->mouse_y >= rh) p->mouse_y = rh - 1;
                break;
            }
            case ClientMessage: {
                if ((Atom)ev.xclient.data.l[0] == p->wm_delete)
                    p->quit_requested = true;
                break;
            }
            case Expose: {
                /* The GL child window owns the surface. */
                if (p->gl_active) break;
                if (p->ximage) {
                    if (p->scale == 1) {
                        XPutImage(p->display, p->window, p->gc, p->ximage,
                                  0, 0, 0, 0, p->fb_width, p->fb_height);
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    return !p->quit_requested;
}

bool platform_key_down(platform_t *p, int keycode) {
    if (!p || keycode < 0 || keycode >= MAX_KEYS) return false;
    return p->key_state[keycode];
}

bool platform_key_hit(platform_t *p, int keycode) {
    if (!p || keycode < 0 || keycode >= MAX_KEYS) return false;
    bool hit = p->key_hit[keycode];
    p->key_hit[keycode] = false;
    return hit;
}

bool platform_key_pressed(platform_t *p, int keycode) {
    if (!p || keycode < 0 || keycode >= MAX_KEYS) return false;
    return p->key_state[keycode] && !p->key_prev[keycode];
}

int platform_mouse_state(platform_t *p, int *out_x, int *out_y) {
    if (!p) return 0;
    if (out_x) *out_x = p->mouse_x;
    if (out_y) *out_y = p->mouse_y;
    return p->mouse_buttons;
}

uint32_t platform_ticks(platform_t *p) {
    static struct timespec s_start = {0, 0};
    struct timespec *ref;

    if (p) {
        ref = &p->start_time;
    } else {
        if (s_start.tv_sec == 0 && s_start.tv_nsec == 0)
            clock_gettime(CLOCK_MONOTONIC, &s_start);
        ref = &s_start;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    /* Signed: the tv_nsec difference can be negative. */
    int64_t elapsed_ms = (int64_t)(now.tv_sec - ref->tv_sec) * 1000
                       + ((int64_t)now.tv_nsec - (int64_t)ref->tv_nsec) / 1000000;
    if (elapsed_ms < 0) elapsed_ms = 0;
    return (uint32_t)elapsed_ms;
}

void platform_delay(uint32_t ms) {
    struct timespec ts, rem;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    /* Resume the remaining interval if a signal cuts the sleep short. */
    while (nanosleep(&ts, &rem) == -1 && errno == EINTR)
        ts = rem;
}

void platform_shutdown(platform_t *p) {
    if (!p) return;

    if (p->ximage) {
        /* Prevent XDestroyImage from freeing our buffer */
        p->ximage->data = NULL;
        XDestroyImage(p->ximage);
        p->ximage = NULL;
    }
    if (p->gc) {
        XFreeGC(p->display, p->gc);
    }
    if (p->window) {
        XDestroyWindow(p->display, p->window);
    }
    if (p->display) {
        XCloseDisplay(p->display);
    }
    for (int i = 0; i < JS_MAX_PADS; i++)
        if (p->js_pads[i].fd >= 0) close(p->js_pads[i].fd);
    if (p->rgba_buffer) {
        free(p->rgba_buffer);
        p->rgba_buffer = NULL;
    }
    free(p);
}

void platform_set_title(platform_t *p, const char *title) {
    if (!p || !title) return;
    XStoreName(p->display, p->window, title);
    XFlush(p->display);
}

/* Gamepad — Linux joystick API (/dev/input/js*) */

static bool name_contains(const char *hay, const char *needle) {
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*b && *a && tolower((unsigned char)*a) == tolower((unsigned char)*b))
            a++, b++;
        if (!*b) return true;
    }
    return false;
}

/* BTN_X and BTN_Y are aliases of BTN_NORTH and BTN_WEST, so drivers that fill
 * their table in Xbox letter order (xpad, hid-steam, Steam Input's virtual
 * X360 pad) report west and north swapped; hid-playstation and hid-nintendo do
 * not. The protocol cannot tell them apart, so key off the device name, with
 * an environment override. */
static bool js_face_buttons_swapped(int fd) {
    const char *env = getenv("ECSTATICA_GAMEPAD_SWAP_FACE");
    if (env) return atoi(env) != 0;

    char name[128] = "";
    if (ioctl(fd, JSIOCGNAME(sizeof(name)), name) < 0)
        return true;

    static const char *compass_correct[] = {
        "playstation", "dualshock", "dualsense", "ps3", "ps4", "ps5",
        "sony", "wireless controller",
        "nintendo", "joy-con", "pro controller", "switch",
    };
    for (size_t i = 0; i < sizeof(compass_correct) / sizeof(*compass_correct); i++)
        if (name_contains(name, compass_correct[i]))
            return false;
    return true;
}

/* Resolve js indices from the driver's code maps. The xpad fallback is used
 * only when the ioctls fail: guessing a slot would alias an unrelated control
 * onto a game action. */
static void js_build_maps(js_pad_t *pad) {
    for (int i = 0; i < GB_COUNT; i++) pad->btn[i] = -1;
    for (int i = 0; i < GA_COUNT; i++) pad->ax[i] = -1;

    uint16_t btnmap[KEY_MAX - BTN_MISC + 1];
    uint8_t  axmap[ABS_CNT];
    uint8_t  nbtn = 0, naxes = 0;
    memset(btnmap, 0, sizeof(btnmap));
    memset(axmap, 0, sizeof(axmap));

    int ok_b = (ioctl(pad->fd, JSIOCGBUTTONS, &nbtn) >= 0)
            && (ioctl(pad->fd, JSIOCGBTNMAP, btnmap) >= 0);
    int ok_a = (ioctl(pad->fd, JSIOCGAXES, &naxes) >= 0)
            && (ioctl(pad->fd, JSIOCGAXMAP, axmap) >= 0);
    bool swap_face = ok_b ? js_face_buttons_swapped(pad->fd) : false;

    if (ok_b) {
        if (nbtn > JS_MAX_BUTTONS) nbtn = JS_MAX_BUTTONS;
        for (int i = 0; i < nbtn; i++) {
            switch (btnmap[i]) {
                case BTN_SOUTH:  pad->btn[GB_SOUTH]  = (int8_t)i; break;
                case BTN_EAST:   pad->btn[GB_EAST]   = (int8_t)i; break;
                case BTN_WEST:
                    pad->btn[swap_face ? GB_NORTH : GB_WEST] = (int8_t)i; break;
                case BTN_NORTH:
                    pad->btn[swap_face ? GB_WEST : GB_NORTH] = (int8_t)i; break;
                case BTN_TL:     pad->btn[GB_LB]     = (int8_t)i; break;
                case BTN_TR:     pad->btn[GB_RB]     = (int8_t)i; break;
                case BTN_TL2:    pad->btn[GB_LT2]    = (int8_t)i; break;
                case BTN_TR2:    pad->btn[GB_RT2]    = (int8_t)i; break;
                case BTN_SELECT: pad->btn[GB_SELECT] = (int8_t)i; break;
                case BTN_START:  pad->btn[GB_START]  = (int8_t)i; break;
                case BTN_THUMBL: pad->btn[GB_LSTICK] = (int8_t)i; break;
                case BTN_THUMBR: pad->btn[GB_RSTICK] = (int8_t)i; break;
                /* hid-steam and the Sony drivers report the D-pad as four
                 * digital buttons; there is no hat axis to read. */
                case BTN_DPAD_UP:    pad->btn[GB_DUP]    = (int8_t)i; break;
                case BTN_DPAD_DOWN:  pad->btn[GB_DDOWN]  = (int8_t)i; break;
                case BTN_DPAD_LEFT:  pad->btn[GB_DLEFT]  = (int8_t)i; break;
                case BTN_DPAD_RIGHT: pad->btn[GB_DRIGHT] = (int8_t)i; break;
                default: break;
            }
        }
    }
    if (ok_a) {
        if (naxes > JS_MAX_AXES) naxes = JS_MAX_AXES;

        /* ABS_Z/RZ are triggers on xpad and DualShock 4 but the right stick on
         * DualShock 3 and most generic pads; read them as triggers only when
         * ABS_RX/RY exist. */
        bool has_rstick = false;
        for (int i = 0; i < naxes; i++)
            if (axmap[i] == ABS_RX) {
                for (int j = 0; j < naxes; j++)
                    if (axmap[j] == ABS_RY) has_rstick = true;
            }

        for (int i = 0; i < naxes; i++) {
            switch (axmap[i]) {
                case ABS_X:     pad->ax[GA_LX] = (int8_t)i; break;
                case ABS_Y:     pad->ax[GA_LY] = (int8_t)i; break;
                case ABS_RX:    pad->ax[GA_RX] = (int8_t)i; break;
                case ABS_RY:    pad->ax[GA_RY] = (int8_t)i; break;
                case ABS_Z:
                    pad->ax[has_rstick ? GA_LT : GA_RX] = (int8_t)i; break;
                case ABS_RZ:
                    pad->ax[has_rstick ? GA_RT : GA_RY] = (int8_t)i; break;
                case ABS_HAT0X: pad->ax[GA_DX] = (int8_t)i; break;
                case ABS_HAT0Y: pad->ax[GA_DY] = (int8_t)i; break;
                /* hid-steam puts its analog triggers here instead of Z/RZ. */
                case ABS_HAT2X:
                    if (pad->ax[GA_RT] < 0) pad->ax[GA_RT] = (int8_t)i;
                    break;
                case ABS_HAT2Y:
                    if (pad->ax[GA_LT] < 0) pad->ax[GA_LT] = (int8_t)i;
                    break;
                default: break;
            }
        }
    }

    /* xpad ordering, for the one case where the driver told us nothing at all. */
    if (!ok_b) {
        static const int8_t fb_btn[GB_COUNT] =
            { 0, 1, 2, 3, 4, 5, 6, 7, 9, 10, -1, -1, -1, -1, -1, -1 };
        memcpy(pad->btn, fb_btn, sizeof(fb_btn));
    }
    if (!ok_a) {
        static const int8_t fb_ax[GA_COUNT] = { 0, 1, 3, 4, 2, 5, 6, 7 };
        memcpy(pad->ax, fb_ax, sizeof(fb_ax));
    }

    if (getenv("ECSTATICA_GAMEPAD_DEBUG")) {
        char name[128] = "?";
        ioctl(pad->fd, JSIOCGNAME(sizeof(name)), name);
        fprintf(stderr, "[PAD] '%s' buttons=%d axes=%d maps=%d/%d swap_face=%d\n",
                name, nbtn, naxes, ok_b, ok_a, swap_face);
        fprintf(stderr, "[PAD] south=%d east=%d west=%d north=%d lb=%d rb=%d "
                        "select=%d start=%d lstick=%d rstick=%d lt2=%d rt2=%d "
                        "dpad=%d/%d/%d/%d\n",
                pad->btn[GB_SOUTH], pad->btn[GB_EAST], pad->btn[GB_WEST],
                pad->btn[GB_NORTH], pad->btn[GB_LB], pad->btn[GB_RB],
                pad->btn[GB_SELECT], pad->btn[GB_START], pad->btn[GB_LSTICK],
                pad->btn[GB_RSTICK], pad->btn[GB_LT2], pad->btn[GB_RT2],
                pad->btn[GB_DUP], pad->btn[GB_DDOWN],
                pad->btn[GB_DLEFT], pad->btn[GB_DRIGHT]);
        fprintf(stderr, "[PAD] lx=%d ly=%d rx=%d ry=%d lt=%d rt=%d dx=%d dy=%d\n",
                pad->ax[GA_LX], pad->ax[GA_LY], pad->ax[GA_RX], pad->ax[GA_RY],
                pad->ax[GA_LT], pad->ax[GA_RT], pad->ax[GA_DX], pad->ax[GA_DY]);
    }
}

static uint8_t js_read_btn(const js_pad_t *pad, int slot) {
    int i = pad->btn[slot];
    return (i >= 0 && i < JS_MAX_BUTTONS) ? pad->buttons[i] : 0;
}

static int16_t js_read_axis(const js_pad_t *pad, int slot) {
    int i = pad->ax[slot];
    return (i >= 0 && i < JS_MAX_AXES) ? pad->axes[i] : 0;
}

/* Triggers rest at -32767 (joydev-rescaled) or 0 depending on the driver, so
 * compare against the value from joydev's JS_EVENT_INIT. */
static bool js_axis_pulled(const js_pad_t *pad, int slot) {
    int i = pad->ax[slot];
    if (i < 0 || i >= JS_MAX_AXES) return false;
    return (int)pad->axes[i] - (int)pad->axes_rest[i] > 16000;
}

/* Needs a left stick and a south button: js nodes also carry accelerometers,
 * yokes and the Deck's motion device. */
static bool js_looks_like_pad(const js_pad_t *pad) {
    return pad->btn[GB_SOUTH] >= 0 && pad->ax[GA_LX] >= 0 && pad->ax[GA_LY] >= 0;
}

static bool js_index_open(const platform_t *p, int index) {
    for (int i = 0; i < JS_MAX_PADS; i++)
        if (p->js_pads[i].fd >= 0 && p->js_pads[i].index == index)
            return true;
    return false;
}

/* Open every /dev/input/js* node that describes a gamepad, up to JS_MAX_PADS.
 * ECSTATICA_GAMEPAD_DEVICE pins a single node ("2", "js2" or a full path) for
 * the case where a machine has so many that the useful one falls off the end. */
static void js_scan(platform_t *p) {
    const char *only = getenv("ECSTATICA_GAMEPAD_DEVICE");
    int only_index = -1;
    if (only && *only) {
        const char *digits = only;
        while (*digits && !isdigit((unsigned char)*digits)) digits++;
        only_index = *digits ? atoi(digits) : -1;
    }

    for (int n = 0; n < 16; n++) {
        if (only_index >= 0 && n != only_index) continue;
        if (js_index_open(p, n)) continue;

        int slot = -1;
        for (int i = 0; i < JS_MAX_PADS; i++)
            if (p->js_pads[i].fd < 0) { slot = i; break; }
        if (slot < 0) return;

        char path[32];
        snprintf(path, sizeof(path), "/dev/input/js%d", n);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        js_pad_t *pad = &p->js_pads[slot];
        memset(pad, 0, sizeof(*pad));
        pad->fd = fd;
        pad->index = n;
        js_build_maps(pad);
        if (js_looks_like_pad(pad)) continue;

        /* The once-a-second rescan reopens non-pads; warn once each. */
        static uint32_t warned = 0;
        if (getenv("ECSTATICA_GAMEPAD_DEBUG") && n < 32 && !(warned & (1u << n))) {
            warned |= 1u << n;
            fprintf(stderr, "[PAD] %s is not a gamepad, skipping\n", path);
        }
        close(fd);
        pad->fd = -1;
        pad->index = -1;
    }
}

/* Buttons are ORed; each stick axis comes from the pad pushing it furthest. */
static void js_merge(const js_pad_t *pad, platform_gamepad_state_t *state) {
    int16_t lx = js_read_axis(pad, GA_LX);
    int16_t ly = (int16_t)(-js_read_axis(pad, GA_LY));
    int16_t rx = js_read_axis(pad, GA_RX);
    int16_t ry = (int16_t)(-js_read_axis(pad, GA_RY));
    if (abs(lx) > abs(state->left_x))  state->left_x  = lx;
    if (abs(ly) > abs(state->left_y))  state->left_y  = ly;
    if (abs(rx) > abs(state->right_x)) state->right_x = rx;
    if (abs(ry) > abs(state->right_y)) state->right_y = ry;

    int16_t dx = js_read_axis(pad, GA_DX), dy = js_read_axis(pad, GA_DY);
    state->dpad_left  |= dx < -16000 || js_read_btn(pad, GB_DLEFT);
    state->dpad_right |= dx >  16000 || js_read_btn(pad, GB_DRIGHT);
    state->dpad_up    |= dy < -16000 || js_read_btn(pad, GB_DUP);
    state->dpad_down  |= dy >  16000 || js_read_btn(pad, GB_DDOWN);

    /* Analog triggers report as axes on most pads and as BTN_TL2/BTN_TR2 on
     * those that expose them digitally. */
    state->btn_lt |= js_axis_pulled(pad, GA_LT) || js_read_btn(pad, GB_LT2);
    state->btn_rt |= js_axis_pulled(pad, GA_RT) || js_read_btn(pad, GB_RT2);

    state->btn_south  |= js_read_btn(pad, GB_SOUTH) != 0;
    state->btn_east   |= js_read_btn(pad, GB_EAST) != 0;
    state->btn_west   |= js_read_btn(pad, GB_WEST) != 0;
    state->btn_north  |= js_read_btn(pad, GB_NORTH) != 0;
    state->btn_lb     |= js_read_btn(pad, GB_LB) != 0;
    state->btn_rb     |= js_read_btn(pad, GB_RB) != 0;
    state->btn_select |= js_read_btn(pad, GB_SELECT) != 0;
    state->btn_start  |= js_read_btn(pad, GB_START) != 0;
    state->btn_lstick |= js_read_btn(pad, GB_LSTICK) != 0;
    state->btn_rstick |= js_read_btn(pad, GB_RSTICK) != 0;
}

void platform_gamepad_poll(platform_t *p, platform_gamepad_state_t *state) {
    memset(state, 0, sizeof(*state));
    if (!p) return;

    /* Rescan for hotplugged pads, throttled — the scan is sixteen open()s. */
    uint32_t now = platform_ticks(p);
    if (now >= p->js_next_scan) {
        p->js_next_scan = now + 1000;
        js_scan(p);
    }

    for (int i = 0; i < JS_MAX_PADS; i++) {
        js_pad_t *pad = &p->js_pads[i];
        if (pad->fd < 0) continue;

        struct js_event ev;
        errno = 0;
        while (read(pad->fd, &ev, sizeof(ev)) == sizeof(ev)) {
            ev.type &= ~JS_EVENT_INIT;
            if (ev.type == JS_EVENT_BUTTON && ev.number < JS_MAX_BUTTONS) {
                pad->buttons[ev.number] = ev.value;
            } else if (ev.type == JS_EVENT_AXIS && ev.number < JS_MAX_AXES) {
                pad->axes[ev.number] = ev.value;
                if (!pad->axes_seen[ev.number]) {
                    pad->axes_seen[ev.number] = true;
                    pad->axes_rest[ev.number] = ev.value;
                }
            }
        }
        if (errno == ENODEV) {
            close(pad->fd);
            pad->fd = -1;
            pad->index = -1;
            continue;
        }

        state->connected = true;
        js_merge(pad, state);
    }
}

/* Audio backend -- ALSA + software mixer in a pthread */

#define AUDIO_MAX_VOICES 16
#define AUDIO_OUT_RATE   44100
#define AUDIO_OUT_CH     2
#define AUDIO_PERIOD     1024

typedef struct {
    const uint8_t *data;
    int   length;
    int   rate;
    int   volume;
    int   pan;
    double pos;
    bool  active;
    bool  loop;
} audio_voice_t;

static audio_voice_t s_voices[AUDIO_MAX_VOICES];
static pthread_mutex_t s_audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static snd_pcm_t *s_pcm = NULL;
static pthread_t s_audio_thread;
static volatile bool s_audio_running = false;
static float s_master_sfx_volume = 0.9f;
static float s_master_music_volume = 0.8f;

static void midi_teardown(void);

static void *audio_thread_func(void *arg) {
    (void)arg;

    int16_t buf[AUDIO_PERIOD * AUDIO_OUT_CH];

    while (s_audio_running) {
        memset(buf, 0, sizeof(buf));

        float mix[AUDIO_PERIOD * AUDIO_OUT_CH];
        memset(mix, 0, sizeof(mix));

        pthread_mutex_lock(&s_audio_mutex);
        for (int v = 0; v < AUDIO_MAX_VOICES; ++v) {
            audio_voice_t *voice = &s_voices[v];
            if (!voice->active) continue;

            double step = (double)voice->rate / (double)AUDIO_OUT_RATE;
            float db    = (float)(voice->volume - 127) * 0.25f;
            float vol   = powf(10.0f, db / 20.0f) * s_master_sfx_volume;
            float pan_norm = (float)voice->pan / 127.0f;
            float l_gain = vol * (pan_norm > 0 ? (1.0f - pan_norm) : 1.0f);
            float r_gain = vol * (pan_norm < 0 ? (1.0f + pan_norm) : 1.0f);

            for (int f = 0; f < AUDIO_PERIOD; ++f) {
                int idx = (int)voice->pos;
                if (idx >= voice->length) {
                    if (voice->loop) {
                        voice->pos = 0.0;
                        idx = 0;
                    } else {
                        voice->active = false;
                        break;
                    }
                }
                float s = ((float)voice->data[idx] - 128.0f) / 128.0f;
                mix[f * AUDIO_OUT_CH + 0] += s * l_gain;
                mix[f * AUDIO_OUT_CH + 1] += s * r_gain;
                voice->pos += step;
            }
        }
        pthread_mutex_unlock(&s_audio_mutex);

        for (int i = 0; i < AUDIO_PERIOD * AUDIO_OUT_CH; ++i) {
            float s = mix[i];
            if (s >  1.0f) s =  1.0f;
            if (s < -1.0f) s = -1.0f;
            buf[i] = (int16_t)(s * 32767.0f);
        }

        snd_pcm_sframes_t written = snd_pcm_writei(s_pcm, buf, AUDIO_PERIOD);
        if (written < 0) {
            snd_pcm_recover(s_pcm, (int)written, 1);
        }
    }

    return NULL;
}

void platform_audio_init(void) {
    if (s_pcm) return;

    int err = snd_pcm_open(&s_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        DBG_LOG(1, "[AUDIO] snd_pcm_open(default) failed: %s\n", snd_strerror(err));
        s_pcm = NULL;
        return;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(s_pcm, params);
    snd_pcm_hw_params_set_access(s_pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(s_pcm, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(s_pcm, params, AUDIO_OUT_CH);

    unsigned int rate = AUDIO_OUT_RATE;
    snd_pcm_hw_params_set_rate_near(s_pcm, params, &rate, NULL);

    snd_pcm_uframes_t period = AUDIO_PERIOD;
    snd_pcm_hw_params_set_period_size_near(s_pcm, params, &period, NULL);

    snd_pcm_uframes_t buffer_size = AUDIO_PERIOD * 4;
    snd_pcm_hw_params_set_buffer_size_near(s_pcm, params, &buffer_size);

    err = snd_pcm_hw_params(s_pcm, params);
    if (err < 0) {
        DBG_LOG(1, "[AUDIO] snd_pcm_hw_params failed: %s\n", snd_strerror(err));
        snd_pcm_close(s_pcm);
        s_pcm = NULL;
        return;
    }

    snd_pcm_prepare(s_pcm);

    s_audio_running = true;
    pthread_create(&s_audio_thread, NULL, audio_thread_func, NULL);
}

int platform_audio_play_pcm(const void *data, int length, int rate,
                            int volume, int pan, bool loop) {
    if (!s_pcm || !data || length <= 0) return -1;
    if (rate <= 0) rate = 22050;

    pthread_mutex_lock(&s_audio_mutex);
    int slot = -1;
    for (int i = 0; i < AUDIO_MAX_VOICES; ++i) {
        if (!s_voices[i].active) { slot = i; break; }
    }
    if (slot < 0) slot = 0;
    s_voices[slot].data   = (const uint8_t *)data;
    s_voices[slot].length = length;
    s_voices[slot].rate   = rate;
    s_voices[slot].volume = volume;
    s_voices[slot].pan    = pan;
    s_voices[slot].pos    = 0.0;
    s_voices[slot].loop   = loop;
    s_voices[slot].active = true;
    pthread_mutex_unlock(&s_audio_mutex);
    return slot;
}

void platform_audio_stop_voice(int slot) {
    if (slot < 0 || slot >= AUDIO_MAX_VOICES) return;
    pthread_mutex_lock(&s_audio_mutex);
    s_voices[slot].active = false;
    pthread_mutex_unlock(&s_audio_mutex);
}

void platform_audio_stop_all(void) {
    pthread_mutex_lock(&s_audio_mutex);
    for (int i = 0; i < AUDIO_MAX_VOICES; ++i) s_voices[i].active = false;
    pthread_mutex_unlock(&s_audio_mutex);
}

void platform_audio_shutdown(void) {
    midi_teardown();

    if (!s_pcm) return;

    s_audio_running = false;
    pthread_join(s_audio_thread, NULL);

    snd_pcm_drop(s_pcm);
    snd_pcm_close(s_pcm);
    s_pcm = NULL;
}

/* MIDI via FluidSynth and a GM soundfont found on disk, as the macOS
 * AVMIDIPlayer path but with no OS bank. libfluidsynth is dlopen'd, so neither
 * it nor a soundfont is required; without them music is silent. */

typedef void fluid_settings_t;
typedef void fluid_synth_t;
typedef void fluid_audio_driver_t;
typedef void fluid_player_t;

static void *s_fluid_lib = NULL;
static fluid_settings_t     *s_fl_settings = NULL;
static fluid_synth_t        *s_fl_synth    = NULL;
static fluid_audio_driver_t *s_fl_driver   = NULL;
static fluid_player_t       *s_fl_player   = NULL;
static bool s_midi_ready  = false;
static bool s_midi_failed = false;

static fluid_settings_t *(*fl_new_settings)(void);
static void  (*fl_delete_settings)(fluid_settings_t *);
static int   (*fl_settings_setnum)(fluid_settings_t *, const char *, double);
static int   (*fl_settings_setint)(fluid_settings_t *, const char *, int);
static fluid_synth_t *(*fl_new_synth)(fluid_settings_t *);
static void  (*fl_delete_synth)(fluid_synth_t *);
static int   (*fl_synth_sfload)(fluid_synth_t *, const char *, int);
static void  (*fl_synth_set_gain)(fluid_synth_t *, float);
static int   (*fl_synth_system_reset)(fluid_synth_t *);
static fluid_audio_driver_t *(*fl_new_audio_driver)(fluid_settings_t *,
                                                    fluid_synth_t *);
static void  (*fl_delete_audio_driver)(fluid_audio_driver_t *);
static fluid_player_t *(*fl_new_player)(fluid_synth_t *);
static void  (*fl_delete_player)(fluid_player_t *);
static int   (*fl_player_add_mem)(fluid_player_t *, const void *, size_t);
static int   (*fl_player_set_loop)(fluid_player_t *, int);
static int   (*fl_player_play)(fluid_player_t *);
static int   (*fl_player_stop)(fluid_player_t *);
static int   (*fl_player_join)(fluid_player_t *);

static bool midi_path_readable(const char *p) {
    return p && p[0] && access(p, R_OK) == 0;
}

static bool midi_join_path(char *out, size_t outlen,
                           const char *dir, const char *name) {
    int len = snprintf(out, outlen, "%s/%s", dir, name);
    return len > 0 && (size_t)len < outlen;
}

/* Preferred banks first, then any soundfont in the directory. */
static bool midi_scan_dir(const char *dir, char *out, size_t outlen) {
    static const char *preferred[] = {
        "default.sf2", "FluidR3_GM.sf2", "FluidR3_GM2-2.sf2",
        "default-GM.sf2", "GeneralUser.sf2", "GeneralUser_GS.sf2",
        "TimGM6mb.sf2", NULL
    };
    char path[1024];

    for (int i = 0; preferred[i]; ++i) {
        if (midi_join_path(path, sizeof(path), dir, preferred[i]) &&
            midi_path_readable(path)) {
            snprintf(out, outlen, "%s", path);
            return true;
        }
    }

    DIR *d = opendir(dir);
    if (!d) return false;

    bool found = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 5) continue;
        if (strcasecmp(e->d_name + n - 4, ".sf2") != 0 &&
            strcasecmp(e->d_name + n - 4, ".sf3") != 0) continue;
        if (!midi_join_path(path, sizeof(path), dir, e->d_name)) continue;
        if (!midi_path_readable(path)) continue;
        snprintf(out, outlen, "%s", path);
        found = true;
        break;
    }
    closedir(d);
    return found;
}

static bool midi_find_soundfont(char *out, size_t outlen) {
    const char *env = getenv("ECSTATICA_SOUNDFONT");
    if (midi_path_readable(env)) {
        snprintf(out, outlen, "%s", env);
        return true;
    }

    /* Game data directory (cwd), so a bank can ship alongside the data. */
    if (midi_scan_dir(".", out, outlen)) return true;
    /* The re-release bundles a GM bank with its data. */
    if (midi_scan_dir("Audio/Music/Instruments", out, outlen)) return true;

    const char *home = getenv("HOME");
    if (home && home[0]) {
        char dir[1024];
        if (midi_join_path(dir, sizeof(dir), home, ".local/share/soundfonts") &&
            midi_scan_dir(dir, out, outlen))
            return true;
    }

    static const char *sys_dirs[] = {
        "/usr/share/soundfonts",
        "/usr/local/share/soundfonts",
        "/usr/share/sounds/sf2",
        "/usr/share/sounds/sf3",
        NULL
    };
    for (int i = 0; sys_dirs[i]; ++i)
        if (midi_scan_dir(sys_dirs[i], out, outlen)) return true;

    return false;
}

static void midi_teardown(void) {
    if (s_fl_player) {
        fl_player_stop(s_fl_player);
        fl_player_join(s_fl_player);
        fl_delete_player(s_fl_player);
        s_fl_player = NULL;
    }
    if (s_fl_driver)   { fl_delete_audio_driver(s_fl_driver); s_fl_driver = NULL; }
    if (s_fl_synth)    { fl_delete_synth(s_fl_synth);         s_fl_synth = NULL; }
    if (s_fl_settings) { fl_delete_settings(s_fl_settings);   s_fl_settings = NULL; }
    if (s_fluid_lib)   { dlclose(s_fluid_lib);                s_fluid_lib = NULL; }
    s_midi_ready = false;
}

#define FL_SYM(var, name) do {                                  \
        *(void **)(&(var)) = dlsym(s_fluid_lib, (name));        \
        if (!(var)) {                                           \
            DBG_LOG(1, "[MIDI] libfluidsynth missing %s\n", (name)); \
            goto fail;                                          \
        }                                                       \
    } while (0)

static bool midi_init(void) {
    if (s_midi_ready)  return true;
    if (s_midi_failed) return false;
    s_midi_failed = true;   /* probe once; stay silent thereafter */

    static const char *libs[] = {
        "libfluidsynth.so.3", "libfluidsynth.so.2", "libfluidsynth.so", NULL
    };
    for (int i = 0; libs[i] && !s_fluid_lib; ++i)
        s_fluid_lib = dlopen(libs[i], RTLD_LAZY | RTLD_LOCAL);

    if (!s_fluid_lib) {
        DBG_LOG(1, "[MIDI] libfluidsynth not found — music disabled\n");
        return false;
    }

    FL_SYM(fl_new_settings,        "new_fluid_settings");
    FL_SYM(fl_delete_settings,     "delete_fluid_settings");
    FL_SYM(fl_settings_setnum,     "fluid_settings_setnum");
    FL_SYM(fl_settings_setint,     "fluid_settings_setint");
    FL_SYM(fl_new_synth,           "new_fluid_synth");
    FL_SYM(fl_delete_synth,        "delete_fluid_synth");
    FL_SYM(fl_synth_sfload,        "fluid_synth_sfload");
    FL_SYM(fl_synth_set_gain,      "fluid_synth_set_gain");
    FL_SYM(fl_new_audio_driver,    "new_fluid_audio_driver");
    FL_SYM(fl_delete_audio_driver, "delete_fluid_audio_driver");
    FL_SYM(fl_new_player,          "new_fluid_player");
    FL_SYM(fl_delete_player,       "delete_fluid_player");
    FL_SYM(fl_player_add_mem,      "fluid_player_add_mem");
    FL_SYM(fl_player_set_loop,     "fluid_player_set_loop");
    FL_SYM(fl_player_play,         "fluid_player_play");
    FL_SYM(fl_player_stop,         "fluid_player_stop");
    FL_SYM(fl_player_join,         "fluid_player_join");

    /* Optional — only used to clear hanging notes between tunes. */
    *(void **)(&fl_synth_system_reset) =
        dlsym(s_fluid_lib, "fluid_synth_system_reset");

    char sf[1024];
    if (!midi_find_soundfont(sf, sizeof(sf))) {
        DBG_LOG(1, "[MIDI] no soundfont found — music disabled "
                   "(set ECSTATICA_SOUNDFONT=/path/to/bank.sf2)\n");
        goto fail;
    }

    s_fl_settings = fl_new_settings();
    if (!s_fl_settings) goto fail;
    fl_settings_setnum(s_fl_settings, "synth.sample-rate", (double)AUDIO_OUT_RATE);
    fl_settings_setint(s_fl_settings, "synth.midi-channels", 16);

    s_fl_synth = fl_new_synth(s_fl_settings);
    if (!s_fl_synth) goto fail;

    if (fl_synth_sfload(s_fl_synth, sf, 1) == -1) {
        DBG_LOG(1, "[MIDI] could not load soundfont %s\n", sf);
        goto fail;
    }
    fl_synth_set_gain(s_fl_synth, s_master_music_volume);

    s_fl_driver = fl_new_audio_driver(s_fl_settings, s_fl_synth);
    if (!s_fl_driver) {
        DBG_LOG(1, "[MIDI] could not open fluidsynth audio driver\n");
        goto fail;
    }

    DBG_LOG(1, "[MIDI] fluidsynth ready, soundfont: %s\n", sf);
    s_midi_ready  = true;
    s_midi_failed = false;
    return true;

fail:
    midi_teardown();
    return false;
}

#undef FL_SYM

int platform_midi_play(const void *smf_data, int length, bool loop) {
    if (!smf_data || length <= 0) return -1;
    if (!midi_init()) return -1;

    platform_midi_stop();

    s_fl_player = fl_new_player(s_fl_synth);
    if (!s_fl_player) return -1;

    if (fl_player_add_mem(s_fl_player, smf_data, (size_t)length) != 0) {
        DBG_LOG(1, "[MIDI] player rejected %d-byte SMF\n", length);
        fl_delete_player(s_fl_player);
        s_fl_player = NULL;
        return -1;
    }

    fl_player_set_loop(s_fl_player, loop ? -1 : 1);
    fl_player_play(s_fl_player);
    return 0;
}

void platform_midi_stop(void) {
    if (!s_fl_player) return;

    fl_player_stop(s_fl_player);
    fl_player_join(s_fl_player);
    fl_delete_player(s_fl_player);
    s_fl_player = NULL;

    if (fl_synth_system_reset) fl_synth_system_reset(s_fl_synth);
}

void platform_set_sfx_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 255) vol = 255;
    s_master_sfx_volume = (float)vol / 255.0f;
}

void platform_set_music_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 255) vol = 255;
    s_master_music_volume = (float)vol / 255.0f;
    if (s_midi_ready) fl_synth_set_gain(s_fl_synth, s_master_music_volume);
}

#endif /* __linux__ */
