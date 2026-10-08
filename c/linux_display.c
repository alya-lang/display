#include "display.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

/* Display backends, preferred first: native Wayland (zwlr-output-management
 * plus wl_output/xdg-output enrichment) wins when WAYLAND_DISPLAY answers,
 * because XWayland shadows real topology; XRandR covers X11 sessions; one
 * entry per X screen is the last resort. Headless hosts report 0. The binary
 * links only against libX11 (CI baseline); XRandR is dlopen'd when its
 * header was present at compile time. */
#if defined(__has_include)
#if __has_include(<X11/extensions/Xrandr.h>)
#define DISPLAY_HAVE_XRANDR_HEADER 1
#include <X11/extensions/Xrandr.h>
#include <dlfcn.h>
#endif
#endif

#define DISPLAY_MAX_ROWS 32
#define DISPLAY_MAX_MODES 128

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

struct display_mode {
    int w, h, r;
};

struct display_row {
    char name[128];
    int x, y, w, h;
    int scale_x100;
    int dpi;
    int refresh_mhz;
    int bpp;
    int primary;
    int orient;
    char color[128];
    int work_x, work_y, work_w, work_h;
    char connector[16];
    char gpu[192];
    char label[128];
    char edid_make[16];
    char edid_model[128];
    char edid_serial[64];
    int wide;
    int hdr;
    int nmode;
    struct display_mode modes[DISPLAY_MAX_MODES];
};

static void row_init(struct display_row *r) {
    memset(r, 0, sizeof(*r));
    r->scale_x100 = -1;
    r->dpi = -1;
    r->refresh_mhz = -1;
    r->bpp = -1;
    r->work_w = -1;
    r->work_h = -1;
    r->wide = -1;
    r->hdr = -1;
}

/* EDID base-block parsing (shared logic with the macOS backend). */
static void edid_text(const uint8_t *p, char *out, size_t outsz) {
    size_t n = 0;
    size_t end = 0;
    if (out == 0 || outsz == 0 || p == 0) {
        if (out != 0 && outsz > 0) {
            out[0] = '\0';
        }
        return;
    }
    out[0] = '\0';
    end = 13;
    while (end > 0 && (p[end - 1] == ' ' || p[end - 1] == '\n' ||
                       p[end - 1] == '\r' || p[end - 1] == '\0')) {
        end--;
    }
    for (n = 0; n < end && n + 1 < outsz; n++) {
        char c = (char)p[n];
        out[n] = (c >= 32 && c < 127) ? c : ' ';
    }
    out[n] = '\0';
    while (n > 0 && out[n - 1] == ' ') {
        out[--n] = '\0';
    }
}

static int parse_edid(const uint8_t *ed, size_t len, char *make, size_t makesz,
                      char *model, size_t modelz, char *serial, size_t serz) {
    static const uint8_t magic[8] = {0x00, 0xFF, 0xFF, 0xFF,
                                     0xFF, 0xFF, 0xFF, 0x00};
    unsigned raw = 0;
    int d = 0;
    if (make != 0 && makesz > 0) {
        make[0] = '\0';
    }
    if (model != 0 && modelz > 0) {
        model[0] = '\0';
    }
    if (serial != 0 && serz > 0) {
        serial[0] = '\0';
    }
    if (ed == 0 || len < 128 || memcmp(ed, magic, 8) != 0) {
        return 0;
    }
    raw = ((unsigned)ed[8] << 8) | ed[9];
    if (make != 0 && makesz >= 4) {
        make[0] = (char)('A' + ((raw >> 10) & 31) - 1);
        make[1] = (char)('A' + ((raw >> 5) & 31) - 1);
        make[2] = (char)('A' + (raw & 31) - 1);
        make[3] = '\0';
        for (d = 0; d < 3; d++) {
            if (make[d] < 'A' || make[d] > 'Z') {
                make[0] = '\0';
                break;
            }
        }
        sanitize_field(make);
    }
    if (serial != 0 && serz > 0) {
        unsigned sn = (unsigned)ed[12] | ((unsigned)ed[13] << 8) |
                      ((unsigned)ed[14] << 16) | ((unsigned)ed[15] << 24);
        if (sn != 0) {
            snprintf(serial, serz, "%u", sn);
        }
    }
    for (d = 0; d < 4; d++) {
        const uint8_t *dp = ed + 54 + d * 18;
        if (dp[0] != 0 || dp[1] != 0) {
            continue;
        }
        if (dp[3] == 0xFC && model != 0) {
            edid_text(dp + 5, model, modelz);
            sanitize_field(model);
        } else if (dp[3] == 0xFF && serial != 0) {
            edid_text(dp + 5, serial, serz);
            sanitize_field(serial);
        }
    }
    return 1;
}

/* Connector vocabulary from an XRandR output name ("HDMI-1", "eDP-1"). */
static const char *connector_for_name(const char *name) {
    if (name == 0 || name[0] == '\0') {
        return "";
    }
    if (strncmp(name, "eDP", 3) == 0 || strncmp(name, "LVDS", 4) == 0 ||
        strncmp(name, "DSI", 3) == 0) {
        return "internal";
    }
    if (strncmp(name, "HDMI", 4) == 0) {
        return "hdmi";
    }
    if (strncmp(name, "DP", 2) == 0) {
        return "dp";
    }
    if (strncmp(name, "DVI", 3) == 0) {
        return "dvi";
    }
    if (strncmp(name, "VGA", 3) == 0) {
        return "vga";
    }
    return "";
}

/* GPU driver label from sysfs: the driver basename (i915, amdgpu, nvidia)
 * when exactly one DRM card exists, "" on multi-GPU (unattributable) or
 * when sysfs is unavailable. Documented approximation. */
