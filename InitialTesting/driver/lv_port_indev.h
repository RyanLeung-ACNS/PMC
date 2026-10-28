
/**
 * @file lv_port_indev_templ.h
 *
 */

#ifndef LV_PORT_INDEV_TEMPL_H
#define LV_PORT_INDEV_TEMPL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

void lv_port_indev_init(void);
void process_tp();

// Reads the touch controller at most every 5 ms; call every main-loop pass.
void touch_poll(void);

// TOUCH?: touch reads, presses seen and the last point, on the link.
// Returns false if the line is not TOUCH?.
bool touch_command(const char *line);

// While a press is down: a second finger on the glass, and where (x, y may
// be NULL). Not given to LVGL; Motor override's Limit override reads it.
bool touch_second(int32_t *x, int32_t *y);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_PORT_INDEV_TEMPL_H*/