# PMC

Pressure-based needle-valve controller firmware for the MikroE Mikromedia 4 (STM32F407ZG), built in NECTO Studio 7 (mikroSDK, LVGL 9.4). The project is in `InitialTesting/`.

## Build notes

- `InitialTesting/pmc_config.h` sets the PC link: `PMC_LINK_ETHERNET 1` gives TCP port 5000 (DHCP, or 169.254.10.50 if no DHCP server answers), and `0` gives the USB COM port. The two can't run together, because RMII needs 50 MHz from MCO1 and USB needs 48 MHz from the same PLL.
- The linker script `.meproject/setup/FileLinker/stm32f407zg.ld` is patched so `.ccmram` is `(NOLOAD)`, which keeps the LVGL heap that lives there out of the flash image. If NECTO ever regenerates this file, re-apply the patch.
- Build Release in NECTO, not Debug (-O0). The display is written one pixel at a time, so an unoptimised build makes redraws slow.
- NECTO's build folders (`build-MM4/`, `build-Default Configuration/`, ...), `compile_commands.json` and the clangd index in `.cache/` are not tracked, so a NECTO build no longer blocks `git pull`. The one exception is `build-MM4/generated/` (screen code from `main_screen.mscr`), which `tools/ui-preview` compiles. NECTO also rewrites `.meproject/config.json` (it holds machine paths, but also the board and MCU selection, so it stays tracked): before a pull, `git stash` or `git checkout -- InitialTesting/.meproject/config.json`.

## PC link commands

Connect to the USB COM port (any baud rate, it is virtual) or, in the Ethernet build, TCP port 5000 (e.g. PuTTY in Raw mode). Send one command per line; CR, LF or CRLF all end a line. Commands are upper case and exact. Each reply ends with `OK`, or is one `ERR ...` line saying what was expected. Type `HELP` (or `?`) on the board for this list.

Unprompted output: a `DATA,t_ms,p_mbar,signal_uV,exc_mV,ratio_mV_per_V,die_C,set_mV,ok` line once a second, status lines when they change, and `LIM closed` / `LIM OPEN` when the limit switch changes.

| Command | What it does |
| --- | --- |
| `HELP` or `?` | List the commands |
| `VER?` | Firmware version and build time |
| `DATA OFF` / `DATA ON` | Pause or resume the DATA and status lines (handy while typing) |
| `TIME?` | Read the real-time clock |
| `TIME SET YYYY-MM-DD HH:MM:SS` | Set the clock, e.g. `TIME SET 2026-10-13 14:30:00` |
| `LIM?` | Valve limit switch: state, raw pin level, change count |
| `UNC?` | Pressure uncertainty budget, term by term |
| `TOUCH?` | Touch controller read and error counters |
| `PERF?` | Longest main loop and display timings since the last `PERF?` |
| `PM DIAG` | Steps the excitation from 9 to 11 V, one step a second, then puts it back (only when `PMC_PM_DIAG` is 1) |

Calibration (Druck, through the INA228):

| Command | What it does |
| --- | --- |
| `CAL?` | Active cal, whether it is saved, and the bench points held |
| `CAL VAC [mbar] [unc]` | Capture the vacuum point over 10 readings. Defaults: 0 mbar, ±0.010 mbar. Range 0 to 50 mbar |
| `CAL AIR <mbar> [unc]` | Capture the air point at the reference reading, 500 to 1100 mbar. Default ±1.0 mbar |
| `CAL SAVE` | Write the active cal to serial flash (it is only in RAM until then) |
| `CAL ERASE` | Erase the saved cal and go back to nominal |
| `CAL NEW [id]`, `CAL PT <mV/V> <mbar>`, `CAL APPLY` | Enter a table by hand (up to 32 points), then make it active |

A bench cal: pump down, `CAL VAC`; vent, `CAL AIR 1013.2` (your reference reading); check with `CAL?`; then `CAL SAVE`. Either point can be redone on its own.

Valve motor (Stepper 3):

| Command | What it does |
| --- | --- |
| `MOT?` | Position, rates, mode, coils, limit |
| `MOT MOVE <steps>` | Move relative, 1 to 100000 steps; positive opens, negative closes. Closing is refused while the limit reads open |
| `MOT STOP` | Stop (coils stay on if hold is on) |
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

The settings commands (`RATE` to `ORDER`, and `ZERO`) answer `ERR busy, MOT STOP first` during a move.

## Tools

- `tools/logger/log_druck.py <IP or COM>` logs the once-a-second DATA lines to CSV and reconnects on its own.
- `tools/cal/druck_cal.py <IP or COM> show|zs|load|erase` manages the Druck calibration table in serial flash.
- `tools/ui-preview/build.sh` renders the main screen to PNG on a PC.