static void sysfs_gpu_label(char *out, size_t outsz) {
    DIR *d = 0;
    struct dirent *e = 0;
    char found[64] = {0};
    int ncards = 0;
    if (out == 0 || outsz == 0) {
        return;
    }
    out[0] = '\0';
    d = opendir("/sys/class/drm");
    if (d == 0) {
        return;
    }
    while ((e = readdir(d)) != 0) {
        char linkp[256];
        char tgt[256];
        ssize_t n = 0;
        char *base = 0;
        if (strncmp(e->d_name, "card", 4) != 0) {
            continue;
        }
        if (e->d_name[4] < '0' || e->d_name[4] > '9' ||
            e->d_name[5] != '\0') {
            continue;
        }
        snprintf(linkp, sizeof(linkp), "/sys/class/drm/%s/device/driver",
                 e->d_name);
        n = readlink(linkp, tgt, sizeof(tgt) - 1);
        if (n <= 0) {
            continue;
        }
        tgt[n] = '\0';
        base = strrchr(tgt, '/');
        base = (base != 0 && base[1] != '\0') ? base + 1 : tgt;
        ncards++;
        strncpy(found, base, sizeof(found) - 1);
        found[sizeof(found) - 1] = '\0';
    }
    closedir(d);
    if (ncards == 1 && found[0] != '\0') {
        strncpy(out, found, outsz - 1);
        out[outsz - 1] = '\0';
        sanitize_field(out);
    }
}

/* ICC profile description from the root _ICC_PROFILE bytes (raw profile,
 * not a filename): walks the tag table for 'desc' TextDescriptionType. */
static void icc_desc_name(Display *dp, Window root, char *out, size_t outsz) {
    Atom prop = None;
    Atom actual = None;
    int format = 0;
    unsigned long nitems = 0;
    unsigned long after = 0;
    unsigned char *data = 0;
    if (out == 0 || outsz == 0 || dp == 0) {
        return;
    }
    out[0] = '\0';
    prop = XInternAtom(dp, "_ICC_PROFILE", True);
    if (prop == None) {
        return;
    }
    if (XGetWindowProperty(dp, root, prop, 0, 1 << 20, False, XA_CARDINAL,
                           &actual, &format, &nitems, &after, &data) !=
            Success ||
        data == 0 || nitems < 132) {
        if (data != 0) {
            XFree(data);
        }
        return;
    }
    {
        /* Tag table at offset 128: count u32 BE, then 12-byte entries. */
        unsigned long count =
            ((unsigned long)data[128] << 24) |
            ((unsigned long)data[129] << 16) |
            ((unsigned long)data[130] << 8) | (unsigned long)data[131];
        unsigned long t = 0;
        if (count > 64) {
            count = 64;
        }
        for (t = 0; t < count; t++) {
            size_t base = 132 + t * 12;
            unsigned long off = 0;
            unsigned long sz = 0;
            unsigned long ascii = 0;
            size_t start = 0;
            size_t n = 0;
            if (base + 12 > nitems) {
                break;
            }
            if (memcmp(data + base, "desc", 4) != 0) {
                continue;
            }
            off = ((unsigned long)data[base + 4] << 24) |
                  ((unsigned long)data[base + 5] << 16) |
                  ((unsigned long)data[base + 6] << 8) |
                  (unsigned long)data[base + 7];
            sz = ((unsigned long)data[base + 8] << 24) |
                 ((unsigned long)data[base + 9] << 16) |
                 ((unsigned long)data[base + 10] << 8) |
                 (unsigned long)data[base + 11];
            if (off + 12 > nitems || sz < 12 || off + sz > nitems) {
                continue;
            }
            ascii = ((unsigned long)data[off + 8] << 24) |
                    ((unsigned long)data[off + 9] << 16) |
                    ((unsigned long)data[off + 10] << 8) |
                    (unsigned long)data[off + 11];
            if (ascii == 0 || ascii > sz) {
                ascii = sz - 8 > 0 ? sz - 8 : 0;
            }
            start = off + 12;
            for (n = 0; n + 1 < outsz && n < ascii && start + n < nitems;
                 n++) {
                char c = (char)data[start + n];
                if (c == '\0') {
                    break;
                }
                out[n] = (c >= 32 && c < 127) ? c : ' ';
            }
            out[n] = '\0';
            sanitize_field(out);
            break;
        }
    }
    XFree(data);
}

/* Desktop-wide _NET_WORKAREA (x,y,w,h CARDINALs); 1 when readable. */
static int net_workarea(Display *dp, Window root, long *x, long *y, long *w,
                        long *h) {
    Atom prop = None;
    Atom actual = None;
    int format = 0;
    unsigned long nitems = 0;
    unsigned long after = 0;
    unsigned char *data = 0;
    long *v = 0;
    prop = XInternAtom(dp, "_NET_WORKAREA", True);
    if (prop == None) {
        return 0;
    }
    if (XGetWindowProperty(dp, root, prop, 0, 4, False, XA_CARDINAL, &actual,
                           &format, &nitems, &after, &data) != Success ||
        data == 0 || format != 32 || nitems < 4) {
        if (data != 0) {
            XFree(data);
        }
        return 0;
    }
    v = (long *)data;
    *x = v[0];
    *y = v[1];
    *w = v[2];
    *h = v[3];
    XFree(data);
    return *w > 0 && *h > 0;
}

#ifdef DISPLAY_HAVE_XRANDR_HEADER
/* Minimal dlopen surface: only the symbols enumeration needs, plus output
 * property reads for EDID. */
typedef XRRScreenResources *(*FnGetRes)(Display *, Window);
typedef XRROutputInfo *(*FnGetOut)(Display *, XRRScreenResources *, RROutput);
typedef XRRCrtcInfo *(*FnGetCrtc)(Display *, XRRScreenResources *, RRCrtc);
typedef void (*FnFreeRes)(XRRScreenResources *);
typedef void (*FnFreeOut)(XRROutputInfo *);
typedef void (*FnFreeCrtc)(XRRCrtcInfo *);
typedef RROutput (*FnGetPrimary)(Display *, Window);
typedef int (*FnGetOutProp)(Display *, RROutput, Atom, long, long, Bool, Bool,
                            Atom, Atom *, int *, unsigned long *,
                            unsigned long *, unsigned char **);

