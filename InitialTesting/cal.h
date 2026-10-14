#ifndef _CAL_H_
#define _CAL_H_

#include <stdbool.h>
#include <stdint.h>

#include "clicks.h"

/**
 * @brief Druck calibration: a table of ratio (signal / excitation) against
 * pressure, linear interpolation between points, linear extrapolation past
 * the ends from the end segments.
 *
 * The normal cal is the bench two-point: a pumped-down vacuum point and an
 * air point at a known room pressure, both read live through the INA228 as
 * wired, so the INA228 input loading of the Druck output (about 5 % low)
 * and the INA228 gain and offset are inside the table.
 *
 * Stored in the on-board serial flash (extflash.c) with a CRC32, loaded at
 * start-up. If nothing valid is stored, the nominal 0 mV / 100 mV at 10 V
 * (15 psia) is used and cal_is_default() is true. A record from before
 * 0.3.30 (versions 1 to 3, CAL ATM era) is not loaded: nominal is used and
 * CAL? says to redo the bench cal.
 *
 * Set over COM3, one command per line:
 *   CAL?                       show the active calibration
 *   CAL VAC [mbar] [unc]       vacuum point: average the live ratio over 10
 *                              readings; pressure 0 unless given
 *   CAL AIR <mbar> [unc]       air point, same, at the given room pressure
 *                              (once both are held the table is applied,
 *                              RAM only; either can be redone alone)
 *   CAL NEW <id text>          start an entered table (id up to 23 chars)
 *   CAL PT <mV/V> <mbar>       add a point, ratio ascending
 *   CAL APPLY                  check the entered table and use it (RAM only)
 *   CAL SAVE                   write the active table to flash
 *   CAL ERASE                  erase flash and go back to the default
 * Replies start with OK or ERR.
 */

#define CAL_MAX_POINTS 32
#define CAL_ID_LEN     24

#define CAL_KIND_TABLE 0   // entered with CAL NEW / PT / APPLY, or nominal
#define CAL_KIND_BENCH 1   // CAL VAC + CAL AIR

// Default uncertainties of the two bench points, 0.001 mbar.
#define CAL_VAC_UNC_DEFAULT 10     // 0.010 mbar: pumped to about 1e-2 mbar, taken as 0
#define CAL_AIR_UNC_DEFAULT 1000   // 1.0 mbar: room barometer

void cal_init(void);
// Call once per reading; runs a CAL VAC / CAL AIR capture.
void cal_update(const clicks_state_t *s);

// Pressure in 0.001 mbar for a ratio in ppb (1 mV/V = 1 000 000 ppb).
int64_t cal_pressure_mmbar(int64_t r_ppb);

bool cal_is_default(void);
const char *cal_id(void);
// True once the active table has been written to flash (or was loaded from
// it), false after APPLY or a bench capture until CAL SAVE.
bool cal_saved(void);
// Version of an old record found in flash and not loaded, 0 if none.
uint32_t cal_old_version(void);
// Bench points of the active table (ratio ppb, pressure 0.001 mbar); false
// if the active table is not a bench cal.
bool cal_bench(int32_t *r_vac, int32_t *p_vac, int32_t *r_air, int32_t *p_air);
int32_t cal_vac_unc_mmbar(void);
int32_t cal_air_unc_mmbar(void);
// Readings still to take in a running capture, 0 if none.
int cal_capture_left(void);

// Handles one command line from COM3. Returns false if it is not a CAL command.
bool cal_command(const char *line);

#endif // _CAL_H_
