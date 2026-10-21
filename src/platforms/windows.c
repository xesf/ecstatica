#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
/* No XInput on Win9x or in Open Watcom; that target uses the winmm joystick
 * API. */
#ifndef __WATCOMC__
#include <xinput.h>
#endif
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
/* gl.h's typedefs collide with gl_loader.h's; they never share a translation
 * unit. The Win9x/Watcom build is software only and leaves ECS_ENABLE_GL
 * undefined. */
#ifdef ECS_ENABLE_GL
#include <GL/gl.h>
#endif
#include "platform.h"

#ifndef __WATCOMC__
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "xinput.lib")
#endif

#define MAX_KEYS 256

struct platform_t {
    int fb_width;
    int fb_height;
    int render_width;
    int render_height;
    int scale;
    uint32_t *rgba_buffer;
    bool quit_requested;

    bool key_state[MAX_KEYS];
    bool key_hit[MAX_KEYS];
    bool key_prev[MAX_KEYS];
    int  mouse_x;
    int  mouse_y;
    int  mouse_buttons;

    LARGE_INTEGER start_ticks;
    LARGE_INTEGER freq;

    HWND hwnd;
    BITMAPINFO bmi;

    bool  fullscreen;
    bool  mode_changed;      /* display mode was switched, restore on the way out */
    DWORD saved_style;
    RECT  saved_rect;        /* window rect to come back to */

    /* Destination rect in the client area, letterboxed when aspects differ. */
    int dst_x, dst_y, dst_w, dst_h;

    /* The GL context uses the game window's own DC: a pixel format can be set
     * on an existing window. */
    HDC   gl_dc;
    HGLRC gl_rc;
    bool  gl_active;
    HMODULE gl_lib;
};

static platform_t *s_platform = NULL;

/* Largest fb-aspect rect that fits the client area, centred. Per frame, so it
 * cannot go stale after a resize or mode change. */
static void compute_dest_rect(platform_t *p) {
    RECT rc;
    int cw, ch, sw, sh;

    if (!GetClientRect(p->hwnd, &rc)) {
        p->dst_x = p->dst_y = 0;
        p->dst_w = p->fb_width * p->scale;
        p->dst_h = p->fb_height * p->scale;
        return;
    }

    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) { cw = p->fb_width; ch = p->fb_height; }

    sw = cw;
    sh = (int)((int64_t)cw * p->fb_height / p->fb_width);
    if (sh > ch) {
        sh = ch;
        sw = (int)((int64_t)ch * p->fb_width / p->fb_height);
    }

    p->dst_x = (cw - sw) / 2;
    p->dst_y = (ch - sh) / 2;
    p->dst_w = sw;
    p->dst_h = sh;
}

/* Alt+Enter fullscreen. Switches the display mode to the framebuffer size
 * when available so the blit stays 1:1; otherwise goes borderless and
 * stretches. */