struct xrandr_api {
    void *handle;
    FnGetRes get_res;
    FnGetOut get_out;
    FnGetCrtc get_crtc;
    FnFreeRes free_res;
    FnFreeOut free_out;
    FnFreeCrtc free_crtc;
    FnGetPrimary get_primary;
    FnGetOutProp get_out_prop;
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
    api->get_out_prop =
        (FnGetOutProp)dlsym(api->handle, "XRRGetOutputProperty");
    if (api->get_res == 0 || api->get_out == 0 || api->get_crtc == 0 ||
        api->free_res == 0 || api->free_out == 0 || api->free_crtc == 0 ||
        api->get_primary == 0 || api->get_out_prop == 0) {
        dlclose(api->handle);
        memset(api, 0, sizeof(*api));
        return 0;
    }
    return 1;
}

static int randr_mode_refresh(XRRScreenResources *res, RRMode id) {
    int m = 0;
    for (m = 0; m < res->nmode; m++) {
        if (res->modes[m].id == id && res->modes[m].hTotal > 0 &&
            res->modes[m].vTotal > 0 && res->modes[m].dotClock > 0) {
            double hz = (double)res->modes[m].dotClock /
                        ((double)res->modes[m].hTotal *
                         (double)res->modes[m].vTotal);
            if (hz > 0.0) {
                return (int)(hz * 1000.0 + 0.5);
            }
            return -1;
        }
    }
    return -1;
}

/* Reads the raw EDID blob of an output ("" outputs stay empty). */
static void randr_edid(Display *dp, struct xrandr_api *api, RROutput output,
                       char *make, size_t makesz, char *model, size_t modelz,
                       char *serial, size_t serz) {
    Atom prop = None;
    Atom actual = None;
    int format = 0;
    unsigned long nitems = 0;
    unsigned long after = 0;
    unsigned char *data = 0;
    if (make != 0 && makesz > 0) {
        make[0] = '\0';
    }
    if (model != 0 && modelz > 0) {
        model[0] = '\0';
    }
    if (serial != 0 && serz > 0) {
        serial[0] = '\0';
    }
    prop = XInternAtom(dp, "EDID", True);
    if (prop == None) {
        return;
    }
    if (api->get_out_prop(dp, output, prop, 0, 128, False, False,
                           AnyPropertyType, &actual, &format, &nitems, &after,
                           &data) != Success ||
        data == 0 || nitems < 128) {
        if (data != 0) {
            XFree(data);
        }
        return;
    }
    parse_edid(data, (size_t)nitems, make, makesz, model, modelz, serial,
               serz);
    XFree(data);
}

static int collect_randr(Display *dp, struct xrandr_api *api,
                         struct display_row *rows, int cap, const char *gpu,
                         const char *icc) {
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
    root = DefaultRootWindow(dp);
    res = api->get_res(dp, root);
    if (res == 0) {
        return 0;
    }
    prim = api->get_primary(dp, root);
    for (int i = 0; i < res->noutput && count < cap; i++) {
        XRROutputInfo *out = api->get_out(dp, res, res->outputs[i]);
        XRRCrtcInfo *crtc = 0;
        int dpi = fallback_dpi;
        int refresh_mhz = -1;
        if (out == 0) {
            continue;
        }
        if (out->connection != RR_Connected || out->crtc == 0) {
            api->free_out(out);
            continue;
        }
        crtc = api->get_crtc(dp, res, out->crtc);
        if (crtc == 0 || crtc->width == 0 || crtc->height == 0) {
            if (crtc != 0) {
                api->free_crtc(crtc);
            }
            api->free_out(out);
            continue;
        }
        refresh_mhz = randr_mode_refresh(res, crtc->mode);
        if (out->mm_width > 0 && crtc->width > 0) {
            dpi = (int)(crtc->width * 25.4 / out->mm_width + 0.5);
        }
        {
            struct display_row *r = &rows[count];
            size_t nlen = out->name != 0 ? strlen(out->name) : 0;
            row_init(r);
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
            if (crtc->rotation == RR_Rotate_90) {
                r->orient = 90;
            } else if (crtc->rotation == RR_Rotate_180) {
                r->orient = 180;
            } else if (crtc->rotation == RR_Rotate_270) {
                r->orient = 270;
            } else {
                r->orient = 0;
            }
            if (icc != 0) {
                strncpy(r->color, icc, sizeof(r->color) - 1);
                r->color[sizeof(r->color) - 1] = '\0';
            }
            strncpy(r->connector, connector_for_name(r->name),
                    sizeof(r->connector) - 1);
            if (gpu != 0) {
                strncpy(r->gpu, gpu, sizeof(r->gpu) - 1);
                r->gpu[sizeof(r->gpu) - 1] = '\0';
            }
            randr_edid(dp, api, res->outputs[i], r->edid_make,
                       sizeof(r->edid_make), r->edid_model,
                       sizeof(r->edid_model), r->edid_serial,
                       sizeof(r->edid_serial));
            if (r->edid_model[0] != '\0') {
                strncpy(r->label, r->edid_model, sizeof(r->label) - 1);
                r->label[sizeof(r->label) - 1] = '\0';
            }
            /* Known mode list for display_native_mode_*: output modes. */
            for (int m = 0; m < out->nmode && r->nmode < DISPLAY_MAX_MODES;
                 m++) {
                for (int k = 0; k < res->nmode; k++) {
                    if (res->modes[k].id == out->modes[m] &&
                        res->modes[k].width > 0 &&
                        res->modes[k].height > 0) {
                        r->modes[r->nmode].w = (int)res->modes[k].width;
                        r->modes[r->nmode].h = (int)res->modes[k].height;
                        r->modes[r->nmode].r =
                            randr_mode_refresh(res, out->modes[m]);
                        r->nmode++;
                        break;
                    }
                }
            }
            /* X11 exposes no HDR/wide-gamut state: stays -1 (unknown). */
            count++;
        }
        api->free_crtc(crtc);
        api->free_out(out);
    }
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
    api->free_res(res);
    return count;
}
#endif

static int collect_fallback(Display *dp, struct display_row *rows, int cap,
                            const char *icc) {
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
        row_init(r);
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
        if (icc != 0) {
            strncpy(r->color, icc, sizeof(r->color) - 1);
            r->color[sizeof(r->color) - 1] = '\0';
        }
        /* Only the current mode is knowable without RandR. */
        r->modes[0].w = px;
        r->modes[0].h = py;
        r->modes[0].r = -1;
        r->nmode = 1;
        count++;
    }
    return count;
}

