"""Frappy (SECoP) driver for the mm4 pressure controller.

The mm4 is reached over its text port (TCP 5000). Two modules share one
connection:

    Pressure  value = pressure (mbar), target = setpoint, mode MANUAL/AUTO,
              stop, alarm reset and warning OK
    Valve     value = valve % open, target = valve % (MANUAL only; the
              board refuses it in AUTO)

Only the remote command set is used: nothing here homes the valve,
calibrates, changes a setting or drives the motor. Those stay on the touch
screen.

Readings come from the board's DATA line (t_s, p_mbar, target_mbar,
valve_pct, mode, alarm, ...). Firmware up to 0.12.21 sends it once a second
after DATA ON, so the driver turns that on and keeps the newest line; a
firmware that answers DATA? with one DATA line works the same way. The
command words for both are in COMMANDS, chosen by the 'command_set' property.
"""

import re
import time

from frappy.core import BUSY, ERROR, IDLE, WARN, Command, Drivable, EnumType, \
    FloatRange, HasIO, Parameter, Property, StringIO, StringType, nopoll
from frappy.errors import CommunicationFailedError, ImpossibleError

# Command words. '0.12' is the firmware as built (0.12.19 to 0.12.21);
# 'plan' is the remote command set with the names picked on 27 Oct 2026,
# not in the firmware yet.
COMMANDS = {
    '0.12': {
        'data_on': 'DATA ON', 'header': 'DATA?', 'data': 'DATA?',
        'target': 'CTRL SP {:.1f}', 'auto': 'CTRL AUTO', 'manual': 'CTRL MAN',
        'valve': 'VALVE GOTO {:.1f}', 'stop': 'VALVE STOP',
        'reset': 'CTRL RESET', 'ok': 'CTRL OK',
    },
    'plan': {
        'data_on': None, 'header': 'DATA HDR?', 'data': 'DATA?',
        'target': 'SP {:.1f}', 'auto': 'MODE AUTO', 'manual': 'MODE MANUAL',
        'valve': 'VALVE SET {:.1f}', 'stop': 'STOP',
        'reset': 'ALARM RESET', 'ok': 'ALARM OK',
    },
}

# The DATA columns as firmware 0.11.52 on lists them, used until the board
# has answered with its own list.
DEFAULT_COLUMNS = ['t_s', 'p_mbar', 'target_mbar', 'valve_pct', 'mode', 'alarm',
                   'ratio_mV_per_V', 'exc_mV', 'die_C', 'baro_mbar']

# A DATA line older than this means the board has stopped sending it.
DATA_STALE_S = 5.0


