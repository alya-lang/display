#include "display.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define DISPLAY_MAX_MONITORS 64

/* GetDpiForMonitor lives in Shcore.dll (Win 8.1+); load it at runtime so
 * the binary still runs on older systems (falls back to GetDeviceCaps). */
typedef enum {
    ALYA_MDT_EFFECTIVE_DPI = 0
} ALYA_MONITOR_DPI_TYPE;

typedef HRESULT(WINAPI *AlyaGetDpiForMonitorFn)(HMONITOR, ALYA_MONITOR_DPI_TYPE,
                                                UINT *, UINT *);

static void sanitize_field(char *s) {
    /* The Alya wire format splits on '|', so field text must never contain
     * it; newlines would break line-based debugging as well. */
    for (; *s != '\0'; s++) {
        if (*s == '|') {
            *s = '/';
        } else if (*s == '\n' || *s == '\r') {
            *s = ' ';
        }
    }
}

struct monitor_collect {
    HMONITOR items[DISPLAY_MAX_MONITORS];
    int count;
};

static BOOL CALLBACK collect_cb(HMONITOR h, HDC dc, LPRECT rc, LPARAM lp) {
    struct monitor_collect *c = (struct monitor_collect *)lp;
    (void)dc;
    (void)rc;
    if (c->count < DISPLAY_MAX_MONITORS) {
        c->items[c->count++] = h;
    }
    return TRUE;
}

static int enum_monitors(HMONITOR *out, int cap) {
    struct monitor_collect c;
    c.count = 0;
    if (cap <= 0 || out == 0) {
        return 0;
    }
    if (!EnumDisplayMonitors(0, 0, collect_cb, (LPARAM)&c)) {
        return 0;
    }
    if (c.count > cap) {
        c.count = cap;
    }
    for (int i = 0; i < c.count; i++) {
        out[i] = c.items[i];
    }
    return c.count;
}

int display_native_count(void) {
    HMONITOR items[DISPLAY_MAX_MONITORS];
    return enum_monitors(items, DISPLAY_MAX_MONITORS);
}

static int query_dpi(HMONITOR mon, const char *device) {
    /* Prefer per-monitor effective DPI (DPI-aware scale); fall back to the
     * device context, then to the 96-DPI baseline sentinel mapping. */
    static AlyaGetDpiForMonitorFn fn = 0;
    static int fn_probed = 0;
    if (!fn_probed) {
        HMODULE shcore = LoadLibraryA("Shcore.dll");
        if (shcore != 0) {
            fn = (AlyaGetDpiForMonitorFn)GetProcAddress(shcore,
                                                       "GetDpiForMonitor");
        }
        fn_probed = 1;
    }
    if (fn != 0) {
        UINT dx = 0, dy = 0;
        if (fn(mon, ALYA_MDT_EFFECTIVE_DPI, &dx, &dy) == S_OK && dx > 0) {
            return (int)dx;
        }
    }
    if (device != 0 && device[0] != '\0') {
        HDC hdc = CreateDCA(device, 0, 0, 0);
        if (hdc != 0) {
            int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
            DeleteDC(hdc);
            if (dpi > 0) {
                return dpi;
            }
        }
    }
    return 96;
}

static void query_color_profile(const char *device, char *out, size_t outsz) {
    /* ICM profile filename (e.g. "sRGB Color Space Profile.icm"); "" when
     * ICM is off or the query fails (common on servers/VMs). */
    if (out == 0 || outsz == 0) {
        return;
    }
    out[0] = '\0';
    if (device == 0 || device[0] == '\0') {
        return;
    }
    {
        HDC hdc = CreateDCA(device, 0, 0, 0);
        char path[512];
        DWORD pathsz = (DWORD)sizeof(path);
        if (hdc != 0) {
            if (GetICMProfileA(hdc, &pathsz, path) && path[0] != '\0') {
                char *base = strrchr(path, '\\');
                if (base != 0 && base[1] != '\0') {
                    strncpy(out, base + 1, outsz - 1);
                } else {
                    strncpy(out, path, outsz - 1);
                }
                out[outsz - 1] = '\0';
                sanitize_field(out);
            }
            DeleteDC(hdc);
        }
    }
}

static char g_entry[1024];