/* ---- Native Wayland enumeration ----
 *
 * Speaks the raw Wayland wire protocol (same helpers as the gui package):
 * wl_output geometry/mode/scale/name plus xdg-output logical extents give
 * position and fractional scale without any toolkit; zwlr-output-management
 * (wlroots family) additionally yields full mode lists and make/model/serial.
 * GNOME/KDE (no zwlr) still get geometry, current mode, scale and position
 * through wl_output + xdg-output. Without any reachable compositor this
 * path yields 0 and X11 is tried next. */

static uint32_t wl_rd32(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wl_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static int wl_send_all(int fd, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static int wl_req(int fd, uint32_t obj, uint32_t opcode, const uint32_t *words,
                  size_t nwords) {
    uint8_t buf[64];
    uint32_t size = (uint32_t)(8 + nwords * 4);
    if (8 + nwords * 4 > sizeof(buf)) {
        return -1;
    }
    wl_wr32(buf, obj);
    wl_wr32(buf + 4, (size << 16) | (opcode & 0xFFFF));
    for (size_t i = 0; i < nwords; i++) {
        wl_wr32(buf + 8 + i * 4, words[i]);
    }
    return wl_send_all(fd, buf, size);
}

/* Request carrying (new_id, object): zxdg_output_manager.get_xdg_output. */
static int wl_req_id_obj(int fd, uint32_t obj, uint32_t opcode, uint32_t nid,
                         uint32_t other) {
    uint8_t buf[16];
    wl_wr32(buf, obj);
    wl_wr32(buf + 4, (16 << 16) | (opcode & 0xFFFF));
    wl_wr32(buf + 8, nid);
    wl_wr32(buf + 12, other);
    return wl_send_all(fd, buf, sizeof(buf));
}

#define WL_MAX_OUTPUTS 32
#define WL_MAX_HEADS 32
#define WL_MAX_MODES 256

struct wl_output_state {
    uint32_t id;
    int have_geo;
    int gx, gy, gmmw, gmmh, transform;
    char make[64];
    char model[128];
    int cur_w, cur_h, cur_r;
    int scale;
    int have_scale;
    char name[128];
    char desc[256];
    int version;
};

struct wl_xdg_state {
    uint32_t id;
    uint32_t output;
    int lx, ly, lw, lh;
    int have_pos;
    int have_size;
    char name[128];
    char desc[256];
};

struct wl_head_state {
    uint32_t id;
    char name[128];
    char desc[256];
    int mmw, mmh;
    int enabled;
    uint32_t cur_mode;
    int x, y, transform, scale;
    int have_scale;
    char make[64];
    char model[128];
    char serial[64];
};

struct wl_mode_state {
    uint32_t id;
    uint32_t head;
    int w, h, r;
};

struct wl_ctx {
    int fd;
    uint32_t next_id;
    uint32_t display_id;
    uint32_t registry_id;
    uint32_t xdg_mgr;
    int xdg_version;
    uint32_t zwlr_mgr;
    int have_zwlr;
    uint32_t sync_id;
    int sync_done;
    struct wl_output_state outputs[WL_MAX_OUTPUTS];
    int noutputs;
    struct wl_xdg_state xdgs[WL_MAX_OUTPUTS];
    int nxdgs;
    struct wl_head_state heads[WL_MAX_HEADS];
    int nheads;
    struct wl_mode_state modes[WL_MAX_MODES];
    int nmodes;
};

static void wl_copy_str(char *dst, size_t dstsz, const uint8_t *body,
                        size_t body_len, size_t *off) {
    uint32_t slen = 0;
    size_t padded = 0;
    size_t n = 0;
    if (dst == 0 || dstsz == 0 || off == 0) {
        return;
    }
    dst[0] = '\0';
    if (*off + 4 > body_len) {
        return;
    }
    slen = wl_rd32(body + *off);
    *off += 4;
    padded = (slen + 3) & ~((size_t)3);
    if (*off + padded > body_len || slen == 0) {
        return;
    }
    n = slen - 1;
    if (n >= dstsz) {
        n = dstsz - 1;
    }
    memcpy(dst, body + *off, n);
    dst[n] = '\0';
    sanitize_field(dst);
    *off += padded;
}

static struct wl_output_state *wl_find_output(struct wl_ctx *c, uint32_t id) {
    for (int i = 0; i < c->noutputs; i++) {
        if (c->outputs[i].id == id) {
            return &c->outputs[i];
        }
    }
    return 0;
}

static struct wl_xdg_state *wl_find_xdg(struct wl_ctx *c, uint32_t id) {
    for (int i = 0; i < c->nxdgs; i++) {
        if (c->xdgs[i].id == id) {
            return &c->xdgs[i];
        }
    }
    return 0;
}

static struct wl_head_state *wl_find_head(struct wl_ctx *c, uint32_t id) {
    for (int i = 0; i < c->nheads; i++) {
        if (c->heads[i].id == id) {
            return &c->heads[i];
        }
    }
    return 0;
}

static struct wl_mode_state *wl_find_mode(struct wl_ctx *c, uint32_t id) {
    for (int i = 0; i < c->nmodes; i++) {
        if (c->modes[i].id == id) {
            return &c->modes[i];
        }
    }
    return 0;
}

static void wl_handle(struct wl_ctx *c, uint32_t obj, uint32_t opcode,
                      const uint8_t *body, size_t body_len) {
    size_t off = 0;
    if (obj == c->registry_id && opcode == 0) {
        /* global: name u32, interface string, version u32. Bind outputs and
         * output managers; everything else is irrelevant here. */
        uint32_t name = 0;
        char iface[64];
        uint32_t version = 0;
        size_t o2 = 0;
        if (body_len < 12) {
            return;
        }
        name = wl_rd32(body);
        off = 4;
        wl_copy_str(iface, sizeof(iface), body, body_len, &off);
        o2 = off;
        if (o2 + 4 <= body_len) {
            version = wl_rd32(body + o2);
        }
        if (strcmp(iface, "wl_output") == 0) {
            if (c->noutputs < WL_MAX_OUTPUTS) {
                uint32_t want = version > 4 ? 4 : version;
                uint32_t nid = c->next_id++;
                uint32_t args[3];
                struct wl_output_state *o = 0;
                args[0] = name;
                args[1] = want;
                args[2] = nid;
                /* wl_registry.bind(name, interface, version, new_id). */
                {
                    const char *s = "wl_output";
                    uint8_t msg[512];
                    size_t slen = strlen(s) + 1;
                    size_t padded = (slen + 3) & ~((size_t)3);
                    uint32_t size =
                        (uint32_t)(8 + 4 + 4 + padded + 4 + 4);
                    size_t at = 0;
                    if (size > sizeof(msg)) {
                        return;
                    }
                    wl_wr32(msg, c->registry_id);
                    wl_wr32(msg + 4, (size << 16) | 0);
                    at = 8;
                    wl_wr32(msg + at, name);
                    at += 4;
                    wl_wr32(msg + at, (uint32_t)slen);
                    at += 4;
                    memset(msg + at, 0, padded);
                    memcpy(msg + at, s, slen - 1);
                    at += padded;
                    wl_wr32(msg + at, want);
                    at += 4;
                    wl_wr32(msg + at, nid);
                    wl_send_all(c->fd, msg, size);
                }
                o = &c->outputs[c->noutputs++];
                memset(o, 0, sizeof(*o));
                o->id = nid;
                o->version = (int)want;
            }
        } else if (strcmp(iface, "zwlr_output_manager_v1") == 0 &&
                   !c->have_zwlr) {
            uint32_t want = version > 3 ? 3 : version;
            uint32_t nid = c->next_id++;
            uint8_t msg[512];
            const char *s = "zwlr_output_manager_v1";
            size_t slen = strlen(s) + 1;
            size_t padded = (slen + 3) & ~((size_t)3);
            uint32_t size = (uint32_t)(8 + 4 + 4 + padded + 4 + 4);
            size_t at = 0;
            if (size > sizeof(msg)) {
                return;
            }
            wl_wr32(msg, c->registry_id);
            wl_wr32(msg + 4, (size << 16) | 0);
            at = 8;
            wl_wr32(msg + at, name);
            at += 4;
            wl_wr32(msg + at, (uint32_t)slen);
            at += 4;
            memset(msg + at, 0, padded);
            memcpy(msg + at, s, slen - 1);
            at += padded;
            wl_wr32(msg + at, want);
            at += 4;
            wl_wr32(msg + at, nid);
            wl_send_all(c->fd, msg, size);
            c->zwlr_mgr = nid;
            c->have_zwlr = 1;
        } else if (strcmp(iface, "zxdg_output_manager_v1") == 0 &&
                   c->xdg_mgr == 0) {
            uint32_t want = version > 3 ? 3 : version;
            uint32_t nid = c->next_id++;
            uint8_t msg[512];
            const char *s = "zxdg_output_manager_v1";
            size_t slen = strlen(s) + 1;
            size_t padded = (slen + 3) & ~((size_t)3);
            uint32_t size = (uint32_t)(8 + 4 + 4 + padded + 4 + 4);
            size_t at = 0;
            if (size > sizeof(msg)) {
                return;
            }
            wl_wr32(msg, c->registry_id);
            wl_wr32(msg + 4, (size << 16) | 0);
            at = 8;
            wl_wr32(msg + at, name);
            at += 4;
            wl_wr32(msg + at, (uint32_t)slen);
            at += 4;
            memset(msg + at, 0, padded);
            memcpy(msg + at, s, slen - 1);
            at += padded;
            wl_wr32(msg + at, want);
            at += 4;
            wl_wr32(msg + at, nid);
            wl_send_all(c->fd, msg, size);
            c->xdg_mgr = nid;
            c->xdg_version = (int)want;
        }
        return;
    }
    if (obj == c->sync_id && opcode == 0) {
        c->sync_done = 1;
        return;
    }
    if (obj == c->display_id && opcode == 0 && body_len >= 8) {
        /* wl_display.error: object u32, code u32, message string. */
        uint32_t err_obj = wl_rd32(body);
        uint32_t err_code = wl_rd32(body + 4);
        char msg[256];
        size_t eo = 8;
        wl_copy_str(msg, sizeof(msg), body, body_len, &eo);
        fprintf(stderr, "alya-display: wayland protocol error %u: %s\n",
                err_code, msg);
        return;
    }
    {
        struct wl_output_state *o = wl_find_output(c, obj);
        if (o != 0) {
            if (opcode == 0) {
                /* geometry: x y phys_w phys_h subpixel make model transform */
                if (body_len >= 20) {
                    o->gx = (int)wl_rd32(body);
                    o->gy = (int)wl_rd32(body + 4);
                    o->gmmw = (int)wl_rd32(body + 8);
                    o->gmmh = (int)wl_rd32(body + 12);
                    off = 20;
                    wl_copy_str(o->make, sizeof(o->make), body, body_len,
                                &off);
                    wl_copy_str(o->model, sizeof(o->model), body, body_len,
                                &off);
                    if (off + 4 <= body_len) {
                        o->transform = (int)wl_rd32(body + off);
                    }
                    o->have_geo = 1;
                }
            } else if (opcode == 1) {
                /* mode: flags width height refresh */
                if (body_len >= 16) {
                    uint32_t flags = wl_rd32(body);
                    int w = (int)wl_rd32(body + 4);
                    int h = (int)wl_rd32(body + 8);
                    int r = (int)wl_rd32(body + 12);
                    if (w > 0 && h > 0 &&
                        ((flags & 1) || o->cur_w <= 0)) {
                        o->cur_w = w;
                        o->cur_h = h;
                        o->cur_r = r;
                    }
                }
            } else if (opcode == 3) {
                if (body_len >= 4) {
                    o->scale = (int)wl_rd32(body);
                    o->have_scale = 1;
                }
            } else if (opcode == 4 && o->version >= 4) {
                off = 0;
                wl_copy_str(o->name, sizeof(o->name), body, body_len, &off);
            } else if (opcode == 5 && o->version >= 4) {
                off = 0;
                wl_copy_str(o->desc, sizeof(o->desc), body, body_len, &off);
            }
            return;
        }
    }
    {
        struct wl_xdg_state *x = wl_find_xdg(c, obj);
        if (x != 0) {
            if (opcode == 0 && body_len >= 8) {
                x->lx = (int)wl_rd32(body);
                x->ly = (int)wl_rd32(body + 4);
                x->have_pos = 1;
            } else if (opcode == 1 && body_len >= 8) {
                x->lw = (int)wl_rd32(body);
                x->lh = (int)wl_rd32(body + 4);
                x->have_size = 1;
            } else if (opcode == 3 && c->xdg_version >= 2) {
                off = 0;
                wl_copy_str(x->name, sizeof(x->name), body, body_len, &off);
            } else if (opcode == 4 && c->xdg_version >= 2) {
                off = 0;
                wl_copy_str(x->desc, sizeof(x->desc), body, body_len, &off);
            }
            return;
        }
    }
    if (obj == c->zwlr_mgr && opcode == 0) {
        /* head: new_id. */
        if (body_len >= 4 && c->nheads < WL_MAX_HEADS) {
            struct wl_head_state *h = &c->heads[c->nheads++];
            memset(h, 0, sizeof(*h));
            h->id = wl_rd32(body);
        }
        return;
    }
    {
        struct wl_head_state *h = wl_find_head(c, obj);
        if (h != 0) {
            if (opcode == 0 || opcode == 1 || opcode == 10 ||
                opcode == 11 || opcode == 12) {
                char *dst = 0;
                size_t dstsz = 0;
                off = 0;
                if (opcode == 0) {
                    dst = h->name;
                    dstsz = sizeof(h->name);
                } else if (opcode == 1) {
                    dst = h->desc;
                    dstsz = sizeof(h->desc);
                } else if (opcode == 10) {
                    dst = h->make;
                    dstsz = sizeof(h->make);
                } else if (opcode == 11) {
                    dst = h->model;
                    dstsz = sizeof(h->model);
                } else {
                    dst = h->serial;
                    dstsz = sizeof(h->serial);
                }
                wl_copy_str(dst, dstsz, body, body_len, &off);
            } else if (opcode == 2 && body_len >= 8) {
                h->mmw = (int)wl_rd32(body);
                h->mmh = (int)wl_rd32(body + 4);
            } else if (opcode == 3) {
                /* mode: new_id. */
                if (body_len >= 4 && c->nmodes < WL_MAX_MODES) {
                    struct wl_mode_state *m = &c->modes[c->nmodes++];
                    memset(m, 0, sizeof(*m));
                    m->id = wl_rd32(body);
                    m->head = h->id;
                }
            } else if (opcode == 4 && body_len >= 4) {
                h->enabled = (int)wl_rd32(body);
            } else if (opcode == 5 && body_len >= 4) {
                h->cur_mode = wl_rd32(body);
            } else if (opcode == 6 && body_len >= 8) {
                h->x = (int)wl_rd32(body);
                h->y = (int)wl_rd32(body + 4);
            } else if (opcode == 7 && body_len >= 4) {
                h->transform = (int)wl_rd32(body);
            } else if (opcode == 8 && body_len >= 4) {
                h->scale = (int)wl_rd32(body);
                h->have_scale = 1;
            }
            return;
        }
    }
    {
        struct wl_mode_state *m = wl_find_mode(c, obj);
        if (m != 0) {
            if (opcode == 0 && body_len >= 8) {
                m->w = (int)wl_rd32(body);
                m->h = (int)wl_rd32(body + 4);
            } else if (opcode == 1 && body_len >= 4) {
                m->r = (int)wl_rd32(body);
            }
            return;
        }
    }
}

/* Read and dispatch all pending messages (non-blocking when drained). */

static void wl_dispatch(struct wl_ctx *c, int block_ms) {
    uint8_t buf[16384];
    struct pollfd pfd;
    ssize_t n = 0;
    size_t off = 0;
    pfd.fd = c->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, block_ms) <= 0) {
        return;
    }
    n = recv(c->fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        return;
    }
    while (off + 8 <= (size_t)n) {
        uint32_t obj = wl_rd32(buf + off);
        uint32_t hdr = wl_rd32(buf + off + 4);
        uint32_t size = hdr >> 16;
        uint32_t opcode = hdr & 0xFFFF;
        if (size < 8 || off + size > (size_t)n) {
            break;
        }
        wl_handle(c, obj, opcode, buf + off + 8, size - 8);
        off += size;
    }
}

