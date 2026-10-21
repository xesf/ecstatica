/**
 * platforms/macos.m
 *
 * macOS backend (Cocoa). The software framebuffer is drawn through
 * CGBitmapContext into an NSView; the optional GL renderer uses a sibling view.
 */

#import <Cocoa/Cocoa.h>
#import <mach/mach_time.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include "platform.h"

/* ── Internal structures ── */

#define MAX_KEYS 256

@class ECView;
@class ECGLView;

struct platform_t {
    int fb_width;
    int fb_height;
    int render_width;
    int render_height;
    int scale;
    uint32_t *rgba_buffer;     /* fb_width * fb_height * 4 bytes (RGBA8888) */
    bool      quit_requested;

    /* GL context on its own sibling view (ECGLView); see there. */
    NSOpenGLContext *gl_ctx;
    ECGLView        *gl_view;
    bool             gl_active;

    bool key_state[MAX_KEYS];
    bool key_prev[MAX_KEYS];
    bool key_hit[MAX_KEYS];
    int  mouse_x;
    int  mouse_y;
    int  mouse_buttons;

    uint64_t start_mach;
    mach_timebase_info_data_t timebase;

    NSWindow *window;
    ECView   *view;
    NSApplication *app;
    id app_delegate;  /* strong ref — NSApp/NSWindow hold delegate weakly */
};

/* Owns the GL surface. Once an NSOpenGLContext is given an NSView, Quartz
 * drawing into that view stops compositing for good, so the context cannot go
 * on ECView. This sibling sits on top, hidden under the software renderer;
 * hitTest: returns nil and it never becomes first responder, so input is
 * untouched. Same idea as the GLX child window. */
@interface ECGLView : NSView
@end

@implementation ECGLView
- (BOOL)isOpaque                       { return YES; }
- (BOOL)acceptsFirstResponder          { return NO; }
- (NSView *)hitTest:(NSPoint)p         { (void)p; return nil; }
@end

/* ── Cocoa view that displays the RGBA buffer ── */

@interface ECView : NSView
@property (nonatomic, assign) platform_t *platform;
@end

@implementation ECView

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView      { return YES; }

- (void)drawRect:(NSRect)dirtyRect {
    platform_t *p = self.platform;
    if (!p || !p->rgba_buffer) return;
    /* GL owns the surface while hardware rendering is up. */
    if (p->gl_active) return;

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(
        p->rgba_buffer,
        p->fb_width, p->fb_height,
        8, p->fb_width * 4,
        cs,
        kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Host
    );
    CGColorSpaceRelease(cs);
    if (!ctx) return;

    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!img) return;

    NSGraphicsContext *gc = [NSGraphicsContext currentContext];
    CGContextRef drawCtx = [gc CGContext];
    CGContextSetInterpolationQuality(drawCtx, kCGInterpolationNone);
    CGContextDrawImage(drawCtx, NSRectToCGRect(self.bounds), img);
    CGImageRelease(img);
}

/* ── Keyboard mapping (macOS virtual keycode → platform keycode) ── */
static int macos_vk_to_pkey(unsigned short vk) {
    switch (vk) {
        case 53:  return PKEY_ESCAPE;
        case 36:  return PKEY_RETURN;
        case 49:  return PKEY_SPACE;
        case 126: return PKEY_UP;
        case 125: return PKEY_DOWN;
        case 123: return PKEY_LEFT;
        case 124: return PKEY_RIGHT;
        case 122: return PKEY_F1;
        case 120: return PKEY_F2;
        case 99:  return PKEY_F3;
        case 118: return PKEY_F4;
        case 96:  return PKEY_F5;
        case 97:  return PKEY_F6;
        case 98:  return PKEY_F7;
        case 100: return PKEY_F8;
        case 101: return PKEY_F9;
        case 109: return PKEY_F10;
        case 103: return PKEY_F11;
        case 111: return PKEY_F12;
        case 0:   return PKEY_A;
        case 2:   return PKEY_D;
        case 1:   return PKEY_S;
        case 13:  return PKEY_W;
        case 12:  return PKEY_Q;
        case 14:  return PKEY_E;
        case 5:   return PKEY_G;
        case 34:  return PKEY_I;
        case 35:  return PKEY_P;
        case 8:   return PKEY_C;
        case 46:  return PKEY_M;
        case 6:   return PKEY_Z;
        case 59:  return PKEY_LCTRL;
        case 58:  return PKEY_LALT;
        case 18:  return PKEY_1;
        case 19:  return PKEY_2;
        case 20:  return PKEY_3;
        case 83:  return PKEY_NUM1;
        case 84:  return PKEY_NUM2;
        case 85:  return PKEY_NUM3;
        case 86:  return PKEY_NUM4;
        case 87:  return PKEY_NUM5;
        case 88:  return PKEY_NUM6;
        case 89:  return PKEY_NUM7;
        case 91:  return PKEY_NUM8;
        case 92:  return PKEY_NUM9;
        case 11:  return PKEY_B;
        case 3:   return PKEY_F;
        case 4:   return PKEY_H;
        case 37:  return PKEY_L;
        case 45:  return PKEY_N;
        case 31:  return PKEY_O;
        case 15:  return PKEY_R;
        case 17:  return PKEY_T;
        case 9:   return PKEY_V;
        case 7:   return PKEY_X;
        case 48:  return PKEY_TAB;
        case 27:  return PKEY_MINUS;
        case 24:  return PKEY_EQUALS;
        case 33:  return PKEY_LBRACKET;
        case 30:  return PKEY_RBRACKET;
        case 43:  return PKEY_COMMA;
        case 47:  return PKEY_PERIOD;
        case 115: return PKEY_HOME;
        case 116: return PKEY_PGUP;
        case 119: return PKEY_END;
        case 121: return PKEY_PGDN;
        default:  return -1;
    }
}

