const { load } = require('./sim-node.js');
const mode = s => (s.cmd('VALVE?', 300).match(/VALVE mode ([A-Z ]+)/) || [, '?'])[1].trim();
function auto5() { const s = load(); s.boot(); s.set(5, 40000); s.run(2000); s.cmd('VALVE CAL', 150000); s.cmd('CTRL SP 5', 300); s.cmd('CTRL AUTO', 300); s.run(300000); return s; }
for (const c of ['MOT RATE 300', 'MOT START 100', 'MOT ACCEL 1000', 'MOT HOLD 1', 'MOT DIR 1', 'MOT ORDER 0123', 'MOT MODE HALF', 'MOT OFF', 'MOT STOP',
  'VALVE POSMAX 50000', 'VALVE OFFSET 100', 'VALVE CLEAR 500', 'CAL VAC', 'CAL AIR 1013', 'CAL ERASE', 'CAL NEW', 'CFG LOAD DEFAULTS', 'CFG LOAD LAST', 'CFG TEST ERASE', 'PID TI 0', 'PID OUTMIN 30']) {
  const s = auto5(); const p0 = s.get(0);
  const o = s.cmd(c, 1500); s.run(20000);
  console.log((c + '                    ').slice(0, 20), '|', mode(s).padEnd(10), '|', o.replace(/\r?\n/g, ' ').slice(c.length + 2, 170), '| P', p0.toFixed(2), '->', s.get(0).toFixed(2));
}
// broken switch: question?
