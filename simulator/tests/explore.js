// Exploratory runs against the firmware build, looking for bugs:
//   node simulator/tests/explore.js
// Each case prints what it saw; FIND lines are suspected bugs.
const { load } = require('./sim-node.js');
const finds = [];
function find(name, detail) { finds.push(name); console.log('FIND ' + name + (detail ? '\n     ' + String(detail).slice(0, 1500) : '')); }
function note(name, detail) { console.log('     ' + name + (detail !== undefined ? ': ' + detail : '')); }
function ok(name) { console.log('ok   ' + name); }
function guard(s, name) { if (s.halted()) find(name + ': firmware halted', s.halted()); }
function homed(s) {
  s.boot(); s.set(5, 40000); s.run(2000);
  const out = s.cmd('VALVE CAL', 120000);
  if (!/Homed in/.test(out)) throw new Error('homing failed:\n' + out.slice(-800));
  return s;
}
// Firmware full-step count against the needle (half steps from the switch point).
function drift(s) { return s.get(30) - s.get(5) / 2; }
function pos(s) { return s.get(31) / 1000; }
function run(name, fn) {
  const s = load();
  try { fn(s); } catch (e) { find(name + ': exception', e.stack); }
  guard(s, name);
}

run('command fuzz', s => {
  s.boot(); s.run(2000);
  const lines = ['', ' ', 'valve?', 'VALVE GOTO', 'VALVE GOTO -1', 'VALVE GOTO 101', 'VALVE GOTO 1e9', 'VALVE GOTO abc',
    'VALVE GOTO 50.55', 'MOT MOVE 0', 'MOT MOVE 400001', 'MOT MOVE -400001', 'MOT MOVE 99999999999', 'MOT GOTO',
    'MOT GOTO 2147483647', 'MOT GOTO -2147483648', 'MOT RATE 0', 'MOT RATE 1001', 'MOT ACCEL 20001', 'MOT ORDER zzzz',
    'MOT ORDER abc', 'MOT ORDER aabb', 'MOT MODE QUARTER', 'PID KP -1', 'PID KP 1e9', 'PID TI -5', 'PID TI 1e9',
    'PID OUTMIN 150', 'PID DB -3', 'PID BAND 0', 'PID KDIR 0', 'PID KDIR 2', 'CTRL SP -1', 'CTRL SP 99999',
    'CTRL SP nan', 'CTRL SP 5.55555', 'CAL VAC 60', 'CAL AIR 100', 'CAL AIR 2000', 'CAL PT 1 2', 'CAL APPLY',
    'TIME SET 2026-13-40 25:61:61', 'TIME SET garbage', 'TREND DUMP 0', 'TREND DUMP 601', 'TREND DUMP 600',
    'EVT? 0', 'EVT? 101', 'EVT? -5', 'VALVE POSMAX 0', 'VALVE POSMAX -1', 'VALVE POSMAX 2147483647', 'VALVE FHOME 0',
    'VALVE BACKOFF -1', 'VALVE OFFSET 999999999', 'CFG LOAD NONSENSE', 'X'.repeat(300), 'VALVE GOTO 5' + '0'.repeat(250)];
  let bad = [];
  for (const l of lines) {
    const o = s.cmd(l, 400);
    if (s.halted()) { find('fuzz halt on "' + l.slice(0, 40) + '"', s.halted()); return; }
    if (l.trim() && !/OK|ERR|\?/.test(o)) bad.push(l.slice(0, 40) + ' -> ' + JSON.stringify(o.slice(0, 120)));
  }
  if (bad.length) find('fuzz: commands with no OK/ERR reply', bad.join('\n     '));
  const v = s.cmd('VALVE?', 400) + s.cmd('PID?', 400) + s.cmd('MOT?', 400);
  note('after fuzz', v.replace(/\r?\n/g, ' | ').slice(0, 900));
  ok('command fuzz');
});

run('accepted out-of-range values', s => {
  s.boot(); s.run(2000);
  const probe = [['VALVE POSMAX 1', 'VALVE?'], ['VALVE FHOME 1', 'VALVE?'], ['MOT RATE 16', 'MOT?'], ['MOT START 1000', 'MOT?'],
    ['PID KP 0.0001', 'PID?'], ['PID OUTMIN 100', 'PID?'], ['PID DB 100000', 'PID?'], ['PID BAND 1000', 'PID?']];
  for (const [c, q] of probe) note(c, s.cmd(c, 300).replace(/\r?\n/g, ' ').trim());
  ok('accepted out-of-range values (see notes)');
});

