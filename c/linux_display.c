#include "display.h"
#include <X11/Xlib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* XRandR is used opportunistically via dlopen so the binary links only
 * against libX11 (the CI baseline, see .github/workflows/ci.yml). When the
 * header or the runtime library is missing, enumeration falls back to one
 * entry per X screen; headless hosts (no DISPLAY) always report 0. */
#if defined(__has_include)
#if __has_include(<X11/extensions/Xrandr.h>)
#define DISPLAY_HAVE_XRANDR_HEADER 1
#include <X11/extensions/Xrandr.h>
#include <dlfcn.h>
#endif
#endif

#define DISPLAY_MAX_SCREENS 64

static void sanitize_field(char *s) {
    for (; *s != '\0'; s++) {
        if (*s == '|') {
            *s = '/';
        } else if (*s == '\n' || *s == '\r') {
            *s = ' ';
        }
    }
}

static int has_display_env(void) {
    const char *d = getenv("DISPLAY");
    return d != 0 && d[0] != '\0';
}

struct display_row {
    char name[128];
    int x, y, w, h;
    int scale_x100;
    int dpi;
    int refresh_mhz;
    int bpp;
    int primary;
    int orient;
    char color[64];
};

#ifdef DISPLAY_HAVE_XRANDR_HEADER
/* Minimal dlopen surface: only the symbols enumeration needs. */
typedef XRRScreenResources *(*FnGetRes)(Display *, Window);
typedef XRROutputInfo *(*FnGetOut)(Display *, XRRScreenResources *, RROutput);
typedef XRRCrtcInfo *(*FnGetCrtc)(Display *, XRRScreenResources *, RRCrtc);
typedef void (*FnFreeRes)(XRRScreenResources *);
typedef void (*FnFreeOut)(XRROutputInfo *);
typedef void (*FnFreeCrtc)(XRRCrtcInfo *);
typedef RROutput (*FnGetPrimary)(Display *, Window);

struct xrandr_api {
    void *handle;
    FnGetRes get_res;
    FnGetOut get_out;
    FnGetCrtc get_crtc;
    FnFreeRes free_res;
    FnFreeOut free_out;
    FnFreeCrtc free_crtc;
    FnGetPrimary get_primary;
};

static void *try_open_randr(void) {
    const char *names[] = {
        "libXrandr.so.2",
        "libXrandr.so.1",
        "libXrandr.so",
        0,
    };
    int i = 0;
    for (i = 0; names[i] != 0; i++) {
        void *h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (h != 0) {
            return h;
        }
    }
    return 0;
}

static int load_randr(struct xrandr_api *api) {
    memset(api, 0, sizeof(*api));
    api->handle = try_open_randr();
    if (api->handle == 0) {
        return 0;
    }
    api->get_res = (FnGetRes)dlsym(api->handle, "XRRGetScreenResources");
    api->get_out = (FnGetOut)dlsym(api->handle, "XRRGetOutputInfo");
    api->get_crtc = (FnGetCrtc)dlsym(api->handle, "XRRGetCrtcInfo");
    api->free_res = (FnFreeRes)dlsym(api->handle, "XRRFreeScreenResources");
    api->free_out = (FnFreeOut)dlsym(api->handle, "XRRFreeOutputInfo");
    api->free_crtc = (FnFreeCrtc)dlsym(api->handle, "XRRFreeCrtcInfo");
    api->get_primary = (FnGetPrimary)dlsym(api->handle, "XRRGetOutputPrimary");
    if (api->get_res == 0 || api->get_out == 0 || api->get_crtc == 0 ||
        api->free_res == 0 || api->free_out == 0 || api->free_crtc == 0 ||
        api->get_primary == 0) {
        dlclose(api->handle);
        memset(api, 0, sizeof(*api));
        return 0;
    }
    return 1;
}

/* Collects connected+active outputs; returns row count (0 on any failure).
 * dpi falls back to the screen average when per-output physical size is 0;
 * scale honors GDK_SCALE (integer steps) and is 100 otherwise — fractional
 * Wayland-style scale is not queryable over X11, so 100 is the honest
 * default rather than a guess. */