- (BOOL)performKeyEquivalent:(NSEvent *)event {
    int k = macos_vk_to_pkey(event.keyCode);
    if (k >= 0 && k < MAX_KEYS) {
        if (event.type == NSEventTypeKeyDown) {
            if (!self.platform->key_state[k]) self.platform->key_hit[k] = true;
            self.platform->key_state[k] = true;
        }
        else if (event.type == NSEventTypeKeyUp)
            self.platform->key_state[k] = false;
        return YES;
    }
    return NO;
}

- (void)keyDown:(NSEvent *)event {
    int k = macos_vk_to_pkey(event.keyCode);
    if (k >= 0 && k < MAX_KEYS) {
        if (!self.platform->key_state[k]) self.platform->key_hit[k] = true;
        self.platform->key_state[k] = true;
    }
}

- (void)keyUp:(NSEvent *)event {
    int k = macos_vk_to_pkey(event.keyCode);
    if (k >= 0 && k < MAX_KEYS) self.platform->key_state[k] = false;
}

- (void)flagsChanged:(NSEvent *)event {
    NSEventModifierFlags flags = event.modifierFlags;
    bool cmd_was = self.platform->key_state[PKEY_LCMD];
    bool cmd_now = (flags & NSEventModifierFlagCommand) != 0;
    self.platform->key_state[PKEY_LCTRL]  = (flags & NSEventModifierFlagControl) != 0;
    self.platform->key_state[PKEY_LALT]   = (flags & NSEventModifierFlagOption)  != 0;
    self.platform->key_state[PKEY_LSHIFT] = (flags & NSEventModifierFlagShift)   != 0;
    self.platform->key_state[PKEY_LCMD]   = cmd_now;
    /* macOS drops keyUp for keys released while Cmd is held; clear them on
     * Cmd release. */
    if (cmd_was && !cmd_now) {
        for (int i = 0; i < MAX_KEYS; i++) {
            if (i == PKEY_LCTRL || i == PKEY_LALT || i == PKEY_LSHIFT || i == PKEY_LCMD)
                continue;
            self.platform->key_state[i] = false;
        }
    }
}

- (void)updateMouseWithEvent:(NSEvent *)event {
    platform_t *p = self.platform;
    NSPoint loc = [self convertPoint:event.locationInWindow fromView:nil];
    int rw = p->render_width;
    int rh = p->render_height;
    p->mouse_x = (int)(loc.x * rw / (p->fb_width * p->scale));
    p->mouse_y = rh - 1 - (int)(loc.y * rh / (p->fb_height * p->scale));
    if (p->mouse_x < 0) p->mouse_x = 0;
    if (p->mouse_y < 0) p->mouse_y = 0;
    if (p->mouse_x >= rw) p->mouse_x = rw - 1;
    if (p->mouse_y >= rh) p->mouse_y = rh - 1;
}

