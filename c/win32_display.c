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
