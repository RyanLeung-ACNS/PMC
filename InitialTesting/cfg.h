#ifndef _CFG_H_
#define _CFG_H_

#include <stdbool.h>

/**
 * @brief Settings store (stage 3 of the porting plan): the motor settings
 * (MOT RATE ... ORDER), the valve settings (VALVE POSMAX ... OFFSET) and the
 * Druck calibration in one record with a revision number and a CRC32, in the
 * on-board serial flash (extflash.c).
 *
 * Two 4 KB sectors are used in turn: a save goes to the sector not holding
 * the newest record, is read back and checked, and only then counts as
 * saved, so a failed or interrupted save leaves the last good record. At
 * start-up the newest valid record is loaded; with none, the factory
 * defaults are used (and the old stand-alone cal record, if any) and an
 * event says so. Nothing moves at start-up either way.
 *
 * Changes made over COM3 or on the screen are live at once and stay
 * unsaved until CFG SAVE. A loaded record also comes in as live values;
 * anything differing from the newest saved record shows as unsaved.
 *
 * Commands:
 *   CFG?                         both sectors, what was loaded, unsaved count
 *   CFG DIFF?                    each setting that differs from the newest save
 *   CFG SAVE                     save everything (CAL SAVE does the same)
 *   CFG LOAD LAST|PREVIOUS       the newest / the other saved record
 *   CFG LOAD DEFAULTS            factory motor and valve settings (cal kept)
 *   CFG TEST CORRUPT             spoil the newest record's CRC (bench test)
 *   CFG TEST ERASE               erase both sectors (bench test)
 * LOAD is refused while the valve moves or calibrates.
 */

// After cal_init(), stepper3_init() and valve_init().
void cfg_init(void);
// Saves the live settings; prints OK or ERR.
bool cfg_save(void);
// Number of settings differing from the newest saved record (or, with none
// saved, from the start-up values).
int cfg_unsaved(void);
bool cfg_command(const char *line);

#endif // _CFG_H_
