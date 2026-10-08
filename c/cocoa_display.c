#include "display.h"
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>

#define DISPLAY_MAX_SCREENS 32

static void sanitize_field(char *s) {
    /* Pipe-split wire format: field text must never contain the separator. */
    for (; *s != '\0'; s++) {
        if (*s == '|') {
            *s = '/';
        } else if (*s == '\n' || *s == '\r') {
            *s = ' ';
        }
    }
}

int display_native_count(void) {
    uint32_t n = 0;
    if (CGGetActiveDisplayList(0, 0, &n) != kCGErrorSuccess) {
        return 0;
    }
    return (int)n;
}

static int color_label(CGColorSpaceRef cs, char *out, size_t outsz) {
    /* Maps well-known color spaces to short labels; anything else keeps its
     * registry name (sanitized). Returns 1 when out was populated. */
    CFStringRef nm = 0;
    char tmp[192];
    if (cs == 0 || out == 0 || outsz == 0) {
        return 0;
    }
    nm = CGColorSpaceGetName(cs);
    if (nm == 0) {
        return 0;
    }
    if (CFStringCompare(nm, kCGColorSpaceSRGB, 0) == kCFCompareEqualTo) {
        strncpy(out, "sRGB", outsz - 1);
    } else if (CFStringCompare(nm, kCGColorSpaceGenericRGB, 0) ==
               kCFCompareEqualTo) {
        strncpy(out, "Generic RGB", outsz - 1);
    } else if (CFStringCompare(nm, kCGColorSpaceGenericGrayGamma2_2, 0) ==
               kCFCompareEqualTo) {
        strncpy(out, "Generic Gray", outsz - 1);
    } else {
        if (!CFStringGetCString(nm, tmp, sizeof(tmp),
                               kCFStringEncodingUTF8) ||
            tmp[0] == '\0') {
            return 0;
        }
        strncpy(out, tmp, outsz - 1);
    }
    out[outsz - 1] = '\0';
    sanitize_field(out);
    /* "Display P3" arrives under a long registry name; shorten it. */
    if (strstr(out, "Display P3") != 0) {
        strncpy(out, "Display P3", outsz - 1);
        out[outsz - 1] = '\0';
    }
    return out[0] != '\0';
}

static char g_entry[1024];

const char *display_native_at(int index) {
    CGDirectDisplayID list[DISPLAY_MAX_SCREENS];
    CGDirectDisplayID d = 0;
    CGRect bounds;
    CGDisplayModeRef mode = 0;
    CGColorSpaceRef cs = 0;
    uint32_t n = 0;
    char name[96];
    char color[192];
    int x, y, w, h, scale_x100, dpi, refresh_mhz, bpp, primary, orient;
    if (index < 0) {
        return "";
    }
    if (CGGetActiveDisplayList(DISPLAY_MAX_SCREENS, list, &n) !=
        kCGErrorSuccess) {
        return "";
    }
    if ((uint32_t)index >= n) {
        return "";
    }
    d = list[index];
    bounds = CGDisplayBounds(d);
    x = (int)bounds.origin.x;
    y = (int)bounds.origin.y;
    w = (int)bounds.size.width;
    h = (int)bounds.size.height;
    if (w <= 0 || h <= 0) {
        return "";
    }
    primary = CGDisplayIsMain(d) ? 1 : 0;

    scale_x100 = 100;
    refresh_mhz = -1;
    bpp = 32;
    mode = CGDisplayCopyDisplayMode(d);
    if (mode != 0) {
        double rate = CGDisplayModeGetRefreshRate(mode);
        size_t pw = CGDisplayModeGetPixelWidth(mode);
        size_t lw = CGDisplayModeGetWidth(mode);
        CFStringRef enc = CGDisplayModeCopyPixelEncoding(mode);
        if (rate > 0.0) {
            refresh_mhz = (int)(rate * 1000.0 + 0.5);
        }
        if (lw > 0 && pw > 0) {
            scale_x100 = (int)(pw * 100 / lw);
        }
        if (enc != 0) {
            char encbuf[64];
            if (CFStringGetCString(enc, encbuf, sizeof(encbuf),
                                  kCFStringEncodingUTF8)) {
                if (strcmp(encbuf, "IO32BitDirectPixels") == 0) {
                    bpp = 32;
                } else if (strcmp(encbuf, "IO16BitDirectPixels") == 0) {
                    bpp = 16;
                } else if (strcmp(encbuf, "IO8BitIndexedPixels") == 0) {
                    bpp = 8;
                }
            }
            CFRelease(enc);
        }
        CGDisplayModeRelease(mode);
    }

    /* Physical size (mm) -> DPI; 0-size panels (projectors/VMs) stay -1. */
    dpi = -1;
    {
        CGSize mm = CGDisplayScreenSize(d);
        if (mm.width > 0.0 && w > 0) {
            dpi = (int)(w * 25.4 / mm.width + 0.5);
        }
    }

    color[0] = '\0';
    cs = CGDisplayCopyColorSpace(d);
    if (cs != 0) {
        color_label(cs, color, sizeof(color));
        CGColorSpaceRelease(cs);
    }

    {
        double rot = CGDisplayRotation(d);
        int r = (int)(rot >= 0.0 ? rot + 0.5 : rot - 0.5);
        if (r < 0) {
            r = 0;
        }
        /* Normalize to the 0/90/180/270 wire vocabulary. */
        if (r >= 45 && r < 135) {
            orient = 90;
        } else if (r >= 135 && r < 225) {
            orient = 180;
        } else if (r >= 225 && r < 315) {
            orient = 270;
        } else {
            orient = 0;
        }
    }

    snprintf(name, sizeof(name), "Display %u", (unsigned)d);
    sanitize_field(name);
    sanitize_field(color);

    snprintf(g_entry, sizeof(g_entry), "%s|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%s",
             name, x, y, w, h, scale_x100, dpi, refresh_mhz, bpp, primary,
             orient, color);
    return g_entry;
}