class Mm4IO(StringIO):
    """Line link to the mm4 text port.

    Every command line gets one final line starting OK or ERR; *IDN? gets
    one line and no OK. Anything else the board sends (its echo of the
    command, status lines, events, DATA lines) is read past: DATA lines are
    kept in 'data', the rest go to the debug log.
    """
    end_of_line = '\n'   # the board ends lines with CR LF; CR is stripped
    timeout = 3
    identification = [('*IDN?', r'PMC,MM4,')]
    command_set = Property('command words: 0.12 (firmware as built) or plan',
                        StringType(), default='0.12')

    def earlyInit(self):
        super().earlyInit()
        if self.command_set not in COMMANDS:
            raise ValueError(f'command_set must be one of {", ".join(COMMANDS)}')
        self.cmd = COMMANDS[self.command_set]
        self.columns = list(DEFAULT_COLUMNS)
        self.data = {}
        self.data_time = 0

    def connectStart(self):
        super().connectStart()
        self.data = {}
        self.data_time = 0
        if self.cmd['data_on']:
            self.query(self.cmd['data_on'])
        final, lines = self.query(self.cmd['header'])
        for line in lines:
            # 'DATA t_s,p_mbar,...' (0.12) or 'DATA t_s,...' / 'DATA HDR t_s,...'
            m = re.match(r'DATA(?: HDR)?[ ,]+([a-zA-Z_][\w,]*)$', line)
            if m and not final.startswith('ERR'):
                self.columns = m.group(1).split(',')
        if self.cmd['data_on']:
            self._wait_data(2.5)   # the first streamed line comes within a second

    def _wait_data(self, seconds):
        end = time.time() + seconds
        with self._lock:
            while not self.data_time and time.time() < end:
                try:
                    self._take(self._readline())
                except TimeoutError:
                    break

    def _take(self, line):
        """Handle a line that is not the reply looked for."""
        if line.startswith('DATA,'):
            fields = line[5:].split(',')
            self.data = dict(zip(self.columns, fields))
            self.data_time = time.time()
        elif line and not line.startswith('> '):
            self.comLog('< %s (not a reply)', line)

    def _readline(self):
        reply = self._conn.readline(self.timeout)
        return reply.decode(self.encoding).strip()

    def query(self, command):
        """Send one line; return (final OK/ERR line, lines before it)."""
        self.check_connection()
        try:
            with self._lock:
                # lines sent since the last command: keep the DATA lines
                pending = self._conn.flush_recv()
                if pending:
                    *whole, part = pending.split(self._eol_read)
                    self._conn._rxbuffer = part
                    for line in whole:
                        self._take(line.decode(self.encoding).strip())
                self._conn.send(command.encode(self.encoding) + self._eol_write)
                self.comLog('> %s', command)
                lines = []
                while True:
                    line = self._readline()
                    if command == '*IDN?' and line.startswith('PMC,'):
                        self.comLog('< %s', line)
                        return line, []
                    if line.startswith(('OK', 'ERR')):
                        self.comLog('< %s', line)
                        return line, lines
                    if line.startswith('DATA') and command.startswith('DATA'):
                        lines.append(line)
                    self._take(line)
        except Exception as e:
            self.closeConnection()
            raise CommunicationFailedError(repr(e)) from None

    def communicate(self, command, noreply=False):
        final, _ = self.query(command)
        return final

    def do(self, key, *args):
        """Run one of the COMMANDS; an ERR reply is raised to the client."""
        final, _ = self.query(self.cmd[key].format(*args))
        if final.startswith('ERR'):
            raise ImpossibleError(final[3:].strip())
        return final

    def fresh_data(self):
        """The newest DATA fields, asked for now (0.12: the streamed line)."""
        self.query(self.cmd['data'])
        if time.time() - self.data_time > DATA_STALE_S:
            if self.cmd['data_on']:
                self.query(self.cmd['data_on'])
            raise CommunicationFailedError('no DATA line from the board')
        return self.data


MODES = ('MANUAL', 'AUTO', 'HOMING', 'NOT_HOMED')


def number(text):
    """A DATA field as a float, None when empty (no reading, not homed)."""
    return float(text) if text else None