static int wl_roundtrip(struct wl_ctx *c) {
    uint32_t args[1];
    int spins = 0;
    c->sync_done = 0;
    c->sync_id = c->next_id++;
    args[0] = c->sync_id;
    if (wl_req(c->fd, c->display_id, 0, args, 1) < 0) {
        return -1;
    }
    while (!c->sync_done && spins < 200) {
        wl_dispatch(c, 500);
        spins++;
    }
    return c->sync_done ? 0 : -1;
}

static int wl_transform_orient(int t) {
    /* WL_OUTPUT_TRANSFORM_*: 0 normal, 1 90, 2 180, 3 270, 4+ flipped. */
    switch (t) {
    case 1:
    case 5:
        return 90;
    case 2:
    case 6:
        return 180;
    case 3:
    case 7:
        return 270;
    default:
        return 0;
    }
}

static struct wl_head_state *wl_match_head(struct wl_ctx *c,
                                           const char *name, int w, int h) {
    int i = 0;
    for (i = 0; i < c->nheads; i++) {
        if (name[0] != '\0' && c->heads[i].name[0] != '\0' &&
            strcmp(c->heads[i].name, name) == 0) {
            return &c->heads[i];
        }
    }
    /* Fallback: match by current resolution (twin outputs share names). */
    for (i = 0; i < c->nheads; i++) {
        struct wl_head_state *hd = &c->heads[i];
        struct wl_mode_state *m = wl_find_mode(c, hd->cur_mode);
        if (m != 0 && m->w == w && m->h == h) {
            return hd;
        }
    }
    return 0;
}

