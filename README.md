# PMC

Pressure-based needle-valve controller firmware for the MikroE Mikromedia 4 (STM32F407ZG), built in NECTO Studio 7 (mikroSDK, LVGL 9.4). The project is in `InitialTesting/`.

## Build notes

- `InitialTesting/pmc_config.h` sets the PC link: `PMC_LINK_ETHERNET 1` gives TCP port 5000 (DHCP, or 169.254.10.50 if no DHCP server answers), and `0` gives the USB COM port. The two can't run together, because RMII needs 50 MHz from MCO1 and USB needs 48 MHz from the same PLL.
- The linker script `.meproject/setup/FileLinker/stm32f407zg.ld` is patched so `.ccmram` is `(NOLOAD)`, which keeps the LVGL heap that lives there out of the flash image. If NECTO ever regenerates this file, re-apply the patch.
- Build Debug (-O0) in NECTO. Optimised builds (Release -O3, or -Os) leave the display blank: the mikroSDK GPIO register struct in `.meproject/setup/*/include/hal_ll_port/hal_ll_gpio_port.h` is not `volatile`, so the compiler merges `WR_LOW; WR_HIGH` into one store and the SSD1963 gets no write strobe (found on the bench, 0.3.33). Debug is about 650 KB of flash.
- NECTO's build folders (`build-MM4/`, `build-Default Configuration/`, ...), `compile_commands.json` and the clangd index in `.cache/` are not tracked, so a NECTO build no longer blocks `git pull`. The one exception is `build-MM4/generated/` (screen code from `main_screen.mscr`), which `tools/ui-preview` compiles. NECTO also rewrites `.meproject/config.json` (it holds machine paths, but also the board and MCU selection, so it stays tracked): before a pull, `git stash` or `git checkout -- InitialTesting/.meproject/config.json`.

## PC link commands

Connect to the USB COM port (any baud rate, it is virtual) or, in the Ethernet build, TCP port 5000 (e.g. PuTTY in Raw mode). Send one command per line; CR, LF or CRLF all end a line. Commands are upper case and exact. The board echoes each line it receives as `> <line>`. Spaces at either end are ignored. Each reply ends with `OK`, or is one `ERR ...` line saying what was expected. Type `HELP` (or `?`) on the board for the groups, then `HELP SYS`, `HELP CFG`, `HELP CAL`, `HELP VALVE` or `HELP MOT`.

Unprompted output: a `DATA,t_ms,p_mbar,signal_uV,exc_mV,ratio_mV_per_V,die_C,set_mV,ok` line once a second (after `DATA ON`; off at boot), status lines when they change, and `LIM closed` / `LIM OPEN` when the limit switch changes, and `F-13 motion timeout: ...` if a move runs longer than its distance / rate plus the ramps plus 2 s (the motor is stopped at once).

| Command | What it does |
| --- | --- |
| `HELP` or `?` | List the command groups; `HELP SYS`, `HELP CFG`, `HELP CAL`, `HELP VALVE`, `HELP MOT` list one group each |
| `VER?` | Firmware version and build time |
| `DATA OFF` / `DATA ON` | Pause or resume the DATA and status lines; off at boot (the logger sends DATA ON) |
| `TIME?` | Read the real-time clock |
| `TIME SET YYYY-MM-DD HH:MM:SS` | Set the clock, e.g. `TIME SET 2026-10-13 14:30:00` |
| `LIM?` | Valve limit switch: state, raw pin level, change count |
| `UNC?` | Pressure uncertainty budget, term by term |
| `TOUCH?` | Touch controller read and error counters |
| `PERF?` | Longest main loop and display timings since the last `PERF?`, and the LVGL heap (total, used, peak since boot, biggest free block, fragmentation) |
| `PM DIAG` | Steps the excitation from 9 to 11 V, one step a second, then puts it back (only when `PMC_PM_DIAG` is 1) |

Settings store (motor, valve and pressure cal in one record, serial flash). Changes are live at once and unsaved until `CFG SAVE`. At power-up the newest valid record is loaded; with none, the factory defaults (and a cal saved by firmware 0.3.30 to 0.5.x, if any):

| Command | What it does |
| --- | --- |
| `CFG?` | Both flash sectors (revision, valid or why not), what was loaded at power-up, number of unsaved settings |
| `CFG DIFF?` | Each setting that differs from the newest save |
| `CFG SAVE` | Save everything. Two sectors are used in turn; a save is read back before it counts, so a failed save keeps the last good record |
| `CFG LOAD LAST` / `CFG LOAD PREVIOUS` | Load the newest / the other saved record (shows as unsaved until saved). Refused while moving or calibrating |
| `CFG LOAD DEFAULTS` | Factory motor and valve settings; the cal is kept |
| `CFG TEST CORRUPT` / `CFG TEST ERASE` | Bench tests: spoil the newest record's CRC / erase both sectors |

Calibration (Druck, through the INA228):