class Pressure(HasIO, Drivable):
    """Pot pressure and its control: setpoint, MANUAL or AUTO, stop."""

    ioClass = Mm4IO

    value = Parameter('pressure', FloatRange(unit='mbar', fmtstr='%.2f'))
    target = Parameter('setpoint, used in AUTO', FloatRange(0, 1000, unit='mbar', fmtstr='%.1f'),
                       readonly=False)
    mode = Parameter('MANUAL or AUTO (HOMING and NOT_HOMED are shown, not set)',
                     EnumType('mode', MANUAL=0, AUTO=1, HOMING=2, NOT_HOMED=3),
                     readonly=False)
    alarm = Parameter('latched fault (F-n) or warning (W-n), empty if none', StringType())
    tolerance = Parameter('within this of the setpoint the status is IDLE in AUTO',
                          FloatRange(0, unit='mbar'), readonly=False, default=0.5)
    room_pressure = Parameter('room pressure (Pressure 21 Click), if fitted',
                              FloatRange(unit='mbar', fmtstr='%.2f'), default=0)

    def read_value(self):
        d = self.io.fresh_data()
        p = number(d.get('p_mbar'))
        sp = number(d.get('target_mbar'))
        if sp is not None:
            self.target = sp
        mode = d.get('mode', '').replace(' ', '_')
        if mode in MODES:
            self.mode = mode
        self.alarm = d.get('alarm', '')
        baro = number(d.get('baro_mbar'))
        if baro is not None:
            self.room_pressure = baro
        self.status = self.status_of(p, sp, mode, self.alarm)
        return p if p is not None else self.value

    def status_of(self, p, sp, mode, alarm):
        if p is None:
            return ERROR, 'no pressure reading'
        if alarm.startswith('F'):
            return ERROR, f'fault {alarm} (see the screen; ALARM RESET once cleared)'
        if mode == 'HOMING':
            return BUSY, 'valve homing'
        if mode == 'NOT_HOMED':
            return WARN, 'valve not homed: home it at the screen'
        if alarm:
            return WARN, f'warning {alarm}'
        if mode == 'AUTO':
            if sp is not None and abs(p - sp) > self.tolerance:
                return BUSY, 'AUTO, going to the setpoint'
            return IDLE, 'AUTO'
        return IDLE, 'MANUAL'

    def write_target(self, value):
        self.io.do('target', value)
        return value

    @nopoll
    def read_mode(self):
        self.read_value()
        return self.mode

    def write_mode(self, value):
        if value == 'AUTO':
            self.io.do('auto')
        elif value == 'MANUAL':
            self.io.do('manual')
        else:
            raise ImpossibleError('only MANUAL or AUTO can be set')
        self.read_value()
        return self.mode

    @nopoll
    def read_alarm(self):
        self.read_value()
        return self.alarm

    @nopoll
    def read_room_pressure(self):
        self.read_value()
        return self.room_pressure

    @Command()
    def stop(self):
        """Stop the valve now; in AUTO this ends AUTO (MANUAL)."""
        self.io.do('stop')

    @Command()
    def reset_alarm(self):
        """Clear latched faults whose cause has gone."""
        self.io.do('reset')

    @Command()
    def ok_warning(self):
        """Acknowledge the warning, as OK on the screen."""
        self.io.do('ok')


class Valve(HasIO, Drivable):
    """Needle valve position, set in MANUAL only (in AUTO the board refuses)."""

    ioClass = Mm4IO

    value = Parameter('valve position', FloatRange(unit='%', fmtstr='%.2f'))
    target = Parameter('valve position to go to', FloatRange(0, 100, unit='%', fmtstr='%.1f'),
                       readonly=False)
    tolerance = Parameter('within this of the target the move is done',
                          FloatRange(0, unit='%'), readonly=False, default=0.1)
    move_timeout = Parameter('give up waiting for a move after this', FloatRange(0, unit='s'),
                             readonly=False, default=300)
    _moving_since = None

    def read_value(self):
        d = self.io.fresh_data()
        v = number(d.get('valve_pct'))
        mode = d.get('mode', '')
        if v is None:
            self.status = WARN, 'valve not homed: home it at the screen'
            return self.value
        if mode == 'HOMING':
            self.status = BUSY, 'valve homing'
        elif self._moving_since is not None:
            if abs(v - self.target) <= self.tolerance:
                self._moving_since = None
                self.status = IDLE, mode
            elif time.time() - self._moving_since > self.move_timeout:
                self._moving_since = None
                self.status = WARN, 'move not finished in time'
            else:
                self.status = BUSY, 'moving'
        else:
            self.status = IDLE, mode
        return v

    def write_target(self, value):
        self.io.do('valve', value)
        self._moving_since = time.time()
        self.status = BUSY, 'moving'
        return value

    @Command()
    def stop(self):
        """Stop the valve now; in AUTO this ends AUTO (MANUAL)."""
        self.io.do('stop')
        self._moving_since = None
