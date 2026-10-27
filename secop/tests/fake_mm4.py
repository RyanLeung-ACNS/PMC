"""A stand-in mm4 text port for testing the Frappy driver without a board.

    python fake_mm4.py [port] [--plan]

Without --plan it answers like firmware 0.12.19 to 0.12.21: it echoes each
line as '> line', sends status lines when a client connects, and sends a
DATA line once a second after DATA ON. With --plan it follows the remote
command plan: no echo, nothing unasked, DATA? answers with one DATA line.

Only the remote command set is modelled. The valve is homed at start, and
the pressure follows the valve with a lag (0.2 mbar per % open).
"""

import socket
import sys
import threading
import time

COLUMNS = 't_s,p_mbar,target_mbar,valve_pct,mode,alarm,ratio_mV_per_V,exc_mV,die_C,baro_mbar'


class Board:
    def __init__(self, plan=False):
        self.plan = plan
        self.t0 = time.time()
        self.p = 1.0
        self.valve = 5.0
        self.goal = 5.0
        self.sp = 5.0
        self.mode = 'MANUAL'
        self.alarm = ''
        self.lock = threading.Lock()

    def step(self, dt):
        with self.lock:
            if self.mode == 'AUTO':
                self.goal = min(100.0, max(0.0, self.goal + 2.0 * (self.sp - self.p) * dt))
            d = self.goal - self.valve
            self.valve += max(-5 * dt, min(5 * dt, d))
            self.p += (0.2 * self.valve - self.p) * min(1.0, dt / 3.0)

    def data_line(self):
        with self.lock:
            t = time.time() - self.t0
            sp = f'{self.sp:.1f}'
            return (f'DATA,{t:.1f},{self.p:.2f},{sp},{self.valve:.2f},{self.mode},{self.alarm},'
                    f'0.012345,10012,31.2,1013.25')

    def command(self, line):
        """Reply lines for one command line."""
        plan = self.plan
        words = {
            'sp': 'SP ' if plan else 'CTRL SP ', 'auto': 'MODE AUTO' if plan else 'CTRL AUTO',
            'man': 'MODE MANUAL' if plan else 'CTRL MAN', 'goto': 'VALVE SET ' if plan else 'VALVE GOTO ',
            'stop': 'STOP' if plan else 'VALVE STOP', 'reset': 'ALARM RESET' if plan else 'CTRL RESET',
            'ok': 'ALARM OK' if plan else 'CTRL OK',
        }
        with self.lock:
            if line == '*IDN?':
                return ['PMC,MM4,0x3600273C,0.12.21']
            if line == 'DATA?' and not plan:
                return [f'DATA {COLUMNS}', 'OK']
            if line == 'DATA HDR?' and plan:
                return [f'DATA HDR {COLUMNS}', 'OK']
            if line in ('DATA ON', 'DATA OFF') and not plan:
                self.data_on = line == 'DATA ON'
                return [f'OK data {"on" if self.data_on else "off"}']
            if line.startswith(words['sp']):
                try:
                    v = float(line[len(words['sp']):])
                except ValueError:
                    return ['ERR Expected: CTRL SP <mbar>']
                if not 0 <= v <= 1034:
                    return ['ERR outside 0 to 1034 mbar']
                self.sp = round(v, 1)
                return [f'OK target {self.sp:.1f} mbar']
            if line == words['auto']:
                if self.alarm.startswith('F'):
                    return ['ERR fault latched, CTRL RESET first']
                self.mode = 'AUTO'
                return [f'OK AUTO at {self.sp:.1f} mbar']
            if line == words['man']:
                if self.mode == 'AUTO':
                    self.goal = self.valve
                self.mode = 'MANUAL'
                return ['OK MANUAL']
            if line.startswith(words['goto']):
                if self.mode == 'AUTO':
                    return ['ERR Switch to MANUAL first']
                self.goal = max(0.0, min(100.0, float(line[len(words['goto']):])))
                return [f'OK going to {self.goal:.1f} %']
            if line == words['stop']:
                self.goal = self.valve
                was_auto = self.mode == 'AUTO'
                self.mode = 'MANUAL'
                return ['OK stopped' + (', AUTO ended, MANUAL' if was_auto else '')]
            if line == words['reset']:
                self.alarm = ''
                return ['OK reset']
            if line == words['ok']:
                return ['OK no warning']
        if line == 'DATA?' and plan:
            return [self.data_line(), 'OK']
        return [f'ERR Unknown command: {line}']


def serve(port, plan):
    board = Board(plan)
    board.data_on = False

    def physics():
        last = time.time()
        while True:
            time.sleep(0.1)
            now = time.time()
            board.step(now - last)
            last = now

    threading.Thread(target=physics, daemon=True).start()
    srv = socket.create_server(('127.0.0.1', port))
    print(f'fake mm4 on port {port} ({"plan" if plan else "0.12"})', flush=True)
    while True:
        conn, _ = srv.accept()
        board.data_on = False
        threading.Thread(target=client, args=(board, conn), daemon=True).start()


def client(board, conn):
    send_lock = threading.Lock()
    alive = True

    def send(text):
        with send_lock:
            conn.sendall((text + '\r\n').encode())

    def pusher():
        while alive:
            time.sleep(1.0)
            if board.data_on and not board.plan:
                try:
                    send(board.data_line())
                except OSError:
                    return

    try:
        if not board.plan:
            # what 0.12 sends to a new client before any command
            send('mm4 0.12.21, PMC,MM4,0x3600273C')
            send('Druck: 1.00 mbar abs  r 0.01234 mV/V  exc 10012 mV  nominal cal')
            send('Valve: MANUAL, calibrated, 5.00 % (count 3302 of 66048)')
        threading.Thread(target=pusher, daemon=True).start()
        buf = b''
        while True:
            data = conn.recv(1024)
            if not data:
                break
            buf += data
            while b'\n' in buf or b'\r' in buf:
                i = min(x for x in (buf.find(b'\n'), buf.find(b'\r')) if x >= 0)
                line, buf = buf[:i].decode().strip(), buf[i + 1:]
                if not line:
                    continue
                if not board.plan and line != '*IDN?':
                    send(f'> {line}')
                for reply in board.command(line):
                    send(reply)
    except OSError:
        pass
    finally:
        alive = False
        conn.close()


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    serve(int(args[0]) if args else 5000, '--plan' in sys.argv)