static void set_fullscreen(platform_t *p, bool on) {
    if (!p || !p->hwnd || p->fullscreen == on) return;

    if (on) {
        DEVMODEA dm;

        p->saved_style = (DWORD)GetWindowLongA(p->hwnd, GWL_STYLE);
        GetWindowRect(p->hwnd, &p->saved_rect);

        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
            dm.dmPelsWidth  = (DWORD)p->fb_width;
            dm.dmPelsHeight = (DWORD)p->fb_height;
            dm.dmFields     = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
            if (ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN) == DISP_CHANGE_SUCCESSFUL)
                p->mode_changed = true;
        }

        SetWindowLongA(p->hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(p->hwnd, HWND_TOP, 0, 0,
                     GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        p->fullscreen = true;
    } else {
        if (p->mode_changed) {
            ChangeDisplaySettingsA(NULL, 0);
            p->mode_changed = false;
        }
        SetWindowLongA(p->hwnd, GWL_STYLE, p->saved_style | WS_VISIBLE);
        SetWindowPos(p->hwnd, HWND_NOTOPMOST,
                     p->saved_rect.left, p->saved_rect.top,
                     p->saved_rect.right - p->saved_rect.left,
                     p->saved_rect.bottom - p->saved_rect.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        p->fullscreen = false;
    }

    InvalidateRect(p->hwnd, NULL, TRUE);
}

static int win32_scancode(LPARAM lParam, WPARAM wParam) {
    int scancode = (lParam >> 16) & 0xFF;
    bool extended = (lParam >> 24) & 1;

    if (extended) {
        switch (scancode) {
            case 0x48: return 0xC8;  /* up */
            case 0x50: return 0xD0;  /* down */
            case 0x4B: return 0xCB;  /* left */
            case 0x4D: return 0xCD;  /* right */
            default:   return scancode | 0x80;
        }
    }
    return scancode;
}

static void update_mouse_pos(platform_t *p, LPARAM lParam) {
    int raw_x = (short)LOWORD(lParam);
    int raw_y = (short)HIWORD(lParam);
    int rw = p->render_width;
    int rh = p->render_height;

    /* Map through the picture rect, not the window. */
    compute_dest_rect(p);
    if (p->dst_w <= 0 || p->dst_h <= 0) return;

    p->mouse_x = (raw_x - p->dst_x) * rw / p->dst_w;
    p->mouse_y = (raw_y - p->dst_y) * rh / p->dst_h;

    if (p->mouse_x < 0) p->mouse_x = 0;
    if (p->mouse_y < 0) p->mouse_y = 0;
    if (p->mouse_x >= rw) p->mouse_x = rw - 1;
    if (p->mouse_y >= rh) p->mouse_y = rh - 1;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    platform_t *p = s_platform;
    if (!p) return DefWindowProcA(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_CLOSE:
            p->quit_requested = true;
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            int sc;
            /* Bit 29 is Alt. Swallowed, so Enter does not reach an open menu. */
            if (msg == WM_SYSKEYDOWN && wParam == VK_RETURN && (lParam & (1 << 29))) {
                set_fullscreen(p, !p->fullscreen);
                return 0;
            }
            sc = win32_scancode(lParam, wParam);
            if (sc >= 0 && sc < MAX_KEYS) {
                if (!p->key_state[sc]) p->key_hit[sc] = true;
                p->key_state[sc] = true;
            }
            return 0;
        }

        case WM_KEYUP:
        case WM_SYSKEYUP: {
            int sc = win32_scancode(lParam, wParam);
            if (sc >= 0 && sc < MAX_KEYS) p->key_state[sc] = false;
            return 0;
        }

        case WM_MOUSEMOVE:
            update_mouse_pos(p, lParam);
            return 0;

        case WM_LBUTTONDOWN:
            update_mouse_pos(p, lParam);
            p->mouse_buttons |= PMOUSE_LEFT;
            SetCapture(hwnd);
            return 0;

        case WM_LBUTTONUP:
            update_mouse_pos(p, lParam);
            p->mouse_buttons &= ~PMOUSE_LEFT;
            if (!(p->mouse_buttons & (PMOUSE_RIGHT | PMOUSE_MIDDLE)))
                ReleaseCapture();
            return 0;

        case WM_RBUTTONDOWN:
            update_mouse_pos(p, lParam);
            p->mouse_buttons |= PMOUSE_RIGHT;
            SetCapture(hwnd);
            return 0;

        case WM_RBUTTONUP:
            update_mouse_pos(p, lParam);
            p->mouse_buttons &= ~PMOUSE_RIGHT;
            if (!(p->mouse_buttons & (PMOUSE_LEFT | PMOUSE_MIDDLE)))
                ReleaseCapture();
            return 0;

        case WM_DESTROY:
            p->quit_requested = true;
            return 0;

        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

platform_t *platform_init(const char *title, int fb_width, int fb_height, int scale) {
    platform_t *p = (platform_t *)calloc(1, sizeof(platform_t));
    if (!p) return NULL;

    p->fb_width      = fb_width;
    p->fb_height     = fb_height;
    p->render_width  = fb_width;
    p->render_height = fb_height;
    p->scale         = scale;
    p->rgba_buffer   = (uint32_t *)calloc(fb_width * fb_height, sizeof(uint32_t));

    QueryPerformanceFrequency(&p->freq);
    QueryPerformanceCounter(&p->start_ticks);
    /* The frame wait sleeps between 70 Hz ticks; the default 15.6 ms timer
     * would oversleep past one. */
    timeBeginPeriod(1);

    s_platform = p;

    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance      = GetModuleHandleA(NULL);
    wc.lpszClassName  = "EcstaticaWnd";
    wc.hCursor        = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    int win_w = fb_width * scale;
    int win_h = fb_height * scale;

    /* Assigned: Open Watcom only accepts constant aggregate initialisers. */
    RECT rc;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    rc.left = 0; rc.top = 0; rc.right = win_w; rc.bottom = win_h;
    AdjustWindowRect(&rc, style, FALSE);

    p->hwnd = CreateWindowExA(
        0, "EcstaticaWnd", title, style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, GetModuleHandleA(NULL), NULL
    );

    memset(&p->bmi, 0, sizeof(p->bmi));
    p->bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    p->bmi.bmiHeader.biWidth       = fb_width;
    p->bmi.bmiHeader.biHeight      = -fb_height;  /* top-down */
    p->bmi.bmiHeader.biPlanes      = 1;
    p->bmi.bmiHeader.biBitCount    = 32;
    p->bmi.bmiHeader.biCompression = BI_RGB;

    ShowWindow(p->hwnd, SW_SHOW);
    UpdateWindow(p->hwnd);

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
    int fb_w;
    uint32_t *buf;

    if (!p) return;
    p->render_width  = w;
    p->render_height = h;

    fb_w = (h == p->fb_height && w * 3 > h * 4) ? w : p->fb_height * 4 / 3;
    if (fb_w == p->fb_width) return;
    buf = (uint32_t *)calloc((size_t)fb_w * p->fb_height, sizeof(uint32_t));
    if (!buf) return;
    free(p->rgba_buffer);
    p->rgba_buffer = buf;
    p->fb_width = fb_w;
    p->bmi.bmiHeader.biWidth = fb_w;

    /* Fullscreen letterboxes into whatever the client area is. */
    if (p->hwnd && !p->fullscreen) {
        RECT rc;
        DWORD style = (DWORD)GetWindowLongA(p->hwnd, GWL_STYLE);
        rc.left = 0; rc.top = 0;
        rc.right = fb_w * p->scale; rc.bottom = p->fb_height * p->scale;
        AdjustWindowRect(&rc, style, FALSE);
        SetWindowPos(p->hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* Put p->rgba_buffer on the screen, letterboxed into the client area. */
static void present(platform_t *p) {
    HDC hdc = GetDC(p->hwnd);
    if (!hdc) return;

    compute_dest_rect(p);

    /* Only the bars, only when present: a full repaint flickers. */
    if (p->dst_x > 0 || p->dst_y > 0) {
        RECT rc;
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        if (GetClientRect(p->hwnd, &rc)) {
            RECT bar;
            if (p->dst_y > 0) {
                bar = rc; bar.bottom = p->dst_y;                    FillRect(hdc, &bar, black);
                bar = rc; bar.top    = p->dst_y + p->dst_h;         FillRect(hdc, &bar, black);
            }
            if (p->dst_x > 0) {
                bar = rc; bar.right  = p->dst_x;                    FillRect(hdc, &bar, black);
                bar = rc; bar.left   = p->dst_x + p->dst_w;         FillRect(hdc, &bar, black);
            }
        }
    }

    StretchDIBits(
        hdc,
        p->dst_x, p->dst_y, p->dst_w, p->dst_h,
        0, 0, p->fb_width, p->fb_height,
        p->rgba_buffer, &p->bmi,
        DIB_RGB_COLORS, SRCCOPY
    );
    ReleaseDC(p->hwnd, hdc);
}

/* ── Hardware rendering (WGL) ─────────────────────────────── */
#ifdef ECS_ENABLE_GL

typedef HGLRC (WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);
typedef BOOL  (WINAPI *PFN_wglSwapIntervalEXT)(int);

#define ECS_WGL_CONTEXT_MAJOR_VERSION_ARB  0x2091
#define ECS_WGL_CONTEXT_MINOR_VERSION_ARB  0x2092
#define ECS_WGL_CONTEXT_PROFILE_MASK_ARB   0x9126
#define ECS_WGL_CONTEXT_CORE_PROFILE_BIT   0x00000001

bool platform_gfx_create(platform_t *p) {
    if (!p || !p->hwnd) return false;
    if (p->gl_rc) return true;

    HDC dc = GetDC(p->hwnd);
    if (!dc) return false;

    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize      = sizeof(pfd);
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int fmt = ChoosePixelFormat(dc, &pfd);
    if (!fmt || !SetPixelFormat(dc, fmt, &pfd)) {
        ReleaseDC(p->hwnd, dc);
        return false;
    }

    /* A legacy context is needed to resolve wglCreateContextAttribsARB, the
     * only way to get a core profile. */
    HGLRC legacy = wglCreateContext(dc);
    if (!legacy) { ReleaseDC(p->hwnd, dc); return false; }
    wglMakeCurrent(dc, legacy);

    PFN_wglCreateContextAttribsARB create_ctx =
        (PFN_wglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");

    HGLRC core = NULL;
    if (create_ctx) {
        int attrs[] = {
            ECS_WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
            ECS_WGL_CONTEXT_MINOR_VERSION_ARB, 3,
            ECS_WGL_CONTEXT_PROFILE_MASK_ARB,  ECS_WGL_CONTEXT_CORE_PROFILE_BIT,
            0
        };
        core = create_ctx(dc, NULL, attrs);
    }

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(legacy);

    if (!core) {
        ReleaseDC(p->hwnd, dc);
        return false;
    }

    wglMakeCurrent(dc, core);

    PFN_wglSwapIntervalEXT swap_interval =
        (PFN_wglSwapIntervalEXT)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap_interval) swap_interval(1);

    /* opengl32.dll exports only 1.1; some drivers answer only one of the two
     * lookups, so platform_gl_proc tries both. */
    p->gl_lib = LoadLibraryA("opengl32.dll");
    p->gl_dc  = dc;
    p->gl_rc  = core;
    /* Not active yet — see platform_gfx_set_active. */
    return true;
}

void platform_gfx_set_active(platform_t *p, bool active) {
    if (!p || !p->gl_rc) { if (p) p->gl_active = false; return; }
    if (p->gl_active == active) return;

    if (active) {
        wglMakeCurrent(p->gl_dc, p->gl_rc);
    } else {
        /* GDI and GL share the DC; release it for StretchDIBits. */
        wglMakeCurrent(NULL, NULL);
        if (p->hwnd) InvalidateRect(p->hwnd, NULL, TRUE);
    }
    p->gl_active = active;
}

void platform_gfx_make_current(platform_t *p) {
    if (!p || !p->gl_rc) return;
    wglMakeCurrent(p->gl_dc, p->gl_rc);
}

void platform_gfx_swap(platform_t *p) {
    if (!p || !p->gl_dc) return;
    SwapBuffers(p->gl_dc);
}

void platform_gfx_destroy(platform_t *p) {
    if (!p || !p->gl_rc) return;
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(p->gl_rc);
    ReleaseDC(p->hwnd, p->gl_dc);
    if (p->gl_lib) { FreeLibrary(p->gl_lib); p->gl_lib = NULL; }
    p->gl_rc     = NULL;
    p->gl_dc     = NULL;
    p->gl_active = false;
    InvalidateRect(p->hwnd, NULL, TRUE);
}

void platform_gfx_drawable_size(platform_t *p, int *w, int *h) {
    if (!p || !p->hwnd) return;
    RECT r;
    if (!GetClientRect(p->hwnd, &r)) return;
    if (w) *w = r.right  - r.left;
    if (h) *h = r.bottom - r.top;
}

void *platform_gl_proc(const char *name) {
    void *fn = (void *)wglGetProcAddress(name);
    /* wglGetProcAddress returns these sentinels, not NULL, for 1.1 functions. */
    if (fn == (void *)0 || fn == (void *)1 || fn == (void *)2 ||
        fn == (void *)3 || fn == (void *)-1) {
        HMODULE m = GetModuleHandleA("opengl32.dll");
        fn = m ? (void *)GetProcAddress(m, name) : NULL;
    }
    return fn;
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

    present(p);
}

void platform_blit_rgba(platform_t *p, const uint8_t *framebuffer) {
    if (!p || !framebuffer) return;

    memcpy(p->rgba_buffer, framebuffer, p->fb_width * p->fb_height * 4);

    present(p);
}

bool platform_pump_events(platform_t *p) {
    if (!p) return false;

    memcpy(p->key_prev, p->key_state, sizeof(p->key_prev));

    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
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
    static LARGE_INTEGER s_start = {0};
    static LARGE_INTEGER s_freq = {0};
    LARGE_INTEGER now;
    LARGE_INTEGER *ref_start;
    LARGE_INTEGER *freq;

    if (p) {
        ref_start = &p->start_ticks;
        freq = &p->freq;
    } else {
        if (!s_freq.QuadPart) {
            QueryPerformanceFrequency(&s_freq);
            QueryPerformanceCounter(&s_start);
        }
        ref_start = &s_start;
        freq = &s_freq;
    }

    QueryPerformanceCounter(&now);
    return (uint32_t)((now.QuadPart - ref_start->QuadPart) * 1000 / freq->QuadPart);
}

void platform_delay(uint32_t ms) {
    Sleep(ms);
}

void platform_shutdown(platform_t *p) {
    if (!p) return;
    /* Restore the desktop mode after fullscreen. */
    if (p->mode_changed) {
        ChangeDisplaySettingsA(NULL, 0);
        p->mode_changed = false;
    }
    if (p->hwnd) {
        DestroyWindow(p->hwnd);
        p->hwnd = NULL;
    }
    if (p->rgba_buffer) {
        free(p->rgba_buffer);
        p->rgba_buffer = NULL;
    }
    timeEndPeriod(1);
    if (s_platform == p) s_platform = NULL;
    free(p);
}

void platform_set_title(platform_t *p, const char *title) {
    if (!p || !title) return;
    SetWindowTextA(p->hwnd, title);
}

/* Gamepad — XInput, or the winmm joystick API where XInput does not exist */

#ifdef __WATCOMC__

/* To positive-up. -(-32768) does not fit an int16. */
static int16_t neg_axis(int v) {
    v = -v;
    return (int16_t)(v > 32767 ? 32767 : (v < -32767 ? -32767 : v));
}

/* Win9x: joyGetPosEx axes are normalised against the driver's declared range,
 * and Y is inverted to match XInput. */
void platform_gamepad_poll(platform_t *p, platform_gamepad_state_t *state) {
    JOYCAPS  caps;
    JOYINFOEX ji;
    DWORD    b;

    memset(state, 0, sizeof(*state));
    (void)p;

    if (joyGetDevCaps(JOYSTICKID1, &caps, sizeof(caps)) != JOYERR_NOERROR)
        return;

    memset(&ji, 0, sizeof(ji));
    ji.dwSize  = sizeof(ji);
    ji.dwFlags = JOY_RETURNALL;
    if (joyGetPosEx(JOYSTICKID1, &ji) != JOYERR_NOERROR)
        return;

    state->connected = true;

    #define AXIS(v, lo, hi) \
        ((int16_t)((hi) > (lo) ? (((int)(v) - (int)(lo)) * 65535 / ((int)(hi) - (int)(lo)) - 32768) : 0))

    state->left_x  =  AXIS(ji.dwXpos, caps.wXmin, caps.wXmax);
    state->left_y  = neg_axis(AXIS(ji.dwYpos, caps.wYmin, caps.wYmax));
    state->right_x =  AXIS(ji.dwRpos, caps.wRmin, caps.wRmax);
    state->right_y = neg_axis(AXIS(ji.dwUpos, caps.wUmin, caps.wUmax));
    #undef AXIS

    /* POV hat, in hundredths of a degree; 0xFFFF means centred. */
    if (ji.dwPOV != JOY_POVCENTERED && ji.dwPOV <= 35900) {
        state->dpad_up    = (ji.dwPOV > 27000 || ji.dwPOV <  9000);
        state->dpad_right = (ji.dwPOV >     0 && ji.dwPOV < 18000);
        state->dpad_down  = (ji.dwPOV >  9000 && ji.dwPOV < 27000);
        state->dpad_left  = (ji.dwPOV > 18000);
    }

    b = ji.dwButtons;
    state->btn_south  = (b & 0x01) != 0;
    state->btn_east   = (b & 0x02) != 0;
    state->btn_west   = (b & 0x04) != 0;
    state->btn_north  = (b & 0x08) != 0;
    state->btn_lb     = (b & 0x10) != 0;
    state->btn_rb     = (b & 0x20) != 0;
    state->btn_lt     = (b & 0x40) != 0;
    state->btn_rt     = (b & 0x80) != 0;
    state->btn_select = (b & 0x100) != 0;
    state->btn_start  = (b & 0x200) != 0;
    state->btn_lstick = (b & 0x400) != 0;
    state->btn_rstick = (b & 0x800) != 0;
}

#else

void platform_gamepad_poll(platform_t *p, platform_gamepad_state_t *state) {
    memset(state, 0, sizeof(*state));
    (void)p;

    XINPUT_STATE xs;
    if (XInputGetState(0, &xs) != ERROR_SUCCESS) return;

    state->connected = true;
    WORD b = xs.Gamepad.wButtons;

    state->dpad_up    = (b & XINPUT_GAMEPAD_DPAD_UP) != 0;
    state->dpad_down  = (b & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
    state->dpad_left  = (b & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
    state->dpad_right = (b & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;

    state->left_x  = xs.Gamepad.sThumbLX;
    state->left_y  = xs.Gamepad.sThumbLY;
    state->right_x = xs.Gamepad.sThumbRX;
    state->right_y = xs.Gamepad.sThumbRY;

    state->btn_south  = (b & XINPUT_GAMEPAD_A) != 0;
    state->btn_east   = (b & XINPUT_GAMEPAD_B) != 0;
    state->btn_west   = (b & XINPUT_GAMEPAD_X) != 0;
    state->btn_north  = (b & XINPUT_GAMEPAD_Y) != 0;
    state->btn_lb     = (b & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
    state->btn_rb     = (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    state->btn_lt     = xs.Gamepad.bLeftTrigger > 76;
    state->btn_rt     = xs.Gamepad.bRightTrigger > 76;
    state->btn_start  = (b & XINPUT_GAMEPAD_START) != 0;
    state->btn_select = (b & XINPUT_GAMEPAD_BACK) != 0;
    state->btn_lstick = (b & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
    state->btn_rstick = (b & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
}

#endif /* __WATCOMC__ */

/* Audio backend -- waveOut + software mixer */

#define AUDIO_MAX_VOICES 16
#define AUDIO_OUT_RATE   44100
#define AUDIO_OUT_CH     2
#define AUDIO_BUF_FRAMES 2048
#define AUDIO_NUM_BUFS   4

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
static CRITICAL_SECTION s_audio_cs;
static HWAVEOUT s_wave_out = NULL;
static WAVEHDR s_wave_hdrs[AUDIO_NUM_BUFS];
static int16_t *s_wave_bufs[AUDIO_NUM_BUFS];
static bool s_audio_ready = false;
static float s_master_sfx_volume = 0.9f;
static float s_master_music_volume = 0.8f;

static void audio_fill_buffer(int16_t *buf, int frames) {
    float *mix = (float *)calloc(frames * AUDIO_OUT_CH, sizeof(float));

    EnterCriticalSection(&s_audio_cs);
    for (int v = 0; v < AUDIO_MAX_VOICES; ++v) {
        audio_voice_t *voice = &s_voices[v];
        if (!voice->active) continue;

        double step = (double)voice->rate / (double)AUDIO_OUT_RATE;
        float db    = (float)(voice->volume - 127) * 0.25f;
        /* pow, not powf — Open Watcom's math.h has no float variant. */
        float vol   = (float)pow(10.0, (double)db / 20.0) * s_master_sfx_volume;
        float pan_norm = (float)voice->pan / 127.0f;
        float l_gain = vol * (pan_norm > 0 ? (1.0f - pan_norm) : 1.0f);
        float r_gain = vol * (pan_norm < 0 ? (1.0f + pan_norm) : 1.0f);

        for (int f = 0; f < frames; ++f) {
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
    LeaveCriticalSection(&s_audio_cs);

    for (int i = 0; i < frames * AUDIO_OUT_CH; ++i) {
        float v = mix[i];
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        buf[i] = (int16_t)(v * 32767.0f);
    }

    free(mix);
}

static void CALLBACK wave_out_proc(HWAVEOUT hwo, UINT uMsg,
                                   DWORD_PTR dwInstance,
                                   DWORD_PTR dwParam1,
                                   DWORD_PTR dwParam2) {
    (void)hwo; (void)dwInstance; (void)dwParam2;
    if (uMsg != WOM_DONE) return;

    WAVEHDR *hdr = (WAVEHDR *)dwParam1;
    audio_fill_buffer((int16_t *)hdr->lpData, AUDIO_BUF_FRAMES);
    waveOutWrite(s_wave_out, hdr, sizeof(WAVEHDR));
}

void platform_audio_init(void) {
    if (s_audio_ready) return;

    InitializeCriticalSection(&s_audio_cs);

    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels        = AUDIO_OUT_CH;
    wfx.nSamplesPerSec   = AUDIO_OUT_RATE;
    wfx.wBitsPerSample   = 16;
    wfx.nBlockAlign      = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec  = wfx.nSamplesPerSec * wfx.nBlockAlign;

    if (waveOutOpen(&s_wave_out, WAVE_MAPPER, &wfx,
                    (DWORD_PTR)wave_out_proc, 0,
                    CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        return;
    }

    int buf_bytes = AUDIO_BUF_FRAMES * AUDIO_OUT_CH * sizeof(int16_t);
    for (int i = 0; i < AUDIO_NUM_BUFS; ++i) {
        s_wave_bufs[i] = (int16_t *)calloc(1, buf_bytes);
        memset(&s_wave_hdrs[i], 0, sizeof(WAVEHDR));
        s_wave_hdrs[i].lpData         = (LPSTR)s_wave_bufs[i];
        s_wave_hdrs[i].dwBufferLength = buf_bytes;
        waveOutPrepareHeader(s_wave_out, &s_wave_hdrs[i], sizeof(WAVEHDR));
        audio_fill_buffer(s_wave_bufs[i], AUDIO_BUF_FRAMES);
        waveOutWrite(s_wave_out, &s_wave_hdrs[i], sizeof(WAVEHDR));
    }

    s_audio_ready = true;
}

int platform_audio_play_pcm(const void *data, int length, int rate,
                            int volume, int pan, bool loop) {
    if (!s_audio_ready || !data || length <= 0) return -1;
    if (rate <= 0) rate = 22050;

    EnterCriticalSection(&s_audio_cs);
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
    LeaveCriticalSection(&s_audio_cs);
    return slot;
}

void platform_audio_stop_voice(int slot) {
    if (slot < 0 || slot >= AUDIO_MAX_VOICES) return;
    EnterCriticalSection(&s_audio_cs);
    s_voices[slot].active = false;
    LeaveCriticalSection(&s_audio_cs);
}

void platform_audio_stop_all(void) {
    EnterCriticalSection(&s_audio_cs);
    for (int i = 0; i < AUDIO_MAX_VOICES; ++i) s_voices[i].active = false;
    LeaveCriticalSection(&s_audio_cs);
}

void platform_audio_shutdown(void) {
    if (!s_audio_ready) return;

    waveOutReset(s_wave_out);
    for (int i = 0; i < AUDIO_NUM_BUFS; ++i) {
        waveOutUnprepareHeader(s_wave_out, &s_wave_hdrs[i], sizeof(WAVEHDR));
        free(s_wave_bufs[i]);
        s_wave_bufs[i] = NULL;
    }
    waveOutClose(s_wave_out);
    s_wave_out = NULL;
    DeleteCriticalSection(&s_audio_cs);
    s_audio_ready = false;
}

/* ── MIDI via mciSendString (needs an SMF file on disk) ───────── */

static bool s_midi_active = false;
static char s_midi_temp_path[MAX_PATH];
static bool s_midi_loop = false;

int platform_midi_play(const void *smf_data, int length, bool loop) {
    if (!smf_data || length <= 0) return -1;

    platform_midi_stop();

    char temp_dir[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_dir);
    GetTempFileNameA(temp_dir, "ecs", 0, s_midi_temp_path);

    /* Replace extension with .mid so MCI recognizes the format */
    size_t len = strlen(s_midi_temp_path);
    if (len > 4) {
        s_midi_temp_path[len - 4] = '\0';
        strcat(s_midi_temp_path, ".mid");
    }

    HANDLE hf = CreateFileA(s_midi_temp_path, GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (hf == INVALID_HANDLE_VALUE) return -1;

    DWORD written;
    WriteFile(hf, smf_data, length, &written, NULL);
    CloseHandle(hf);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "open \"%s\" type sequencer alias ecsmidi", s_midi_temp_path);
    if (mciSendStringA(cmd, NULL, 0, NULL) != 0) {
        DeleteFileA(s_midi_temp_path);
        return -1;
    }

    mciSendStringA("play ecsmidi from 0", NULL, 0, NULL);
    s_midi_active = true;
    s_midi_loop = loop;
    return 0;
}

void platform_midi_stop(void) {
    if (!s_midi_active) return;
    mciSendStringA("stop ecsmidi", NULL, 0, NULL);
    mciSendStringA("close ecsmidi", NULL, 0, NULL);
    if (s_midi_temp_path[0]) {
        DeleteFileA(s_midi_temp_path);
        s_midi_temp_path[0] = '\0';
    }
    s_midi_active = false;
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

#endif /* _WIN32 */