/* Collects Wayland rows; 0 when no compositor answers. */
static int collect_wayland(struct display_row *rows, int cap,
                           const char *gpu) {
    const char *sock = getenv("WAYLAND_DISPLAY");
    const char *run_dir = 0;
    char path[256];
    struct sockaddr_un addr;
    struct wl_ctx c;
    int fd = -1;
    int i = 0;
    int count = 0;
    if (rows == 0 || cap <= 0) {
        return 0;
    }
    if (sock == 0 || sock[0] == '\0') {
        return 0;
    }
    if (sock[0] != '/') {
        run_dir = getenv("XDG_RUNTIME_DIR");
        if (run_dir == 0) {
            return 0;
        }
        snprintf(path, sizeof(path), "%s/%s", run_dir, sock);
        sock = path;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return 0;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return 0;
    }
    memset(&c, 0, sizeof(c));
    c.fd = fd;
    c.display_id = 1;
    c.next_id = 3;
    /* wl_display.get_registry -> 2 (ids 0/1 are reserved, 2 is registry). */
    {
        uint32_t args[1];
        args[0] = 2;
        if (wl_req(fd, 1, 1, args, 1) < 0) {
            close(fd);
            return 0;
        }
    }
    c.registry_id = 2;
    if (wl_roundtrip(&c) < 0 || c.noutputs == 0) {
        close(fd);
        return 0;
    }
    /* Bind xdg outputs, then flush head/mode traffic. */
    for (i = 0; i < c.noutputs && c.xdg_mgr != 0; i++) {
        if (c.nxdgs < WL_MAX_OUTPUTS) {
            struct wl_xdg_state *x = &c.xdgs[c.nxdgs++];
            memset(x, 0, sizeof(*x));
            x->id = c.next_id++;
            x->output = c.outputs[i].id;
            wl_req_id_obj(fd, c.xdg_mgr, 1, x->id, x->output);
        }
    }
    wl_roundtrip(&c);
    wl_roundtrip(&c);
    wl_roundtrip(&c);
    for (i = 0; i < c.noutputs && count < cap; i++) {
        struct wl_output_state *o = &c.outputs[i];
        struct wl_xdg_state *x = 0;
        struct wl_head_state *h = 0;
        struct display_row *r = 0;
        char eff_name[128];
        int k = 0;
        int phys_w = o->cur_w;
        int phys_h = o->cur_h;
        if (phys_w <= 0 || phys_h <= 0) {
            continue;
        }
        for (k = 0; k < c.nxdgs; k++) {
            if (c.xdgs[k].output == o->id) {
                x = &c.xdgs[k];
                break;
            }
        }
        eff_name[0] = '\0';
        if (x != 0 && x->name[0] != '\0') {
            strncpy(eff_name, x->name, sizeof(eff_name) - 1);
        } else if (o->name[0] != '\0') {
            strncpy(eff_name, o->name, sizeof(eff_name) - 1);
        }
        eff_name[sizeof(eff_name) - 1] = '\0';
        h = (eff_name[0] != '\0')
                ? wl_match_head(&c, eff_name, phys_w, phys_h)
                : 0;
        if (h == 0 && eff_name[0] == '\0' && o->model[0] != '\0') {
            /* Compositors without any naming (bare wl_output): match by
             * model string as a last resort. */
            for (k = 0; k < c.nheads; k++) {
                if (strcmp(c.heads[k].model, o->model) == 0) {
                    h = &c.heads[k];
                    break;
                }
            }
        }
        r = &rows[count];
        row_init(r);
        if (eff_name[0] != '\0') {
            strncpy(r->name, eff_name, sizeof(r->name) - 1);
        } else if (h != 0 && h->name[0] != '\0') {
            strncpy(r->name, h->name, sizeof(r->name) - 1);
        } else {
            snprintf(r->name, sizeof(r->name), "Wayland-%d", count);
        }
        r->name[sizeof(r->name) - 1] = '\0';
        sanitize_field(r->name);
        r->x = (x != 0 && x->have_pos)
                   ? x->lx
                   : (h != 0 ? h->x : 0);
        r->y = (x != 0 && x->have_pos)
                   ? x->ly
                   : (h != 0 ? h->y : 0);
        r->w = phys_w;
        r->h = phys_h;
        /* Fractional scale from xdg logical extents when they disagree with
         * physical pixels; integer wl/zwlr scale otherwise. */
        if (x != 0 && x->have_size && x->lw > 0 && x->lh > 0 &&
            (x->lw != phys_w || x->lh != phys_h)) {
            int s = (phys_w * 100) / x->lw;
            r->scale_x100 =
                (s >= 50 && s <= 400) ? s : (o->scale > 0 ? o->scale * 100
                                                          : 100);
        } else if (h != 0 && h->have_scale && h->scale > 0) {
            r->scale_x100 = h->scale * 100;
        } else if (o->have_scale && o->scale > 0) {
            r->scale_x100 = o->scale * 100;
        } else {
            r->scale_x100 = 100;
        }
        if (o->gmmw > 0) {
            r->dpi = (int)(phys_w * 25.4 / o->gmmw + 0.5);
        } else if (h != 0 && h->mmw > 0) {
            r->dpi = (int)(phys_w * 25.4 / h->mmw + 0.5);
        }
        r->refresh_mhz = (o->cur_r > 0) ? o->cur_r : -1;
        r->bpp = -1; /* no color-depth event on the wire */
        r->primary = (count == 0) ? 1 : 0;
        r->orient = wl_transform_orient(
            (h != 0 && h->transform >= 0) ? h->transform : o->transform);
        strncpy(r->connector, connector_for_name(r->name),
                sizeof(r->connector) - 1);
        if (gpu != 0) {
            strncpy(r->gpu, gpu, sizeof(r->gpu) - 1);
            r->gpu[sizeof(r->gpu) - 1] = '\0';
        }
        if (x != 0 && x->desc[0] != '\0') {
            strncpy(r->label, x->desc, sizeof(r->label) - 1);
        } else if (h != 0 && h->desc[0] != '\0') {
            strncpy(r->label, h->desc, sizeof(r->label) - 1);
        } else if (o->desc[0] != '\0') {
            strncpy(r->label, o->desc, sizeof(r->label) - 1);
        }
        r->label[sizeof(r->label) - 1] = '\0';
        /* Modes: zwlr head list wins; otherwise the current mode alone. */
        if (h != 0) {
            for (k = 0; k < c.nmodes && r->nmode < DISPLAY_MAX_MODES; k++) {
                if (c.modes[k].head == h->id && c.modes[k].w > 0 &&
                    c.modes[k].h > 0) {
                    r->modes[r->nmode].w = c.modes[k].w;
                    r->modes[r->nmode].h = c.modes[k].h;
                    r->modes[r->nmode].r =
                        c.modes[k].r > 0 ? c.modes[k].r : -1;
                    r->nmode++;
                }
            }
        }
        if (r->nmode == 0) {
            r->modes[0].w = phys_w;
            r->modes[0].h = phys_h;
            r->modes[0].r = r->refresh_mhz;
            r->nmode = 1;
        }
        /* Wayland exposes no work-area, ICC, EDID, HDR, or depth state:
         * rows keep the unknown sentinels (documented). */
        count++;
    }
    close(fd);
    return count;
}