static int collect_randr(Display *dp, struct display_row *rows, int cap) {
    struct xrandr_api api;
    Window root;
    XRRScreenResources *res = 0;
    RROutput prim = 0;
    int count = 0;
    int fallback_dpi = 96;
    int scale_x100 = 100;
    int bpp = 24;
    if (dp == 0 || rows == 0 || cap <= 0) {
        return 0;
    }
    {
        int scr = DefaultScreen(dp);
        int px = DisplayWidth(dp, scr);
        int mm = DisplayWidthMM(dp, scr);
        if (px > 0 && mm > 0) {
            fallback_dpi = (int)(px * 25.4 / mm + 0.5);
        }
        bpp = DefaultDepth(dp, scr);
    }
    {
        const char *gs = getenv("GDK_SCALE");
        if (gs != 0 && gs[0] >= '1' && gs[0] <= '4' && gs[1] == '\0') {
            scale_x100 = (gs[0] - '0') * 100;
        }
    }
    if (!load_randr(&api)) {
        return 0;
    }
    root = DefaultRootWindow(dp);
    res = api.get_res(dp, root);
    if (res == 0) {
        dlclose(api.handle);
        return 0;
    }
    prim = api.get_primary(dp, root);
    for (int i = 0; i < res->noutput && count < cap; i++) {
        XRROutputInfo *out = api.get_out(dp, res, res->outputs[i]);
        XRRCrtcInfo *crtc = 0;
        int dpi = fallback_dpi;
        int refresh_mhz = -1;
        if (out == 0) {
            continue;
        }
        if (out->connection != RR_Connected || out->crtc == 0) {
            api.free_out(out);
            continue;
        }
        crtc = api.get_crtc(dp, res, out->crtc);
        if (crtc == 0 || crtc->width == 0 || crtc->height == 0) {
            if (crtc != 0) {
                api.free_crtc(crtc);
            }
            api.free_out(out);
            continue;
        }
        /* Refresh from the driving mode; 0 dotClock means unknown. */
        for (int m = 0; m < res->nmode; m++) {
            if (res->modes[m].id == crtc->mode && res->modes[m].hTotal > 0 &&
                res->modes[m].vTotal > 0 && res->modes[m].dotClock > 0) {
                double hz = (double)res->modes[m].dotClock /
                            ((double)res->modes[m].hTotal *
                             (double)res->modes[m].vTotal);
                if (hz > 0.0) {
                    refresh_mhz = (int)(hz * 1000.0 + 0.5);
                }
                break;
            }
        }
        if (out->mm_width > 0 && crtc->width > 0) {
            dpi = (int)(crtc->width * 25.4 / out->mm_width + 0.5);
        }
        {
            struct display_row *r = &rows[count];
            size_t nlen = out->name != 0 ? strlen(out->name) : 0;
            if (nlen >= sizeof(r->name)) {
                nlen = sizeof(r->name) - 1;
            }
            if (nlen > 0) {
                memcpy(r->name, out->name, nlen);
            }
            r->name[nlen] = '\0';
            sanitize_field(r->name);
            r->x = crtc->x;
            r->y = crtc->y;
            r->w = (int)crtc->width;
            r->h = (int)crtc->height;
            r->scale_x100 = scale_x100;
            r->dpi = dpi > 0 ? dpi : -1;
            r->refresh_mhz = refresh_mhz;
            r->bpp = bpp > 0 ? bpp : -1;
            r->primary = (res->outputs[i] == prim) ? 1 : 0;
            /* RandR rotation bitmask -> wire degrees. */
            if (crtc->rotation == RR_Rotate_90) {
                r->orient = 90;
            } else if (crtc->rotation == RR_Rotate_180) {
                r->orient = 180;
            } else if (crtc->rotation == RR_Rotate_270) {
                r->orient = 270;
            } else {
                r->orient = 0;
            }
            r->color[0] = '\0';
            count++;
        }
        api.free_crtc(crtc);
        api.free_out(out);
    }
    /* First enumerated output is primary when the server reports none. */
    if (count > 0) {
        int any_primary = 0;
        for (int i = 0; i < count; i++) {
            if (rows[i].primary) {
                any_primary = 1;
            }
        }
        if (!any_primary) {
            rows[0].primary = 1;
        }
    }
    api.free_res(res);
    dlclose(api.handle);
    return count;
}
#endif

static int collect_fallback(Display *dp, struct display_row *rows, int cap) {
    int nscr = 0;
    int count = 0;
    if (dp == 0 || rows == 0 || cap <= 0) {
        return 0;
    }
    nscr = ScreenCount(dp);
    if (nscr > cap) {
        nscr = cap;
    }
    for (int s = 0; s < nscr; s++) {
        struct display_row *r = &rows[count];
        int px = DisplayWidth(dp, s);
        int py = DisplayHeight(dp, s);
        int mm = DisplayWidthMM(dp, s);
        if (px <= 0 || py <= 0) {
            continue;
        }
        snprintf(r->name, sizeof(r->name), "Screen %d", s);
        r->x = 0;
        r->y = 0;
        r->w = px;
        r->h = py;
        r->scale_x100 = 100;
        r->dpi = (mm > 0) ? (int)(px * 25.4 / mm + 0.5) : -1;
        r->refresh_mhz = -1;
        r->bpp = DefaultDepth(dp, s) > 0 ? DefaultDepth(dp, s) : -1;
        r->primary = (s == DefaultScreen(dp)) ? 1 : 0;
        r->orient = 0;
        r->color[0] = '\0';
        count++;
    }
    return count;
}

static int collect_all(struct display_row *rows, int cap) {
    Display *dp = 0;
    int n = 0;
    if (!has_display_env() || rows == 0 || cap <= 0) {
        return 0;
    }
    dp = XOpenDisplay(0);
    if (dp == 0) {
        return 0;
    }
#ifdef DISPLAY_HAVE_XRANDR_HEADER
    n = collect_randr(dp, rows, cap);
    if (n > 0) {
        XCloseDisplay(dp);
        return n;
    }
#endif
    n = collect_fallback(dp, rows, cap);
    XCloseDisplay(dp);
    return n;
}

int display_native_count(void) {
    struct display_row rows[DISPLAY_MAX_SCREENS];
    return collect_all(rows, DISPLAY_MAX_SCREENS);
}

static char g_entry[1024];

const char *display_native_at(int index) {
    struct display_row rows[DISPLAY_MAX_SCREENS];
    struct display_row *r = 0;
    int n = 0;
    if (index < 0) {
        return "";
    }
    n = collect_all(rows, DISPLAY_MAX_SCREENS);
    if (index >= n) {
        return "";
    }
    r = &rows[index];
    snprintf(g_entry, sizeof(g_entry), "%s|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%s",
             r->name, r->x, r->y, r->w, r->h, r->scale_x100, r->dpi,
             r->refresh_mhz, r->bpp, r->primary, r->orient, r->color);
    return g_entry;
}
