#ifndef ALYA_DISPLAY_H
#define ALYA_DISPLAY_H

int alya_display_add(int a, int b);

/* Display enumeration natives (per-OS in c/win32_display.c,
 * c/cocoa_display.c, c/linux_display.c; stubs in c/display.c for
 * unknown targets). Headless-safe: 0 / "" when no display is reachable.
 * Numeric fields use -1 when unavailable; strings return "". */
int display_native_count(void);

/* Indexed entry as "name|x|y|w|h|scale_x100|dpi|refresh_milliHz|bpp|primary|orientation|color".
 * scale_x100 is percent (100 = 1.00x, 200 = 2.00x Retina, -1 unknown).
 * refresh is milli-Hz (60000 = 60Hz, -1 unknown). orientation is 0/90/180/270.
 * color is the OS color profile/pixel-format label ("" when unknown).
 * Returns "" on failure or out-of-range index. */
const char *display_native_at(int index);

/* Extended per-display record as
 * "work_x|work_y|work_w|work_h|connector|gpu|label|edid_make|edid_model|edid_serial|wide|hdr".
 * work_* is the taskbar/dock-excluded usable area in virtual-desktop pixels
 * (work_w <= 0 when unknown — callers fall back to full bounds). connector is
 * "internal", "hdmi", "dp", "edp", "dvi", "vga" or "" when unknown. gpu is the
 * adapter/driver label ("" when unknown). label is the friendly monitor name
 * ("" when unknown). edid_* come from the monitor EDID ("" when unreadable).
 * wide is 1 when wide-gamut (P3), 0 when standard sRGB, -1 unknown; hdr is 1
 * when HDR is active/capable, 0 when SDR-only, -1 unknown.
 * Returns "" on failure or out-of-range index. */
const char *display_native_extra(int index);

/* Mode list for enumeration index disp as newline-joined "w|h|refresh_milliHz"
 * lines (raw native order, may contain duplicates across pixel formats —
 * the Alya side dedupes and sorts; capped at 256 lines). Returns "" when
 * the list is unavailable (e.g. Wayland compositors without
 * wlr-output-management expose only the current mode). One native
 * enumeration per call by design (no O(n^2) per-index roundtrips). */
const char *display_native_modes_all(int disp);

#endif
