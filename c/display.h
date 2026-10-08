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

#endif
