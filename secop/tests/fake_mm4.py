"""A stand-in mm4 text port for testing the Frappy driver without a board.

    python fake_mm4.py [port]

It answers as the firmware with the remote command set will: no echo,
nothing sent unasked, ';' chaining, and one reply line per command: a query
gives its words and value (PRES 4.625 mbar), a set command its words and the
value now in force (PRES SP 5.0 mbar), a refusal ERR <code> <text>.

Only the remote command set is modelled. The valve is homed at start, and
the pressure follows the valve with a lag (0.2 mbar per % open).
"""

import socket
import sys
import threading
import time

class Board:
    def __init__(self):
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

    def chain(self, line):
        """One reply line for a line of commands separated by ';'. At the
        first ERR the rest is skipped."""
        replies = []
        for part in line.split(';'):
            reply = self.command(part.strip())[0]
            replies.append(reply)
            if reply.startswith('ERR'):
                break
        return ';'.join(replies)

    def command(self, line):
        """Reply lines for one command line."""
        with self.lock:
            if line == '*IDN?':
                return ['RL,HPC1,3600273C,0.13.0']
            if line == 'PRES?':
                return [f'PRES {self.p:.3f} mbar']
            if line == 'PRES SP?':
                return [f'PRES SP {self.sp:.1f} mbar']
            if line == 'MODE?':
                return [f'MODE {self.mode}']
            if line == 'VALV?':
                return [f'VALV {self.valve:.2f} %']
            if line == 'VALV MOVING?':
                return [f'VALV MOVING {int(abs(self.goal - self.valve) > 0.005)}']
            if line == 'BARO?':
                return ['BARO 1013.25 mbar']
            if line == 'ALRM?':
                return [f'ALRM {self.alarm},1,Test alarm' if self.alarm else 'ALRM NONE']
            if line.startswith('PRES SP '):
                try:
                    v = float(line[8:])
                except ValueError:
                    return ['ERR 101 Expected: PRES SP <mbar>']
                if not 0 <= v <= 1034:
                    return ['ERR 102 Outside 0 to 1034 mbar']
                self.sp = round(v, 1)
                return [f'PRES SP {self.sp:.1f} mbar']
            if line == 'MODE AUTO':
                if self.alarm.startswith('F'):
                    return ['ERR 203 Reset the fault first']
                self.mode = 'AUTO'
                return ['MODE AUTO']
            if line == 'MODE MANUAL':
                if self.mode == 'AUTO':
                    self.goal = self.valve
                self.mode = 'MANUAL'
                return ['MODE MANUAL']
            if line.startswith('VALV SET '):
                if self.mode == 'AUTO':
                    return ['ERR 202 Switch to MANUAL first']
                self.goal = max(0.0, min(100.0, float(line[9:])))
                return [f'VALV SET {self.goal:.1f} %']
            if line == 'STOP':
                self.goal = self.valve
                self.mode = 'MANUAL'
                return ['STOP']
            if line == 'ALRM RESET':
                self.alarm = ''
                return ['ALRM RESET']
            if line == 'ALRM OK':
                return ['ALRM OK']
        return [f'ERR 100 Unknown command: {line}']


def serve(port):
    board = Board()

    def physics():
        last = time.time()
        while True:
            time.sleep(0.1)
            now = time.time()
            board.step(now - last)
            last = now

    threading.Thread(target=physics, daemon=True).start()
    srv = socket.create_server(('127.0.0.1', port))
    print(f'fake mm4 on port {port}', flush=True)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=client, args=(board, conn), daemon=True).start()


def client(board, conn):
    try:
        buf = b''
        while True:
            data = conn.recv(1024)
            if not data:
                break
            buf += data
            while b'\n' in buf or b'\r' in buf:
                i = min(x for x in (buf.find(b'\n'), buf.find(b'\r')) if x >= 0)
                line, buf = buf[:i].decode().strip(), buf[i + 1:]
                if line:
                    conn.sendall((board.chain(line) + '\r\n').encode())
    except OSError:
        pass
    finally:
        conn.close()


if __name__ == '__main__':
    serve(int(sys.argv[1]) if len(sys.argv) > 1 else 8888)