run('STOP in each homing stage, then home again', s0 => {
  let s;
  for (const stage of [0, 1, 2, 3]) {
    s = load(); s.boot(); s.set(5, stage ? 40000 : 0); s.run(2000);
    s.cmd('VALVE CAL', 100);
    let seen = false;
    const want = stage ? new RegExp('HOMING, ' + stage + ' of 3') : /HOMING/;
    for (let t = 0; t < 120000 && !seen; t += 300) { if (want.test(s.cmd('VALVE?', 300))) seen = true; }
    if (!seen) { find('stage ' + stage + ' never seen'); continue; }
    s.run(500);
    s.cmd(stage % 2 ? 'CTRL STOP' : 'MOT OFF', 500);
    const again = s.cmd('VALVE CAL', 150000);
    if (!/Homed in/.test(again)) find('rehome after stop in stage ' + stage, again.slice(-600));
    else if (Math.abs(drift(s)) > 2) find('rehome after stop in stage ' + stage + ': count off the needle by ' + drift(s));
  }
  ok('STOP in each homing stage');
});

run('many GOTOs and retargets keep the count on the needle', s => {
  homed(s);
  let worst = 0;
  for (let i = 0; i < 300; i++) {
    const p = Math.round(Math.random() * 1000) / 10;
    s.cmd('VALVE GOTO ' + p, Math.random() < 0.5 ? 50 + Math.random() * 3000 : 30000);
    worst = Math.max(worst, Math.abs(drift(s)));
  }
  s.cmd('VALVE GOTO 0', 60000);
  const d = drift(s);
  if (Math.abs(d) > 1 || worst > 1) find('count drifts from the needle over 300 GOTOs', 'end ' + d + ', worst ' + worst + ', stalled ' + s.get(17));
  else ok('300 GOTOs, count on the needle (stalled ' + s.get(17) + ')');
});

run('step mode change in MANUAL keeps the position', s => {
  homed(s);
  s.cmd('VALVE GOTO 40', 200000);
  const before = pos(s);
  for (const m of ['HALF', 'WAVE', 'FULL', 'HALF', 'FULL']) {
    const o = s.cmd('MOT MODE ' + m, 500);
    if (/ERR/.test(o)) note('MOT MODE ' + m, o.trim());
  }
  s.cmd('VALVE GOTO 10', 200000); s.cmd('VALVE GOTO 40', 200000);
  if (Math.abs(pos(s) - before) > 0.02 || Math.abs(drift(s)) > 1) find('mode changes lose position', 'before ' + before + ' after ' + pos(s) + ' drift ' + drift(s));
  s.cmd('MOT MODE HALF', 500); s.cmd('VALVE GOTO 20', 200000);
  note('HALF GOTO 20', pos(s) + ' %, needle full ' + s.get(5) / 2);
  const h = s.cmd('VALVE CAL', 150000);
  if (!/Homed in/.test(h)) find('homing in HALF mode', h.slice(-500));
  ok('step mode changes');
});

run('power cut during a move: saved position offered', s => {
  homed(s);
  s.cmd('VALVE GOTO 60', 3000);   // mid-move
  const f = s.flash(); const needle = s.get(5);
  const t = load(); t.boot(f); t.set(5, needle); t.run(3000);
  const sv = t.cmd('VALVE SAVED?', 400);
  note('VALVE SAVED? after a cut mid-move', sv.replace(/\r?\n/g, ' ').trim());
  const r = t.cmd('VALVE RESTORE', 1000);
  const d = drift(t);
  note('restore', r.replace(/\r?\n/g, ' ').trim() + ' drift ' + d);
  if (/OK/.test(r) && Math.abs(d) > 1100) find('restored position more than the stated 2 s of travel off', 'drift ' + d);
  guard(t, 'power cut restore');
  ok('power cut during a move');
});

