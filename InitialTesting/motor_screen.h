#ifndef _MOTOR_SCREEN_H_
#define _MOTOR_SCREEN_H_

#include "ui.h"

/**
 * @brief Motor override (Settings > Motor override): jog by a chosen step
 * size, Set zero, Coils off; and Motor drive behind it: every MOT setting
 * (rates, accel, mode, coil order, direction, hold). Screens on the ui.c
 * framework (header, strip and STOP as everywhere).
 */
extern const ui_screen_t ui_scr_motor, ui_scr_motor_drive;

#endif // _MOTOR_SCREEN_H_