const char *display_native_at(int index) {
    HMONITOR items[DISPLAY_MAX_MONITORS];
    MONITORINFOEXA mi;
    DEVMODEA dm;
    char color[192];
    char name[64];
    int n, x, y, w, h, dpi, scale_x100, refresh_mhz, bpp, primary, orient;
    if (index < 0) {
        return "";
    }
    n = enum_monitors(items, DISPLAY_MAX_MONITORS);
    if (index >= n) {
        return "";
    }
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(items[index], (LPMONITORINFO)&mi)) {
        return "";
    }
    x = (int)mi.rcMonitor.left;
    y = (int)mi.rcMonitor.top;
    w = (int)(mi.rcMonitor.right - mi.rcMonitor.left);
    h = (int)(mi.rcMonitor.bottom - mi.rcMonitor.top);
    if (w <= 0 || h <= 0) {
        return "";
    }
    primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0 ? 1 : 0;
    strncpy(name, mi.szDevice, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    sanitize_field(name);

    dpi = query_dpi(items[index], name);
    if (dpi > 0) {
        scale_x100 = dpi * 100 / 96;
    } else {
        dpi = -1;
        scale_x100 = -1;
    }

    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    refresh_mhz = -1;
    bpp = -1;
    orient = 0;
    if (EnumDisplaySettingsA(name[0] != '\0' ? name : 0, ENUM_CURRENT_SETTINGS,
                             &dm)) {
        if (dm.dmBitsPerPel > 0) {
            bpp = (int)dm.dmBitsPerPel;
        }
        /* dmDisplayFrequency is 0/1 when unknown (not a real refresh). */
        if (dm.dmDisplayFrequency > 1) {
            refresh_mhz = (int)dm.dmDisplayFrequency * 1000;
        }
        switch (dm.dmDisplayOrientation) {
        case DMDO_90:
            orient = 90;
            break;
        case DMDO_180:
            orient = 180;
            break;
        case DMDO_270:
            orient = 270;
            break;
        default:
            orient = 0;
            break;
        }
    }

    query_color_profile(name, color, sizeof(color));

    snprintf(g_entry, sizeof(g_entry), "%s|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%s",
             name, x, y, w, h, scale_x100, dpi, refresh_mhz, bpp, primary,
             orient, color);
    return g_entry;
}

/* ---- Display modes, work area, connector, GPU, HDR ---- */

#define DISPLAY_MAX_MODES 256

/* Resolves the GDI device name ("\\.\\DISPLAY1") for an enumeration index. */
static int monitor_device(int index, char *out, size_t outsz) {
    HMONITOR items[DISPLAY_MAX_MONITORS];
    MONITORINFOEXA mi;
    int n = 0;
    if (out == 0 || outsz == 0 || index < 0) {
        return 0;
    }
    out[0] = '\0';
    n = enum_monitors(items, DISPLAY_MAX_MONITORS);
    if (index >= n) {
        return 0;
    }
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(items[index], (LPMONITORINFO)&mi)) {
        return 0;
    }
    strncpy(out, mi.szDevice, outsz - 1);
    out[outsz - 1] = '\0';
    return out[0] != '\0';
}

static char g_modes[16384];

const char *display_native_modes_all(int disp) {
    char dev[64];
    DEVMODEA dm;
    size_t pos = 0;
    int count = 0;
    int i = 0;
    g_modes[0] = '\0';
    if (disp < 0 || !monitor_device(disp, dev, sizeof(dev))) {
        return "";
    }
    for (i = 0; i < DISPLAY_MAX_MODES * 4; i++) {
        int mhz = -1;
        int n = 0;
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsA(dev, i, &dm)) {
            break;
        }
        if (dm.dmPelsWidth <= 0 || dm.dmPelsHeight <= 0) {
            continue;
        }
        if (dm.dmDisplayFrequency > 1) {
            mhz = (int)dm.dmDisplayFrequency * 1000;
        }
        n = snprintf(g_modes + pos, sizeof(g_modes) - pos, "%s%d|%d|%d",
                     count > 0 ? "\n" : "", (int)dm.dmPelsWidth,
                     (int)dm.dmPelsHeight, mhz);
        if (n < 0 || (size_t)n >= sizeof(g_modes) - pos) {
            break;
        }
        pos += (size_t)n;
        count++;
        if (count >= DISPLAY_MAX_MODES) {
            break;
        }
    }
    return g_modes;
}