run('watchdog reset during AUTO', s => {
  homed(s);
  s.cmd('CTRL SP 5', 300); s.cmd('CTRL AUTO', 300); s.run(300000);
  const f = s.flash(); const needle = s.get(5);
  const t = load(); t.x.sim_bench_init();
  const fl = new Uint8Array(t.x.memory.buffer, t.x.sim_flash(), t.x.sim_flash_size()); fl.set(f);
  t.x.sim_boot(1, Date.now() / 1000); t.set(5, needle); t.run(3000);
  const v = t.cmd('VALVE?', 400);
  note('after watchdog reset', v.replace(/\r?\n/g, ' | ').slice(0, 300));
  if (/AUTO/.test(v.split('\n')[1] || '') && !/NOT HOMED/.test(v)) find('watchdog reset resumes AUTO', v);
  if (s.get(5) !== needle) find('needle moved');
  guard(t, 'watchdog');
  ok('watchdog reset');
});

run('AUTO setpoint changes and edges', s => {
  homed(s);
  s.cmd('CTRL SP 5', 300); s.cmd('CTRL AUTO', 300); s.run(400000);
  for (const sp of [0, 0.1, 1034, 20, 2, 50, 5]) {
    const o = s.cmd('CTRL SP ' + sp, 300);
    s.run(sp === 1034 ? 60000 : 300000);
    const v = s.cmd('VALVE?', 300);
    note('SP ' + sp, (o.trim().split('\n').pop() || '') + ' | P ' + s.get(0).toFixed(2) + ' valve ' + pos(s).toFixed(2) + ' ' + (v.match(/VALVE mode [A-Z ]+/) || [''])[0]);
  }
  const ev = s.cmd('EVT? 30', 500);
  if (/F-6/.test(ev)) find('F-6 during setpoint changes', ev);
  ok('setpoint edges');
});

run('Druck unplugged in AUTO, then back', s => {
  homed(s);
  s.cmd('CTRL SP 5', 300); s.cmd('CTRL AUTO', 300); s.run(300000);
  const p0 = pos(s);
  s.set(8, 0); s.run(5000);
  const v = s.cmd('VALVE?', 300); const p1 = pos(s);
  note('F-1 in AUTO', (v.match(/VALVE mode [A-Z ]+/) || [''])[0] + ', valve ' + p0.toFixed(2) + ' -> ' + p1.toFixed(2));
  if (/AUTO/.test((v.match(/VALVE mode [A-Z ]+/) || [''])[0])) find('AUTO continues with no pressure signal', v);
  s.set(8, 1); s.run(3000);
  const r = s.cmd('CTRL RESET', 500);
  note('reset', r.replace(/\r?\n/g, ' ').trim());
  const a = s.cmd('CTRL AUTO', 500);
  if (/ERR/.test(a)) find('cannot restart AUTO after F-1 clears', a + s.cmd('CTRL?', 300));
  ok('Druck in AUTO');
});

run('supply lost during homing and during AUTO', s => {
  s.boot(); s.set(5, 40000); s.run(2000);
  s.cmd('VALVE CAL', 5000); s.set(12, 0); s.run(5000); s.set(12, 12); s.run(3000);
  const v = s.cmd('VALVE?', 300);
  note('after supply loss in homing', (v.match(/VALVE mode [A-Z ]+/) || [''])[0]);
  if (/MANUAL/.test(v) && !/NOT HOMED/.test(v)) find('homing finishes or keeps homed after the supply dropped', v);
  const h = s.cmd('VALVE CAL', 150000);
  if (!/Homed in/.test(h) || Math.abs(drift(s)) > 1) find('rehome after supply loss', h.slice(-400) + ' drift ' + drift(s));
  s.cmd('CTRL SP 5', 300); s.cmd('CTRL AUTO', 300); s.run(200000);
  s.set(12, 0); s.run(5000); s.set(12, 12); s.run(5000);
  const w = s.cmd('VALVE?', 300);
  note('after supply loss in AUTO', (w.match(/VALVE mode [A-Z ]+/) || [''])[0]);
  ok('supply loss');
});