- (void)mouseDown:(NSEvent *)event {
    [self updateMouseWithEvent:event];
    self.platform->mouse_buttons |= PMOUSE_LEFT;
}
- (void)mouseUp:(NSEvent *)event {
    [self updateMouseWithEvent:event];
    self.platform->mouse_buttons &= ~PMOUSE_LEFT;
}
- (void)rightMouseDown:(NSEvent *)event {
    [self updateMouseWithEvent:event];
    self.platform->mouse_buttons |= PMOUSE_RIGHT;
}
- (void)rightMouseUp:(NSEvent *)event {
    [self updateMouseWithEvent:event];
    self.platform->mouse_buttons &= ~PMOUSE_RIGHT;
}
- (void)mouseMoved:(NSEvent *)event {
    [self updateMouseWithEvent:event];
}
- (void)mouseDragged:(NSEvent *)event {
    [self updateMouseWithEvent:event];
}
- (void)rightMouseDragged:(NSEvent *)event {
    [self updateMouseWithEvent:event];
}

@end

/* ── App delegate to handle window close ── */

@interface ECAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (nonatomic, assign) platform_t *platform;
@end

@implementation ECAppDelegate

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    if (self.platform) self.platform->quit_requested = true;
    return NSTerminateCancel;  /* let the game loop handle shutdown */
}

- (void)windowWillClose:(NSNotification *)notification {
    if (self.platform) self.platform->quit_requested = true;
}

@end

/* ── Platform API implementation ── */