static void wide_to_utf8(const WCHAR *w, char *out, size_t outsz) {
    if (out == 0 || outsz == 0) {
        return;
    }
    out[0] = '\0';
    if (w == 0 || w[0] == 0) {
        return;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outsz, 0, 0) == 0) {
        out[0] = '\0';
        return;
    }
    out[outsz - 1] = '\0';
    sanitize_field(out);
}

static const char *connector_label(int tech) {
    /* DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY subset; anything else is "".
     * Embedded LVDS/UDI panels and INTERNAL links report as internal
     * laptop displays. */
    if (tech == (int)0x80000000) { /* INTERNAL */
        return "internal";
    }
    switch (tech) {
    case 1: /* HD15 */
        return "vga";
    case 5: /* DVI */
        return "dvi";
    case 6: /* HDMI */
        return "hdmi";
    case 7: /* LVDS */
        return "internal";
    case 10: /* DISPLAYPORT_EXTERNAL */
        return "dp";
    case 11: /* DISPLAYPORT_EMBEDDED */
        return "edp";
    case 13: /* UDI_EMBEDDED */
        return "internal";
    default:
        return "";
    }
}

struct display_path_info {
    int tech;
    char label[128];
    char edid_make[16];
    char edid_model[128];
    int hdr; /* 1 HDR on, 0 SDR, -1 unknown */
};

/* Derives EDID-style identity from a monitor device path
 * ("\\?\DISPLAY#BOE0823#...#{...}"): first 3 chars are the manufacturer
 * trigraph, the rest the product code. Strict alnum validation; outputs
 * stay "" when the path has an unexpected shape. */
static void parse_monitor_devpath(const char *devpath, char *make,
                                  size_t makesz, char *model,
                                  size_t modelz) {
    const char *p = 0;
    const char *hash = 0;
    size_t n = 0;
    size_t i = 0;
    if (make != 0 && makesz > 0) {
        make[0] = '\0';
    }
    if (model != 0 && modelz > 0) {
        model[0] = '\0';
    }
    if (devpath == 0) {
        return;
    }
    p = strstr(devpath, "DISPLAY#");
    if (p == 0) {
        return;
    }
    p += 8;
    hash = strchr(p, '#');
    if (hash == 0) {
        return;
    }
    n = (size_t)(hash - p);
    if (n < 4 || n > 32) {
        return;
    }
    for (i = 0; i < n; i++) {
        char c = p[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9');
        if (!ok) {
            return;
        }
    }
    if (make != 0 && makesz >= 4) {
        make[0] = (char)(p[0] >= 'a' ? p[0] - 32 : p[0]);
        make[1] = (char)(p[1] >= 'a' ? p[1] - 32 : p[1]);
        make[2] = (char)(p[2] >= 'a' ? p[2] - 32 : p[2]);
        make[3] = '\0';
    }
    if (model != 0 && modelz > 0 && n > 3) {
        size_t mlen = n - 3;
        if (mlen >= modelz) {
            mlen = modelz - 1;
        }
        memcpy(model, p + 3, mlen);
        model[mlen] = '\0';
        sanitize_field(model);
    }
}

static void query_display_config(const char *dev, struct display_path_info *pi) {
    UINT32 npath = 0, nmode = 0;
    DISPLAYCONFIG_PATH_INFO *paths = 0;
    DISPLAYCONFIG_MODE_INFO *modes = 0;
    LONG rc = ERROR_NOT_SUPPORTED;
    pi->tech = -1;
    pi->label[0] = '\0';
    pi->edid_make[0] = '\0';
    pi->edid_model[0] = '\0';
    pi->hdr = -1;
    if (dev == 0 || dev[0] == '\0') {
        return;
    }
    rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &npath, &nmode);
    if (rc != ERROR_SUCCESS || npath == 0 || npath > 64) {
        return;
    }    paths = (DISPLAYCONFIG_PATH_INFO *)malloc(
        sizeof(DISPLAYCONFIG_PATH_INFO) * npath);
    modes = (DISPLAYCONFIG_MODE_INFO *)malloc(
        sizeof(DISPLAYCONFIG_MODE_INFO) * (nmode > 0 ? nmode : 1));
    if (paths == 0 || modes == 0) {
        free(paths);
        free(modes);
        return;
    }
    rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &npath, paths, &nmode,
                            modes, 0);
    if (rc == ERROR_SUCCESS) {
        UINT32 k = 0;
        for (k = 0; k < npath; k++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
            memset(&src, 0, sizeof(src));
            src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size = sizeof(src);
            src.header.adapterId = paths[k].sourceInfo.adapterId;
            src.header.id = paths[k].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS) {
                char gdi[64];
                wide_to_utf8(src.viewGdiDeviceName, gdi, sizeof(gdi));
                if (strcmp(gdi, dev) == 0) {
                    DISPLAYCONFIG_TARGET_DEVICE_NAME tgt;
                    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO adv;
                    pi->tech =
                        (int)paths[k].targetInfo.outputTechnology;
                    memset(&tgt, 0, sizeof(tgt));
                    tgt.header.type =
                        DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                    tgt.header.size = sizeof(tgt);
                    tgt.header.adapterId = paths[k].targetInfo.adapterId;
                    tgt.header.id = paths[k].targetInfo.id;
                    if (DisplayConfigGetDeviceInfo(&tgt.header) ==
                        ERROR_SUCCESS) {
                        char devpath[256];
                        wide_to_utf8(tgt.monitorFriendlyDeviceName,
                                     pi->label, sizeof(pi->label));
                        /* Friendly names are often empty (as here); the
                         * device path still carries EDID make/product. */
                        wide_to_utf8(tgt.monitorDevicePath, devpath,
                                     sizeof(devpath));
                        parse_monitor_devpath(devpath, pi->edid_make,
                                              sizeof(pi->edid_make),
                                              pi->edid_model,
                                              sizeof(pi->edid_model));
                    }
                    memset(&adv, 0, sizeof(adv));
                    adv.header.type =
                        DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
                    adv.header.size = sizeof(adv);
                    adv.header.adapterId = paths[k].targetInfo.adapterId;
                    adv.header.id = paths[k].targetInfo.id;
                    if (DisplayConfigGetDeviceInfo(&adv.header) ==
                        ERROR_SUCCESS) {
                        pi->hdr = adv.advancedColorEnabled ? 1 : 0;
                    }
                    break;
                }
            }
        }
    }
    free(paths);
    free(modes);
}