run('limit switch breaks in AUTO near the seat', s => {
  homed(s);
  s.cmd('PID OUTMIN 0', 300); s.cmd('CTRL SP 0', 300); s.cmd('CTRL AUTO', 300); s.run(400000);
  s.set(7, 2 /* stuck: never trips */); s.cmd('CTRL SP 0', 300); s.run(300000);
  note('stuck switch, target 0', 'needle ' + s.get(5) + ' stalled ' + s.get(17) + ' valve ' + pos(s));
  if (s.get(17) > 0) find('valve driven into the seat (end stop) with the switch stuck', 'stalled ' + s.get(17));
  ok('limit in AUTO');
});

run('settings saved 400 times', s => {
  s.boot(); s.run(2000);
  for (let i = 0; i < 400; i++) {
    s.cmd('PID KP ' + (1 + (i % 50) / 10).toFixed(1), 50);
    const o = s.cmd('CFG SAVE', 400);
    if (!/OK/.test(o)) { find('CFG SAVE failed at save ' + i, o); return; }
  }
  const kp = (1 + (399 % 50) / 10).toFixed(1);
  const t = load(); t.boot(s.flash()); t.run(3000);
  const p = t.cmd('PID?', 400);
  if (!new RegExp('KP ' + kp).test(p)) find('after 400 saves the newest is not loaded', 'want KP ' + kp + '\n' + p);
  else ok('400 saves, newest loads');
});

run('position log over many moves and a power cycle', s => {
  homed(s);
  for (let i = 0; i < 5000; i++) s.cmd('VALVE GOTO ' + (i % 2 ? 30 : 31), 1200);
  const end = pos(s); const needle = s.get(5);
  const t = load(); t.boot(s.flash()); t.set(5, needle); t.run(3000);
  const sv = t.cmd('VALVE SAVED?', 400);
  note('after 5000 moves', 'at ' + end + ' %, saved: ' + sv.replace(/\r?\n/g, ' ').trim());
  const cnt = +(sv.match(/count (-?\d+)/) || [0, NaN])[1];
  if (!(Math.abs(cnt - s.get(30)) <= 1100)) find('saved position after 5000 moves wrong', 'firmware count ' + s.get(30) + '\n' + sv);
  ok('position log');
});

run('long AUTO run: heap, trend', s => {
  homed(s);
  s.cmd('CTRL SP 5', 300); s.cmd('CTRL AUTO', 300);
  const p0 = s.cmd('PERF?', 300);
  s.run(4 * 3600 * 1000);
  const p1 = s.cmd('PERF?', 300);
  note('PERF before', p0.replace(/\r?\n/g, ' ').trim()); note('PERF after 4 h', p1.replace(/\r?\n/g, ' ').trim());
  const td = s.cmd('TREND DUMP 600', 2000);
  note('TREND DUMP lines', td.split('\n').length);
  const v = s.cmd('VALVE?', 300);
  if (!/AUTO/.test(v)) find('AUTO did not hold for 4 h', v + s.cmd('EVT? 20', 400));
  ok('long run');
});

run('screen monkey: 3000 random taps', s => {
  homed(s);
  let last = '';
  for (let i = 0; i < 3000; i++) {
    const x = (Math.random() * 480) | 0, y = (Math.random() * 272) | 0;
    s.x.sim_touch(x, y, 1); s.run(60 + (Math.random() * 400 | 0)); s.x.sim_touch(x, y, 0); s.run(200);
    if (s.halted()) { find('monkey halt after ' + i + ' taps at ' + x + ',' + y, s.halted()); return; }
    if (i % 500 === 0) last = s.cmd('PERF?', 200);
  }
  note('PERF after monkey', s.cmd('PERF?', 200).replace(/\r?\n/g, ' ').trim());
  const ev = s.cmd('EVT? 100', 600);
  const refs = (ev.match(/REF [^\r\n]*/g) || []);
  note('drift', drift(s) + ' stalled ' + s.get(17) + ', REF events ' + refs.length);
  if (Math.abs(drift(s)) > 1 && !/set|restored|ZERO|zero/i.test(ev)) find('monkey: count off the needle with no position set', ev);
  if (s.get(17) > 0) find('monkey drove the needle into an end stop', 'stalled ' + s.get(17) + '\n' + s.cmd('EVT? 30', 300));
  ok('monkey');
});

console.log('\n' + finds.length + ' suspected bugs');