platform_t *platform_init(const char *title, int fb_width, int fb_height, int scale) {
    platform_t *p = (platform_t *)calloc(1, sizeof(platform_t));
    if (!p) return NULL;

    p->fb_width      = fb_width;
    p->fb_height     = fb_height;
    p->render_width  = fb_width;
    p->render_height = fb_height;
    p->scale         = scale;
    p->rgba_buffer = (uint32_t *)calloc(fb_width * fb_height, sizeof(uint32_t));

    mach_timebase_info(&p->timebase);
    p->start_mach = mach_absolute_time();

    @autoreleasepool {
        p->app = [NSApplication sharedApplication];
        [p->app setActivationPolicy:NSApplicationActivationPolicyRegular];

        ECAppDelegate *delegate = [[ECAppDelegate alloc] init];
        delegate.platform = p;
        p->app_delegate = delegate;
        [p->app setDelegate:delegate];

        NSRect frame = NSMakeRect(100, 100, fb_width * scale, fb_height * scale);
        NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                  NSWindowStyleMaskMiniaturizable;
        p->window = [[NSWindow alloc] initWithContentRect:frame
                                                styleMask:style
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
        [p->window setTitle:[NSString stringWithUTF8String:title]];
        [p->window setAcceptsMouseMovedEvents:YES];
        [p->window setDelegate:delegate];

        ECView *view = [[ECView alloc] initWithFrame:frame];
        view.platform = p;
        p->view = view;

        [p->window setContentView:view];
        [p->window makeFirstResponder:view];
        [p->window makeKeyAndOrderFront:nil];

        [p->app activateIgnoringOtherApps:YES];

        /* Run just enough to show the window */
        NSEvent *event;
        while ((event = [p->app nextEventMatchingMask:NSEventMaskAny
                                            untilDate:nil
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES])) {
            [p->app sendEvent:event];
        }
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
    free(p->rgba_buffer);
    p->rgba_buffer = buf;
    p->fb_width = fb_w;

    @autoreleasepool {
        if (!p->window) return;
        NSRect content = [p->window contentRectForFrameRect:[p->window frame]];
        CGFloat ch = content.size.height;
        [p->window setContentSize:NSMakeSize(ch * fb_w / p->fb_height, ch)];
        if (p->gl_view) [p->gl_view setFrame:[p->view bounds]];
        if (p->gl_ctx) [p->gl_ctx update];
    }
}

/* ── Hardware rendering (OpenGL) ──────────────────────────── */

bool platform_gfx_create(platform_t *p) {
    if (!p || !p->view) return false;
    if (p->gl_ctx) return true;

    @autoreleasepool {
        /* 3.2 core caps GLSL at 150; 4.1 core is the smallest with 330. */
        NSOpenGLPixelFormatAttribute attrs[] = {
            NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion4_1Core,
            NSOpenGLPFADoubleBuffer,
            NSOpenGLPFAAccelerated,
            NSOpenGLPFAColorSize,   24,
            NSOpenGLPFAAlphaSize,    8,
            NSOpenGLPFADepthSize,   24,
            0
        };

        NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
        if (!pf) return false;

        NSOpenGLContext *ctx = [[NSOpenGLContext alloc] initWithFormat:pf shareContext:nil];
        if (!ctx) return false;

        ECGLView *glv = [[ECGLView alloc] initWithFrame:[p->view bounds]];
        [glv setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [glv setHidden:YES];
        [p->view addSubview:glv];

        /* Not covered by GL_SILENCE_DEPRECATION. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [glv setWantsBestResolutionOpenGLSurface:YES];
        [ctx setView:glv];
#pragma clang diagnostic pop

        GLint swap = 1;
        [ctx setValues:&swap forParameter:NSOpenGLContextParameterSwapInterval];

        p->gl_ctx  = ctx;
        p->gl_view = glv;
        /* The view stays hidden until platform_gfx_set_active. */
        p->gl_active = false;
    }
    return true;
}

/* The context keeps its own view's drawable; only visibility changes. */
void platform_gfx_set_active(platform_t *p, bool active) {
    if (!p || !p->gl_ctx || !p->gl_view) { if (p) p->gl_active = false; return; }
    if (p->gl_active == active) return;

    @autoreleasepool {
        [p->gl_view setHidden:!active];
        if (active) {
            [p->gl_ctx makeCurrentContext];
            [p->gl_ctx update];
        } else {
            [NSOpenGLContext clearCurrentContext];
            [p->view setNeedsDisplay:YES];
        }
    }
    p->gl_active = active;
}

void platform_gfx_make_current(platform_t *p) {
    if (!p || !p->gl_ctx) return;
    @autoreleasepool { [p->gl_ctx makeCurrentContext]; }
}

void platform_gfx_swap(platform_t *p) {
    if (!p || !p->gl_ctx) return;
    @autoreleasepool { [p->gl_ctx flushBuffer]; }
}

void platform_gfx_destroy(platform_t *p) {
    if (!p || !p->gl_ctx) return;
    @autoreleasepool {
        [NSOpenGLContext clearCurrentContext];
        [p->gl_ctx clearDrawable];
        [p->gl_view removeFromSuperview];
        p->gl_ctx  = nil;
        p->gl_view = nil;
        [p->view setNeedsDisplay:YES];
    }
    p->gl_active = false;
}

void platform_gfx_drawable_size(platform_t *p, int *w, int *h) {
    if (!p || !p->view) return;
    @autoreleasepool {
        NSView *src = p->gl_view ? (NSView *)p->gl_view : (NSView *)p->view;
        NSRect b = [src convertRectToBacking:[src bounds]];
        if (w) *w = (int)b.size.width;
        if (h) *h = (int)b.size.height;
    }
}

void *platform_gl_proc(const char *name) {
    /* OpenGL.framework exports every 4.1 symbol directly. */
    return dlsym(RTLD_DEFAULT, name);
}

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

    @autoreleasepool {
        [p->view setNeedsDisplay:YES];
    }
}

void platform_blit_rgba(platform_t *p, const uint8_t *framebuffer) {
    if (!p || !framebuffer) return;

    memcpy(p->rgba_buffer, framebuffer, p->fb_width * p->fb_height * 4);

    @autoreleasepool {
        [p->view setNeedsDisplay:YES];
    }
}

bool platform_pump_events(platform_t *p) {
    if (!p) return false;

    memcpy(p->key_prev, p->key_state, sizeof(p->key_prev));

    @autoreleasepool {
        NSEvent *event;
        while ((event = [p->app nextEventMatchingMask:NSEventMaskAny
                                            untilDate:nil
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES])) {
            [p->app sendEvent:event];
        }

        /* Only while Quartz owns the surface, or AppKit composites the GL
         * view on its own schedule and shows stale frames. */
        if (!p->gl_active)
            [p->view displayIfNeeded];
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
    static uint64_t s_start_mach = 0;
    static mach_timebase_info_data_t s_timebase = {0, 0};
    uint64_t ref_start;
    mach_timebase_info_data_t *tb;

    if (p) {
        ref_start = p->start_mach;
        tb = &p->timebase;
    } else {
        /* Fallback for NULL context: use static global time base */
        if (!s_timebase.denom) {
            mach_timebase_info(&s_timebase);
            s_start_mach = mach_absolute_time();
        }
        ref_start = s_start_mach;
        tb = &s_timebase;
    }
    uint64_t elapsed = mach_absolute_time() - ref_start;
    return (uint32_t)((elapsed * tb->numer) / (tb->denom * 1000000ULL));
}

void platform_delay(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

void platform_shutdown(platform_t *p) {
    if (!p) return;
    @autoreleasepool {
        if (p->window) {
            [p->window close];
            p->window = nil;
        }
    }
    if (p->rgba_buffer) {
        free(p->rgba_buffer);
        p->rgba_buffer = NULL;
    }
    free(p);
}

void platform_set_title(platform_t *p, const char *title) {
    if (!p || !title) return;
    @autoreleasepool {
        [p->window setTitle:[NSString stringWithUTF8String:title]];
    }
}

/* Gamepad — GameController.framework */
#import <GameController/GameController.h>

void platform_gamepad_poll(platform_t *p, platform_gamepad_state_t *state) {
    memset(state, 0, sizeof(*state));
    if (!p) return;

    NSArray<GCController *> *controllers = [GCController controllers];
    if (controllers.count == 0) return;

    GCController *gc = controllers[0];
    GCExtendedGamepad *gp = gc.extendedGamepad;
    if (!gp) return;

    state->connected = true;

    GCControllerDirectionPad *dp = gp.dpad;
    state->dpad_up    = dp.up.isPressed;
    state->dpad_down  = dp.down.isPressed;
    state->dpad_left  = dp.left.isPressed;
    state->dpad_right = dp.right.isPressed;

    GCControllerDirectionPad *ls = gp.leftThumbstick;
    state->left_x = (int16_t)(ls.xAxis.value * 32767);
    state->left_y = (int16_t)(ls.yAxis.value * 32767);

    GCControllerDirectionPad *rs = gp.rightThumbstick;
    state->right_x = (int16_t)(rs.xAxis.value * 32767);
    state->right_y = (int16_t)(rs.yAxis.value * 32767);

    state->btn_south = gp.buttonA.isPressed;
    state->btn_east  = gp.buttonB.isPressed;
    state->btn_west  = gp.buttonX.isPressed;
    state->btn_north = gp.buttonY.isPressed;

    state->btn_lb = gp.leftShoulder.isPressed;
    state->btn_rb = gp.rightShoulder.isPressed;
    state->btn_lt = gp.leftTrigger.value > 0.3f;
    state->btn_rt = gp.rightTrigger.value > 0.3f;

    state->btn_start  = gp.buttonMenu.isPressed;
    if (@available(macOS 10.15, *))
        state->btn_select = gp.buttonOptions.isPressed;
    if (@available(macOS 12.1, *)) {
        state->btn_lstick = gp.leftThumbstickButton.isPressed;
        state->btn_rstick = gp.rightThumbstickButton.isPressed;
    }
}

/* Audio backend — AudioUnit output + software mixer */

#import <AudioUnit/AudioUnit.h>
#import <AudioToolbox/AudioToolbox.h>
#include <pthread.h>

#define AUDIO_MAX_VOICES 16
#define AUDIO_OUT_RATE   44100
#define AUDIO_OUT_CH     2

typedef struct {
    const uint8_t *data;   /* unsigned 8-bit PCM, mono */
    int   length;          /* bytes */
    int   rate;            /* source sample rate, Hz */
    int   volume;          /* 0..127 */
    int   pan;             /* -128..127 */
    double pos;            /* current playback position in source samples (float) */
    bool  active;
    bool  loop;
} audio_voice_t;

static audio_voice_t s_voices[AUDIO_MAX_VOICES];
static pthread_mutex_t s_audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static AudioUnit s_audio_unit;
static bool s_audio_ready = false;
static float s_master_sfx_volume = 0.9f;
static float s_master_music_volume = 0.8f;

static OSStatus audio_render_cb(void *inRefCon,
                                AudioUnitRenderActionFlags *ioActionFlags,
                                const AudioTimeStamp *inTimeStamp,
                                UInt32 inBusNumber,
                                UInt32 inNumberFrames,
                                AudioBufferList *ioData) {
    (void)inRefCon; (void)ioActionFlags; (void)inTimeStamp; (void)inBusNumber;

    float *out = (float *)ioData->mBuffers[0].mData;
    memset(out, 0, sizeof(float) * inNumberFrames * AUDIO_OUT_CH);

    pthread_mutex_lock(&s_audio_mutex);
    for (int v = 0; v < AUDIO_MAX_VOICES; ++v) {
        audio_voice_t *voice = &s_voices[v];
        if (!voice->active) continue;

        double step = (double)voice->rate / (double)AUDIO_OUT_RATE;
        float db    = (float)(voice->volume - 127) * 0.25f;
        float vol   = powf(10.0f, db / 20.0f) * s_master_sfx_volume;
        float pan_norm = (float)voice->pan / 127.0f;   /* -1..+1 */
        float l_gain = vol * (pan_norm > 0 ? (1.0f - pan_norm) : 1.0f);
        float r_gain = vol * (pan_norm < 0 ? (1.0f + pan_norm) : 1.0f);

        for (UInt32 f = 0; f < inNumberFrames; ++f) {
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
            out[f * AUDIO_OUT_CH + 0] += s * l_gain;
            out[f * AUDIO_OUT_CH + 1] += s * r_gain;
            voice->pos += step;
        }
    }
    pthread_mutex_unlock(&s_audio_mutex);

    for (UInt32 i = 0; i < inNumberFrames * AUDIO_OUT_CH; ++i) {
        if (out[i] >  1.0f) out[i] =  1.0f;
        if (out[i] < -1.0f) out[i] = -1.0f;
    }
    return noErr;
}

void platform_audio_init(void) {
    if (s_audio_ready) return;

    AudioComponentDescription desc = {
        .componentType         = kAudioUnitType_Output,
        .componentSubType      = kAudioUnitSubType_DefaultOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) return;
    if (AudioComponentInstanceNew(comp, &s_audio_unit) != noErr) return;

    AudioStreamBasicDescription fmt = {
        .mSampleRate       = AUDIO_OUT_RATE,
        .mFormatID         = kAudioFormatLinearPCM,
        .mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mFramesPerPacket  = 1,
        .mChannelsPerFrame = AUDIO_OUT_CH,
        .mBitsPerChannel   = 32,
        .mBytesPerPacket   = 4 * AUDIO_OUT_CH,
        .mBytesPerFrame    = 4 * AUDIO_OUT_CH,
    };
    AudioUnitSetProperty(s_audio_unit, kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));

    AURenderCallbackStruct cb = { .inputProc = audio_render_cb };
    AudioUnitSetProperty(s_audio_unit, kAudioUnitProperty_SetRenderCallback,
                         kAudioUnitScope_Input, 0, &cb, sizeof(cb));

    if (AudioUnitInitialize(s_audio_unit) != noErr) return;
    if (AudioOutputUnitStart(s_audio_unit) != noErr)  return;
    s_audio_ready = true;
}

int platform_audio_play_pcm(const void *data, int length, int rate,
                            int volume, int pan, bool loop) {
    if (!s_audio_ready || !data || length <= 0) return -1;
    if (rate <= 0) rate = 22050;

    pthread_mutex_lock(&s_audio_mutex);
    int slot = -1;
    for (int i = 0; i < AUDIO_MAX_VOICES; ++i) {
        if (!s_voices[i].active) { slot = i; break; }
    }
    if (slot < 0) slot = 0;   /* steal voice 0 */
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
    if (!s_audio_ready) return;
    AudioOutputUnitStop(s_audio_unit);
    AudioUnitUninitialize(s_audio_unit);
    AudioComponentInstanceDispose(s_audio_unit);
    s_audio_ready = false;
}

/* ── MIDI music playback via AVMIDIPlayer + built-in GM synth ── */
#import <AVFoundation/AVFoundation.h>

static AVMIDIPlayer *s_midi_player = nil;
static NSData *s_midi_data = nil;
static bool s_midi_loop = false;

int platform_midi_play(const void *smf_data, int length, bool loop) {
    if (!smf_data || length <= 0) return -1;

    platform_midi_stop();

    NSData *data = [NSData dataWithBytes:smf_data length:length];
    NSError *err = nil;
    AVMIDIPlayer *player = [[AVMIDIPlayer alloc] initWithData:data
                                                soundBankURL:nil
                                                       error:&err];
    if (!player || err) {
        NSLog(@"[MIDI] init failed: %@", err);
        return -1;
    }
    [player prepareToPlay];
    s_midi_player = player;
    s_midi_data = data;
    s_midi_loop = loop;
    [player play:^{
        /* Called on completion. Loop if requested. */
        if (s_midi_loop && s_midi_player == player) {
            player.currentPosition = 0.0;
            [player play:nil];
        }
    }];
    return 0;
}

void platform_midi_stop(void) {
    if (s_midi_player) {
        [s_midi_player stop];
        s_midi_player = nil;
        s_midi_data = nil;
    }
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
}
