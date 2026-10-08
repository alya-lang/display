#include "display.h"
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISPLAY_MAX_SCREENS 32
#define DISPLAY_MAX_MODES 256

/* Objective-C runtime C API, declared manually (same pattern as the gui
 * package's cocoa_window.c): no ObjC syntax, so this file compiles with a
 * plain C compiler. Used for NSScreen work-area/EDR queries (AppKit). */
typedef struct objc_class *alya_Class;
typedef struct objc_object *alya_id;
typedef const struct objc_selector *alya_SEL;
typedef signed char alya_BOOL;

extern alya_Class objc_getClass(const char *name);
extern alya_SEL sel_registerName(const char *name);
extern alya_id objc_msgSend(alya_id self, alya_SEL op, ...);
extern double objc_msgSend_fpret(alya_id self, alya_SEL op, ...);
#if defined(__x86_64__)
extern void objc_msgSend_stret(void *st, alya_id self, alya_SEL op, ...);
#endif

typedef struct alya_NSRect {
    double x;
    double y;
    double w;
    double h;
} alya_NSRect;

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

/* ---- Display modes, work area, EDID, GPU, HDR ---- */

static char g_modes[16384];

const char *display_native_modes_all(int disp) {
    CGDirectDisplayID list[DISPLAY_MAX_SCREENS];
    CFArrayRef modes = 0;
    uint32_t n = 0;
    CFIndex count = 0;
    CFIndex i = 0;
    size_t pos = 0;
    int kept = 0;
    g_modes[0] = '\0';
    if (disp < 0) {
        return "";
    }
    if (CGGetActiveDisplayList(DISPLAY_MAX_SCREENS, list, &n) !=
        kCGErrorSuccess) {
        return "";
    }
    if ((uint32_t)disp >= n) {
        return "";
    }
    modes = CGDisplayCopyAllDisplayModes(list[disp], 0);
    if (modes == 0) {
        return "";
    }
    count = CFArrayGetCount(modes);
    for (i = 0; i < count && kept < DISPLAY_MAX_MODES; i++) {
        CGDisplayModeRef m =
            (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        if (m != 0) {
            size_t w = CGDisplayModeGetWidth(m);
            size_t h = CGDisplayModeGetHeight(m);
            double rate = CGDisplayModeGetRefreshRate(m);
            int mhz = (rate > 0.0) ? (int)(rate * 1000.0 + 0.5) : -1;
            int len = 0;
            if (w == 0 || h == 0) {
                continue;
            }
            len = snprintf(g_modes + pos, sizeof(g_modes) - pos,
                           "%s%lu|%lu|%d", kept > 0 ? "\n" : "",
                           (unsigned long)w, (unsigned long)h, mhz);
            if (len < 0 || (size_t)len >= sizeof(g_modes) - pos) {
                break;
            }
            pos += (size_t)len;
            kept++;
        }
    }
    CFRelease(modes);
    return g_modes;
}

/* EDID parsing (base block): manufacturer trigraph, product code, serial,
 * week/year stamps, and the four 18-byte descriptors (0xFC monitor name,
 * 0xFF serial text). Returns 1 when the header magic is valid. */
static void edid_text(const uint8_t *p, char *out, size_t outsz) {
    size_t n = 0;
    size_t end = 0;
    if (out == 0 || outsz == 0) {
        return;
    }
    out[0] = '\0';
    if (p == 0) {
        return;
    }
    /* Text is space/0x0A padded; cut at the first NUL/line-feed. */
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
    /* Manufacturer trigraph: three 5-bit letters, A=1. */
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
    /* Numeric serial as fallback; descriptor text wins when present. */
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
            continue; /* timing descriptor, not text */
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

/* Looks up the NSScreen for a display id, returning its backing scale and
 * visible frame (points). Matches via deviceDescription NSScreenNumber,
 * which is the CGDirectDisplayID. Returns 1 on match. */
static int cocoa_screen_info(CGDirectDisplayID d, double *scale,
                             alya_NSRect *vis) {
    alya_Class scrCls = 0;
    alya_Class poolCls = 0;
    alya_id pool = 0;
    alya_id screens = 0;
    unsigned long n = 0;
    unsigned long i = 0;
    int found = 0;
    scrCls = objc_getClass("NSScreen");
    if (scrCls == 0) {
        return 0;
    }
    poolCls = objc_getClass("NSAutoreleasePool");
    if (poolCls != 0) {
        pool = ((alya_id(*)(alya_id, alya_SEL))objc_msgSend)(
            (alya_id)poolCls, sel_registerName("alloc"));
        if (pool != 0) {
            pool = ((alya_id(*)(alya_id, alya_SEL))objc_msgSend)(
                pool, sel_registerName("init"));
        }
    }
    screens = ((alya_id(*)(alya_id, alya_SEL))objc_msgSend)(
        (alya_id)scrCls, sel_registerName("screens"));
    if (screens != 0) {
        n = ((unsigned long (*)(alya_id, alya_SEL))objc_msgSend)(
            screens, sel_registerName("count"));
        for (i = 0; i < n; i++) {
            alya_id scr = ((alya_id(*)(alya_id, alya_SEL,
                                       unsigned long))objc_msgSend)(
                screens, sel_registerName("objectAtIndex:"), i);
            alya_id desc = ((alya_id(*)(alya_id, alya_SEL))objc_msgSend)(
                scr, sel_registerName("deviceDescription"));
            alya_Class strCls = objc_getClass("NSString");
            alya_id key = 0;
            alya_id num = 0;
            unsigned int nd = 0;
            if (desc == 0 || strCls == 0) {
                continue;
            }
            key = ((alya_id(*)(alya_id, alya_SEL,
                               const char *))objc_msgSend)(
                (alya_id)strCls,
                sel_registerName("stringWithUTF8String:"),
                "NSScreenNumber");
            num = ((alya_id(*)(alya_id, alya_SEL, alya_id))objc_msgSend)(
                desc, sel_registerName("objectForKey:"), key);
            if (num == 0) {
                continue;
            }
            nd = ((unsigned int (*)(alya_id, alya_SEL))objc_msgSend)(
                num, sel_registerName("unsignedIntValue"));
            if (nd != (unsigned int)d) {
                continue;
            }
            {
                double s = 1.0;
#if defined(__x86_64__)
                s = objc_msgSend_fpret(
                    scr, sel_registerName("backingScaleFactor"));
#else
                s = ((double (*)(alya_id, alya_SEL))objc_msgSend)(
                    scr, sel_registerName("backingScaleFactor"));
#endif
                if (s > 0.0 && scale != 0) {
                    *scale = s;
                }
            }
            if (vis != 0) {
#if defined(__x86_64__)
                objc_msgSend_stret(vis, scr,
                                   sel_registerName("visibleFrame"));
#else
                *vis = ((alya_NSRect(*)(alya_id, alya_SEL))objc_msgSend)(
                    scr, sel_registerName("visibleFrame"));
#endif
            }
            found = 1;
            break;
        }
    }
    if (pool != 0) {
        ((void (*)(alya_id, alya_SEL))objc_msgSend)(
            pool, sel_registerName("drain"));
    }
    return found;
}

/* Extended dynamic range headroom (>1.0 means HDR-capable): 1 HDR, 0 SDR,
 * -1 when the selector is unavailable (older macOS) or no screen matches. */
static int cocoa_hdr_for(CGDirectDisplayID d) {
    alya_Class scrCls = 0;
    alya_Class poolCls = 0;
    alya_id pool = 0;
    alya_id screens = 0;
    unsigned long n = 0;
    unsigned long i = 0;
    int hdr = -1;
    scrCls = objc_getClass("NSScreen");
    if (scrCls == 0) {
        return -1;
    }
    poolCls = objc_getClass("NSAutoreleasePool");
    if (poolCls != 0) {
        pool = ((alya_id(*)(alya_id, alya_SEL))objc_msgSend)(
            (alya_id)poolCls, sel_registerName("alloc"));
        if (pool != 0) {
            pool = ((alya_id(*)(alya_id, alya_SEL))objc_msgSend)(
                pool, sel_registerName("init"));
        }
    }
    screens = ((alya_id(*)(alya_id, alya_SEL))objc_msgSend)(
        (alya_id)scrCls, sel_registerName("screens"));
    if (screens != 0) {
        n = ((unsigned long (*)(alya_id, alya_SEL))objc_msgSend)(
            screens, sel_registerName("count"));
        for (i = 0; i < n; i++) {
            alya_id scr = ((alya_id(*)(alya_id, alya_SEL,
                                       unsigned long))objc_msgSend)(
                screens, sel_registerName("objectAtIndex:"), i);
            alya_id desc = ((alya_id(*)(alya_id, alya_SEL))objc_msgSend)(
                scr, sel_registerName("deviceDescription"));
            alya_Class strCls = objc_getClass("NSString");
            alya_id key = 0;
            alya_id num = 0;
            unsigned int nd = 0;
            alya_SEL edrSel = 0;
            alya_BOOL responds = 0;
            double v = 1.0;
            if (desc == 0 || strCls == 0) {
                continue;
            }
            key = ((alya_id(*)(alya_id, alya_SEL,
                               const char *))objc_msgSend)(
                (alya_id)strCls,
                sel_registerName("stringWithUTF8String:"),
                "NSScreenNumber");
            num = ((alya_id(*)(alya_id, alya_SEL, alya_id))objc_msgSend)(
                desc, sel_registerName("objectForKey:"), key);
            if (num == 0) {
                continue;
            }
            nd = ((unsigned int (*)(alya_id, alya_SEL))objc_msgSend)(
                num, sel_registerName("unsignedIntValue"));
            if (nd != (unsigned int)d) {
                continue;
            }
            edrSel = sel_registerName(
                "maximumExtendedDynamicRangeColorComponentValue");
            responds = ((alya_BOOL(*)(alya_id, alya_SEL,
                                      alya_SEL))objc_msgSend)(
                scr, sel_registerName("respondsToSelector:"), edrSel);
            if (!responds) {
                break;
            }
#if defined(__x86_64__)
            v = objc_msgSend_fpret(scr, edrSel);
#else
            v = ((double (*)(alya_id, alya_SEL))objc_msgSend)(scr, edrSel);
#endif
            hdr = (v > 1.0) ? 1 : 0;
            break;
        }
    }
    if (pool != 0) {
        ((void (*)(alya_id, alya_SEL))objc_msgSend)(
            pool, sel_registerName("drain"));
    }
    return hdr;
}

/* Best-effort GPU label: first IOFramebuffer's parent accelerator/PCI
 * device "model" ("" on Apple Silicon VMs/headless where none matches). */
static void cocoa_gpu_label(char *out, size_t outsz) {
    io_iterator_t it = 0;
    io_object_t fb = 0;
    if (out == 0 || outsz == 0) {
        return;
    }
    out[0] = '\0';
    if (IOServiceGetMatchingServices(kIOMasterPortDefault,
                                     IOServiceMatching("IOFramebuffer"),
                                     &it) != KERN_SUCCESS) {
        return;
    }
    while ((fb = IOIteratorNext(it)) != 0) {
        io_object_t parent = 0;
        if (IORegistryEntryGetParentEntry(fb, kIOServicePlane, &parent) ==
            KERN_SUCCESS) {
            CFTypeRef m = IORegistryEntryCreateCFProperty(
                parent, CFSTR("model"), kCFAllocatorDefault, 0);
            if (m != 0) {
                if (CFGetTypeID(m) == CFDataGetTypeID()) {
                    CFIndex len = CFDataGetLength((CFDataRef)m);
                    const UInt8 *bytes = CFDataGetBytePtr((CFDataRef)m);
                    size_t n = 0;
                    if (len > 0 && bytes != 0) {
                        /* "model" is NUL-terminated ASCII ("AMD Radeon Pro..."). */
                        while (n + 1 < outsz && (CFIndex)n < len - 1 &&
                               bytes[n] != '\0') {
                            char c = (char)bytes[n];
                            out[n] = (c >= 32 && c < 127) ? c : ' ';
                            n++;
                        }
                        out[n] = '\0';
                        sanitize_field(out);
                    }
                }
                CFRelease(m);
            }
            IOObjectRelease(parent);
        }
        IOObjectRelease(fb);
        if (out[0] != '\0') {
            break;
        }
    }
    IOObjectRelease(it);
}

struct cocoa_edid_hit {
    char make[16];
    char model[128];
    char serial[64];
    char label[128];
};

/* Walks IODisplayConnect entries, matching vendor/product(/serial) against
 * the CG display numbers; fills EDID fields plus the localized product
 * name. All outputs stay "" when nothing matches (headless/VMs). */
static void cocoa_edid_for(CGDirectDisplayID d, struct cocoa_edid_hit *hit) {
    uint32_t vendor = CGDisplayVendorNumber(d);
    uint32_t model = CGDisplayModelNumber(d);
    uint32_t serial = CGDisplaySerialNumber(d);
    io_iterator_t it = 0;
    io_object_t e = 0;
    hit->make[0] = '\0';
    hit->model[0] = '\0';
    hit->serial[0] = '\0';
    hit->label[0] = '\0';
    if (IOServiceGetMatchingServices(kIOMasterPortDefault,
                                     IOServiceMatching("IODisplayConnect"),
                                     &it) != KERN_SUCCESS) {
        return;
    }
    while ((e = IOIteratorNext(it)) != 0) {
        CFTypeRef edid = IORegistryEntryCreateCFProperty(
            e, CFSTR("IODisplayEDID"), kCFAllocatorDefault, 0);
        if (edid != 0 && CFGetTypeID(edid) == CFDataGetTypeID()) {
            CFIndex len = CFDataGetLength((CFDataRef)edid);
            const UInt8 *bytes = CFDataGetBytePtr((CFDataRef)edid);
            if (len >= 128 && bytes != 0) {
                char make[16] = {0};
                char mname[128] = {0};
                char sname[64] = {0};
                /* Product code lives at fixed offsets; it is the binding
                 * key (EDID trigraphs and CG numeric vendor ids share no
                 * namespace, so vendor cannot cross-check here). */
                unsigned prod =
                    (unsigned)bytes[10] | ((unsigned)bytes[11] << 8);
                if (prod == (model & 0xFFFF)) {
                    if (parse_edid(bytes, (size_t)len, make, sizeof(make),
                                   mname, sizeof(mname), sname,
                                   sizeof(sname))) {
                        /* Serial match required only when both sides know it. */
                        int serial_ok = 1;
                        if (serial != 0 && sname[0] != '\0' &&
                            sname[0] >= '0' && sname[0] <= '9') {
                            char want[64];
                            snprintf(want, sizeof(want), "%u",
                                     (unsigned)serial);
                            serial_ok = strcmp(sname, want) == 0;
                        }
                        if (serial_ok) {
                            CFTypeRef pname =
                                IORegistryEntryCreateCFProperty(
                                    e, CFSTR("DisplayProductName"),
                                    kCFAllocatorDefault, 0);
                            strncpy(hit->make, make, sizeof(hit->make) - 1);
                            strncpy(hit->model, mname,
                                    sizeof(hit->model) - 1);
                            strncpy(hit->serial, sname,
                                    sizeof(hit->serial) - 1);
                            hit->make[sizeof(hit->make) - 1] = '\0';
                            hit->model[sizeof(hit->model) - 1] = '\0';
                            hit->serial[sizeof(hit->serial) - 1] = '\0';
                            if (pname != 0) {
                                if (CFGetTypeID(pname) ==
                                    CFDictionaryGetTypeID()) {
                                    CFStringRef loc =
                                        (CFStringRef)CFDictionaryGetValue(
                                            (CFDictionaryRef)pname,
                                            CFSTR("en_US"));
                                    if (loc == 0) {
                                        loc = (CFStringRef)CFDictionaryGetValue(
                                            (CFDictionaryRef)pname,
                                            CFSTR("en"));
                                    }
                                    if (loc != 0 &&
                                        CFGetTypeID(loc) ==
                                            CFStringGetTypeID()) {
                                        char tmp[128];
                                        if (CFStringGetCString(
                                                loc, tmp, sizeof(tmp),
                                                kCFStringEncodingUTF8)) {
                                            strncpy(hit->label, tmp,
                                                    sizeof(hit->label) - 1);
                                            hit->label[sizeof(hit->label) -
                                                       1] = '\0';
                                            sanitize_field(hit->label);
                                        }
                                    }
                                } else if (CFGetTypeID(pname) ==
                                           CFStringGetTypeID()) {
                                    char tmp[128];
                                    if (CFStringGetCString(
                                            (CFStringRef)pname, tmp,
                                            sizeof(tmp),
                                            kCFStringEncodingUTF8)) {
                                        strncpy(hit->label, tmp,
                                                sizeof(hit->label) - 1);
                                        hit->label[sizeof(hit->label) - 1] =
                                            '\0';
                                        sanitize_field(hit->label);
                                    }
                                }
                                CFRelease(pname);
                            }
                            CFRelease(edid);
                            IOObjectRelease(e);
                            /* First product match wins: identical twin
                             * monitors share vendor+product, so the serial
                             * check above is the only disambiguator. */
                            (void)vendor;
                            break;
                        }
                    }
                }
            }
            CFRelease(edid);
        }
        IOObjectRelease(e);
    }
    IOObjectRelease(it);
}

static char g_extra[1024];

const char *display_native_extra(int index) {
    CGDirectDisplayID list[DISPLAY_MAX_SCREENS];
    CGDirectDisplayID d = 0;
    CGColorSpaceRef cs = 0;
    uint32_t n = 0;
    struct cocoa_edid_hit ed;
    char gpu[192];
    char color[192];
    double scale = 1.0;
    alya_NSRect vis = {0, 0, 0, 0};
    int have_vis = 0;
    int wx = 0, wy = 0, ww = -1, wh = -1;
    int wide = -1, hdr = -1;
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
    /* Usable area: NSScreen visibleFrame (menu bar + Dock excluded),
     * converted from Cocoa points (bottom-left) to CG pixels (top-left). */
    have_vis = cocoa_screen_info(d, &scale, &vis);
    if (have_vis && scale > 0.0 && vis.w > 0.0 && vis.h > 0.0) {
        int mainH =
            (int)CGDisplayPixelsHigh(CGMainDisplayID());
        if (mainH > 0) {
            wx = (int)(vis.x * scale + 0.5);
            wy = mainH - (int)((vis.y + vis.h) * scale + 0.5);
            ww = (int)(vis.w * scale + 0.5);
            wh = (int)(vis.h * scale + 0.5);
        }
    }
    /* Wide-gamut follows the color-space label ("" stays unknown). */
    color[0] = '\0';
    cs = CGDisplayCopyColorSpace(d);
    if (cs != 0) {
        color_label(cs, color, sizeof(color));
        CGColorSpaceRelease(cs);
    }
    if (color[0] != '\0') {
        wide = (strstr(color, "P3") != 0) ? 1 : 0;
    }
    hdr = cocoa_hdr_for(d);
    cocoa_gpu_label(gpu, sizeof(gpu));
    cocoa_edid_for(d, &ed);
    snprintf(g_extra, sizeof(g_extra), "%d|%d|%d|%d|%s|%s|%s|%s|%s|%s|%d|%d",
             wx, wy, ww, wh, CGDisplayIsBuiltin(d) ? "internal" : "", gpu,
             ed.label, ed.make, ed.model, ed.serial, wide, hdr);
    return g_extra;
}
