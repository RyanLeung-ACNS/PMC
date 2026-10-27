"""Frappy (SECoP) driver for the mm4 pressure controller.

The mm4 is reached over its text port (TCP 5000). Three modules share one
connection (names as in cfg/mm4_cfg.py):

    pressure       value = pressure (mbar), target = setpoint (starts AUTO),
                   control_active = AUTO, stop, reset (faults), _ok_warning
    valve          value = valve % open, target = valve % (ends AUTO),
                   controlled_by = self or pressure
    room_pressure  value = room pressure (mbar), if the Click is fitted

The SECoP side follows the standard controller pattern: the pressure module
controls the valve module (control_active, controlled_by), and writing
either target takes control, as SECoP expects.

It uses the remote command set (names picked 27 Oct 2026), which the
firmware gets in the same release as this driver: SP, MODE AUTO,
MODE MANUAL, VALVE SET, STOP, ALARM RESET, ALARM OK, DATA?, DATA HDR?.
Nothing here homes the valve, calibrates, changes a setting or drives the
motor. Those stay on the touch screen.

Readings come from DATA?, one DATA line (t_s, p_mbar, target_mbar,
valve_pct, mode, alarm, ...), with the column names from DATA HDR?.
"""

import re
import time

from frappy.core import BUSY, ERROR, IDLE, WARN, BoolType, Command, Drivable, \
    FloatRange, HasIO, Parameter, Readable, StringIO, StringType, nopoll
from frappy.mixins import HasControlledBy, HasOutputModule
from frappy.errors import CommunicationFailedError, ImpossibleError

COMMANDS = {
    'header': 'DATA HDR?', 'data': 'DATA?',
    'target': 'SP {:.1f}', 'auto': 'MODE AUTO', 'manual': 'MODE MANUAL',
    'valve': 'VALVE SET {:.1f}', 'stop': 'STOP',
    'reset': 'ALARM RESET', 'ok': 'ALARM OK',
}

# The DATA columns, used until DATA HDR? has answered.
DEFAULT_COLUMNS = ['t_s', 'p_mbar', 'target_mbar', 'valve_pct', 'mode', 'alarm',
                   'ratio_mV_per_V', 'exc_mV', 'die_C', 'baro_mbar']


class Mm4IO(StringIO):
    """Line link to the mm4 text port.

    Every command line gets one final line starting OK or ERR; *IDN? gets
    one line and no OK. Any other line before the final one is kept as the
    reply body (DATA? and DATA HDR?) or, if unexpected, logged and skipped.
    """
    end_of_line = '\n'   # the board ends lines with CR LF; CR is stripped
    timeout = 3
    identification = [('*IDN?', r'PMC,MM4,')]

    def earlyInit(self):
        super().earlyInit()
        self.columns = list(DEFAULT_COLUMNS)

    def connectStart(self):
        super().connectStart()
        final, lines = self.query(COMMANDS['header'])
        for line in lines:
            m = re.match(r'DATA HDR ([a-zA-Z_][\w,]*)$', line)
            if m and final.startswith('OK'):
                self.columns = m.group(1).split(',')

    def _readline(self):
        reply = self._conn.readline(self.timeout)
        return reply.decode(self.encoding).strip()

    def query(self, command):
        """Send one line; return (final OK/ERR line, DATA lines before it)."""
        self.check_connection()
        try:
            with self._lock:
                garbage = self._conn.flush_recv()
                if garbage:
                    self.comLog('garbage: %r', garbage)
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
                    if line.startswith('DATA'):
                        lines.append(line)
                    elif line:
                        self.comLog('< %s (not a reply)', line)
        except Exception as e:
            self.closeConnection()
            raise CommunicationFailedError(repr(e)) from None

    def communicate(self, command, noreply=False):
        final, _ = self.query(command)
        return final

    def do(self, key, *args):
        """Run one of the COMMANDS; an ERR reply is raised to the client."""
        final, _ = self.query(COMMANDS[key].format(*args))
        if final.startswith('ERR'):
            raise ImpossibleError(final[3:].strip())
        return final

    def fresh_data(self):
        """The DATA fields now, from DATA?."""
        final, lines = self.query(COMMANDS['data'])
        for line in lines:
            if line.startswith('DATA,'):
                return dict(zip(self.columns, line[5:].split(',')))
        raise CommunicationFailedError(f'no DATA line in the reply to DATA? ({final})')


