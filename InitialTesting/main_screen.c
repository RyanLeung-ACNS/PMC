#include "main_screen.h"

/*
 * The 0.6.0 main (pressure) screen was removed in 0.9.0; its sensing moved
 * to sense.c and its readings to the new screens. NECTO's LVGL Designer
 * project type still generates screens.c and scr_main_screen.c from
 * main_screen.mscr, and they need these to link. Nothing calls them.
 */
lvgl_main_screen_ui_t lvgl_main_screen_ui;

void init_main_screen(void)
{
}

void show_main_screen(void)
{
}