static char g_extra[1024];

const char *display_native_extra(int index) {
    HMONITOR items[DISPLAY_MAX_MONITORS];
    MONITORINFOEXA mi;
    struct display_path_info pi;
    char dev[64];
    char gpu[192];
    int n = 0;
    if (index < 0) {
        return "";
    }
    n = enum_monitors(items, DISPLAY_MAX_MONITORS);
    if (index >= n) {
        return "";
    }
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(items[index], (LPMONITORINFO)&mi)) {
        return "";
    }
    strncpy(dev, mi.szDevice, sizeof(dev) - 1);
    dev[sizeof(dev) - 1] = '\0';
    /* Adapter label via the adapter enumeration (EnumDisplayDevices on a
     * device name returns the *monitor*; adapters enumerate from NULL and
     * match on DeviceName). e.g. "Intel(R) UHD Graphics". */
    gpu[0] = '\0';
    {
        DISPLAY_DEVICEA ad;
        int ai = 0;
        for (ai = 0; ai < 32; ai++) {
            memset(&ad, 0, sizeof(ad));
            ad.cb = sizeof(ad);
            if (!EnumDisplayDevicesA(0, ai, &ad, 0)) {
                break;
            }
            if (strcmp(ad.DeviceName, dev) == 0 &&
                ad.DeviceString[0] != '\0') {
                strncpy(gpu, ad.DeviceString, sizeof(gpu) - 1);
                gpu[sizeof(gpu) - 1] = '\0';
                sanitize_field(gpu);
                break;
            }
        }
    }
    query_display_config(dev, &pi);
    /* Windows exposes no EDID blob or wide-gamut query at this layer:
     * make/product come from the monitor device path, wide stays -1. */
    snprintf(g_extra, sizeof(g_extra),
             "%d|%d|%d|%d|%s|%s|%s|%s|%s||%d|%d",
             (int)mi.rcWork.left, (int)mi.rcWork.top,
             (int)(mi.rcWork.right - mi.rcWork.left),
             (int)(mi.rcWork.bottom - mi.rcWork.top),
             pi.tech >= 0 || pi.tech == (int)0x80000000
                 ? connector_label(pi.tech)
                 : "",
             gpu, pi.label, pi.edid_make, pi.edid_model, -1, pi.hdr);
    return g_extra;
}