| Command | What it does |
| --- | --- |
| `CAL?` | Active cal, whether it is saved, and the bench points held |
| `CAL VAC [mbar] [unc]` | Capture the vacuum point over 10 readings. Defaults: 0 mbar, ±0.010 mbar. Range 0 to 50 mbar |
| `CAL AIR <mbar> [unc]` | Capture the air point at the reference reading, 500 to 1100 mbar. Default ±1.0 mbar |
| `CAL SAVE` | Same as `CFG SAVE`: saves all settings, the active cal included (it is only in RAM until then) |
| `CAL ERASE` | Go back to nominal, erase the old 0.5.x cal record and save |
| `CAL NEW [id]`, `CAL PT <mV/V> <mbar>`, `CAL APPLY` | Enter a table by hand (up to 32 points), then make it active |

A bench cal: pump down, `CAL VAC`; vent, `CAL AIR 1013.2` (your reference reading); check with `CAL?`; then `CAL SAVE`. Either point can be redone on its own.

Valve (calibration, position, STOP, faults). Nothing moves at power-up; the valve is NOT CALIBRATED until `VALVE CAL`:

| Command | What it does |
| --- | --- |
| `VALVE?` | Mode, count and % open, settings, latched faults, any waiting switch question |
| `VALVE CAL` | Calibrate: off the switch if pressed, fast close to the switch, back off, slow close; the switch point is 0 % |
| `VALVE GOTO <0-100>` | Go to % open (one decimal), once calibrated |
| `VALVE SETZERO` / `VALVE SKIP` | After "closed switch pressed" (W-01) during a move: call this point 0 %, or keep the count |
| `VALVE POSMAX\|FHOME\|FCREEP\|BACKOFF\|CLEAR\|OFFSET <n>` | Settings: POS_MAX (99072), calibration speeds (400, 25 steps/s), back-off (400), clearance (800), switch count (0). `CFG SAVE` keeps them |
| `CTRL STOP` | Stop now and latch F-12; cancels calibration |
| `CTRL RESET` | Clear the latched faults whose cause has gone |
| `CTRL?` | Latched faults, highest priority first |
| `EVT? [1-200]` | The last events (default 20). Each event is also sent as it happens: `EVT <s> <tag> <text>` |

Faults latch, stop the motor and refuse VALVE and MOT moves until `CTRL RESET`: F-01 no Druck signal, F-03 excitation outside 9.5 to 10.5 V, F-07 switch not found while calibrating, F-09 switch still pressed after moving off it, F-13 motion timeout, F-12 STOP. F-01 and F-03 only reset once the cause has gone, so with no Druck connected the motor cannot be moved.

Valve motor (Stepper 3, engineering; refused while calibrating or with a fault latched, except `MOT STOP` and `MOT OFF`):

| Command | What it does |
| --- | --- |
| `MOT?` | Position, rates, mode, coils, limit |
| `MOT MOVE <steps>` | Move relative, 1 to 400000 steps; positive opens, negative closes. Closing is refused while the limit reads open. During a move, relative to where that move ends |
| `MOT GOTO <pos> [sps]` | Move to an absolute position, at its own rate if given (16 to 1000). During a move it retargets: a target further on extends the move, one behind ramps down and turns round |
| `MOT STOP` | Stop at once (coils stay on if hold is on) |
| `MOT STOP RAMP` | Ramp down at the normal deceleration, then stop |
| `MOT OFF` | Stop and de-energise the coils |
| `MOT ZERO` | Call the current position 0 |
| `MOT OVERRIDE` | Let the next move close past an open limit switch (one move, not saved); `MOT OVERRIDE OFF` cancels |
| `MOT RATE <16-1000>` | Run rate, steps/s |
| `MOT START <16-1000>` | Start rate, steps/s |
| `MOT ACCEL <10-20000>` | Acceleration, steps/s² |
| `MOT MODE WAVE\|FULL\|HALF` | Step sequence |
| `MOT HOLD 0\|1` | Keep the coils on when stopped |
| `MOT DIR 0\|1` | Reverse the direction |
| `MOT ORDER <abcd>` | Coil order, digits 0 to 3 once each, e.g. `MOT ORDER 0123` |

`CFG SAVE` keeps the motor settings (`RATE` to `ORDER`).

The settings commands (`RATE` to `ORDER`, and `ZERO`) answer `ERR busy, MOT STOP first` during a move.

## Tools

- `tools/logger/log_druck.py <IP or COM>` logs the once-a-second DATA lines to CSV and reconnects on its own.
- `tools/cal/druck_cal.py <IP or COM> show|zs|load|erase` manages the Druck calibration table in serial flash.
- `tools/ui-preview/build.sh` renders the main screen to PNG on a PC.
- `tools/xbuild/build.sh [Release|Debug]` cross-builds the firmware on Linux (cmake, ninja, Ubuntu `gcc-arm-none-eabi` and `libnewlib-arm-none-eabi`) with the mikroSDK libraries in `.meproject` and prints flash and RAM use. For size checks only; the flashed build is NECTO's.
