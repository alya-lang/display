#include "display.h"

/* Bundled engine smoke test (exercised by alya test). */

int alya_display_add(int a, int b) {
    return a + b;
}

/* Fallback stubs for targets without a dedicated backend file.
 * Known targets get real implementations from c/win32_display.c,
 * c/cocoa_display.c, or c/linux_display.c; this guard prevents duplicate
 * symbols there while keeping exotic targets linkable (headless-safe). */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__linux__)
int display_native_count(void) {
    return 0;
}

const char *display_native_at(int index) {
    (void)index;
    return "";
}

const char *display_native_extra(int index) {
    (void)index;
    return "";
}

const char *display_native_modes_all(int disp) {
    (void)disp;
    return "";
}
#endif