def number(text):
    """A DATA field as a float, None when empty (no reading, not homed)."""
    return float(text) if text else None


class Pressure(HasOutputModule, HasIO, Drivable):
    """Pot pressure and its control (SECoP: a controller with an output module).

    Writing target sets the setpoint and starts AUTO (control_active true).
    stop ends AUTO and holds the valve. Writing the valve's target ends AUTO
    first (the valve's controlled_by goes back to self).
    """

    ioClass = Mm4IO

    value = Parameter('pressure', FloatRange(unit='mbar', fmtstr='%.2f'))
    target = Parameter('setpoint; writing it starts AUTO',
                       FloatRange(0, 1000, unit='mbar', fmtstr='%.1f'), readonly=False)
    control_active = Parameter('true in AUTO (the board regulates the pressure)', BoolType(),
                               default=False)
    alarm = Parameter('latched fault (F-n) or warning (W-n), empty if none', StringType())
    tolerance = Parameter('within this of the setpoint the status is IDLE in AUTO',
                          FloatRange(0, unit='mbar'), readonly=False, default=0.5)

    def read_value(self):
        d = self.io.fresh_data()
        p = number(d.get('p_mbar'))
        sp = number(d.get('target_mbar'))
        if sp is not None:
            self.target = sp
        mode = d.get('mode', '')
        self.alarm = d.get('alarm', '')
        self.sync_control(mode == 'AUTO')
        self.status = self.status_of(p, sp, mode, self.alarm)
        return p if p is not None else self.value

    def sync_control(self, auto):
        """Follow the board's mode, without sending anything."""
        self.control_active = auto
        out = self.output_module
        if out:
            if auto and out.controlled_by != self.name:
                out.controlled_by = self.name
            elif not auto and out.controlled_by:
                out.controlled_by = 0   # self

    def status_of(self, p, sp, mode, alarm):
        if p is None:
            return ERROR, 'no pressure reading'
        if alarm.startswith('F'):
            return ERROR, f'fault {alarm} (see the screen; reset once cleared)'
        if mode == 'HOMING':
            return BUSY, 'valve homing'
        if mode == 'NOT HOMED':
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
        self.io.do('auto')
        self.activate_control()
        return value

    def set_control_active(self, active):
        if self.control_active and not active:
            self.io.do('manual')
        self.control_active = active

    @nopoll
    def read_control_active(self):
        self.read_value()
        return self.control_active

    @nopoll
    def read_alarm(self):
        self.read_value()
        return self.alarm

    @Command()
    def stop(self):
        """Stop the valve now; in AUTO this ends AUTO (MANUAL)."""
        self.io.do('stop')
        self.sync_control(False)

    @Command()
    def reset(self):
        """Clear latched faults whose cause has gone."""
        self.io.do('reset')

    @Command()
    def ok_warning(self):
        """Acknowledge the warning, as OK on the screen."""
        self.io.do('ok')


class Valve(HasControlledBy, HasIO, Drivable):
    """Needle valve position. Writing target ends AUTO first (MANUAL)."""

    ioClass = Mm4IO

    value = Parameter('valve position', FloatRange(unit='%', fmtstr='%.2f'))
    target = Parameter('valve position to go to; writing it ends AUTO',
                       FloatRange(0, 100, unit='%', fmtstr='%.1f'), readonly=False)
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
        self.self_controlled()   # ends AUTO through the pressure module
        self.io.do('valve', value)
        self._moving_since = time.time()
        self.status = BUSY, 'moving'
        return value

    @Command()
    def stop(self):
        """Stop the valve now; in AUTO this ends AUTO (MANUAL)."""
        self.io.do('stop')
        self._moving_since = None


class RoomPressure(HasIO, Readable):
    """Room pressure from the Pressure 21 Click, if fitted."""

    ioClass = Mm4IO

    value = Parameter('room pressure', FloatRange(unit='mbar', fmtstr='%.2f'))

    def read_value(self):
        baro = number(self.io.fresh_data().get('baro_mbar'))
        if baro is None:
            self.status = ERROR, 'no reading (Pressure 21 not fitted?)'
            return self.value
        self.status = IDLE, ''
        return baro