static int collect_all(struct display_row *rows, int cap) {
    Display *dp = 0;
    char gpu[192] = {0};
    char icc[128] = {0};
    int n = 0;
    if (rows == 0 || cap <= 0) {
        return 0;
    }
    if (cap > DISPLAY_MAX_ROWS) {
        cap = DISPLAY_MAX_ROWS;
    }
    sysfs_gpu_label(gpu, sizeof(gpu));
    /* Native Wayland first: XWayland shadows real topology. */
    n = collect_wayland(rows, cap, gpu);
    if (n > 0) {
        return n;
    }
    if (!has_display_env()) {
        return 0;
    }
    dp = XOpenDisplay(0);
    if (dp == 0) {
        return 0;
    }
    {
        Window root = DefaultRootWindow(dp);
        icc_desc_name(dp, root, icc, sizeof(icc));
#ifdef DISPLAY_HAVE_XRANDR_HEADER
        {
            struct xrandr_api api;
            if (load_randr(&api)) {
                n = collect_randr(dp, &api, rows, cap, gpu, icc);
                if (n > 0) {
                    long wx = 0, wy = 0, ww = 0, wh = 0;
                    /* Single-display desktops inherit _NET_WORKAREA. */
                    if (n == 1 &&
                        net_workarea(dp, root, &wx, &wy, &ww, &wh)) {
                        rows[0].work_x = (int)wx;
                        rows[0].work_y = (int)wy;
                        rows[0].work_w = (int)ww;
                        rows[0].work_h = (int)wh;
                    }
                    /* Unload RandR only after XCloseDisplay: Xlib keeps
                     * extension close hooks into libXrandr, so dlclose
                     * first segfaults inside XCloseDisplay. */
                    XCloseDisplay(dp);
                    dlclose(api.handle);
                    return n;
                }
                dlclose(api.handle);
            }
        }
#endif
        n = collect_fallback(dp, rows, cap, icc);
        if (n == 1) {
            long wx = 0, wy = 0, ww = 0, wh = 0;
            if (net_workarea(dp, root, &wx, &wy, &ww, &wh)) {
                rows[0].work_x = (int)wx;
                rows[0].work_y = (int)wy;
                rows[0].work_w = (int)ww;
                rows[0].work_h = (int)wh;
            }
        }
    }
    XCloseDisplay(dp);
    return n;
}

