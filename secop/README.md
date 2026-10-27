# mm4 Frappy driver

A [Frappy](https://github.com/SampleEnvironment/frappy) driver so a SECoP
server can read and drive the mm4. It talks to the board's text port (TCP
5000). Nothing in the firmware changes.

## What the SECoP client sees

| Module | Parameter or command | What it is |
|---|---|---|
| `pressure` | `value` | Pot pressure, mbar |
| | `target` | Setpoint, mbar, 0.1 mbar steps; used in AUTO |
| | `mode` | `MANUAL` or `AUTO` (can be set); `HOMING`, `NOT_HOMED` (shown only) |
| | `status` | IDLE in MANUAL, or in AUTO within `tolerance` of the setpoint; BUSY while AUTO is still getting there, or while homing; WARN for a warning or a valve that is not homed; ERROR for a fault or no reading |
| | `_alarm` | Latched fault `F-n` or warning `W-n`, empty if none |
| | `_tolerance` | Band for IDLE in AUTO, default 0.5 mbar (driver only, not the board's target band) |
| | `_room_pressure` | Room pressure from the Pressure 21 Click, if fitted |
| | `stop` | Stop the valve now; in AUTO this ends AUTO |
| | `_reset_alarm`, `_ok_warning` | Clear latched faults whose cause has gone; acknowledge the warning |
| `valve` | `value` | Valve position, % open |
| | `target` | Valve position to go to, MANUAL only (the board refuses it in AUTO) |
| | `stop` | As above |

Homing, calibration, settings, tuning and the motor stay on the touch
screen. The driver has no way to reach them.

## Files

- `frappy_mm4/mm4.py`: the driver (`Mm4IO`, `Pressure`, `Valve`).
- `cfg/mm4_cfg.py`: a Frappy configuration for one board.
- `tests/fake_mm4.py`: a stand-in board for testing without hardware.
- `tests/run.py`: runs the driver against the stand-in board.

## Setting it up

1. `pip install frappy-core` (tested with 0.20.4), or use the Frappy the
   server already runs.
2. Put the `secop` folder on `PYTHONPATH`, or copy `frappy_mm4` next to the
   server's other driver packages.
3. Copy `cfg/mm4_cfg.py` into the server's configuration folder. Set the
   board's address in `MM4_URI` or edit the default in the file
   (`tcp://<address shown on the screen>:5000`). Copy it once per board,
   with its own node name and SECoP port.
4. `frappy-server -c mm4_cfg.py mm4`

`command_set` in the cfg file chooses the command words: `0.12` for the
firmware as built (0.12.19 to 0.12.21), `plan` once the board has the
remote command set (`SP`, `MODE AUTO`, `STOP`, ...). The default is `0.12`.

## Things to know

- The board takes one connection on port 5000 at a time, and a new one
  replaces the old. While the Frappy server is connected, close any terminal
  and the DATA logger (`tools/logger`).
- With firmware 0.12 the driver sends `DATA ON` and reads the once-a-second
  DATA line. Values are at most a second old.
- An `ERR` from the board comes back to the SECoP client as an error with
  the board's text, for example `Switch to MANUAL first`.
- The driver has been tested against `tests/fake_mm4.py` only, not yet
  against a board.

## Testing

    pip install frappy-core
    python secop/tests/run.py

It starts the stand-in board and a Frappy server on local ports, then checks
reading, setpoint, MANUAL and AUTO, valve moves, refusals and stop, for both
command sets.