int display_native_count(void) {
    struct display_row rows[DISPLAY_MAX_ROWS];
    return collect_all(rows, DISPLAY_MAX_ROWS);
}

static char g_entry[1024];

const char *display_native_at(int index) {
    struct display_row rows[DISPLAY_MAX_ROWS];
    struct display_row *r = 0;
    int n = 0;
    if (index < 0) {
        return "";
    }
    n = collect_all(rows, DISPLAY_MAX_ROWS);
    if (index >= n) {
        return "";
    }
    r = &rows[index];
    snprintf(g_entry, sizeof(g_entry), "%s|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%s",
             r->name, r->x, r->y, r->w, r->h, r->scale_x100, r->dpi,
             r->refresh_mhz, r->bpp, r->primary, r->orient, r->color);
    return g_entry;
}

static char g_extra[1408];

const char *display_native_extra(int index) {
    struct display_row rows[DISPLAY_MAX_ROWS];
    struct display_row *r = 0;
    int n = 0;
    if (index < 0) {
        return "";
    }
    n = collect_all(rows, DISPLAY_MAX_ROWS);
    if (index >= n) {
        return "";
    }
    r = &rows[index];
    snprintf(g_extra, sizeof(g_extra), "%d|%d|%d|%d|%s|%s|%s|%s|%s|%s|%d|%d",
             r->work_x, r->work_y, r->work_w, r->work_h, r->connector,
             r->gpu, r->label, r->edid_make, r->edid_model, r->edid_serial,
             r->wide, r->hdr);
    return g_extra;
}

static char g_modes[16384];

const char *display_native_modes_all(int disp) {
    struct display_row rows[DISPLAY_MAX_ROWS];
    struct display_row *r = 0;
    int n = 0;
    size_t pos = 0;
    int k = 0;
    g_modes[0] = '\0';
    if (disp < 0) {
        return "";
    }
    n = collect_all(rows, DISPLAY_MAX_ROWS);
    if (disp >= n) {
        return "";
    }
    r = &rows[disp];
    for (k = 0; k < r->nmode; k++) {
        int len = snprintf(g_modes + pos, sizeof(g_modes) - pos, "%s%d|%d|%d",
                           k > 0 ? "\n" : "", r->modes[k].w, r->modes[k].h,
                           r->modes[k].r);
        if (len < 0 || (size_t)len >= sizeof(g_modes) - pos) {
            break;
        }
        pos += (size_t)len;
    }
    return g_modes;
}
