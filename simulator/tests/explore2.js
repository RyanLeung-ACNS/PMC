// More exploratory runs: node simulator/tests/explore2.js
const { load } = require('./sim-node.js');
const finds = [];
function find(name, detail) { finds.push(name); console.log('FIND ' + name + (detail ? '\n     ' + String(detail).replace(/\r/g, '').slice(0, 1500).replace(/\n/g, '\n     ') : '')); }
function note(name, detail) { console.log('     ' + name + (detail !== undefined ? ': ' + String(detail).replace(/\r?\n/g, ' | ').slice(0, 700) : '')); }
function ok(name) { console.log('ok   ' + name); }
function homed(s) {
  s.boot(); s.set(5, 40000); s.run(2000);
  const out = s.cmd('VALVE CAL', 150000);
  if (!/Homed in/.test(out)) throw new Error('homing failed:\n' + out.slice(-800));
  return s;
}
const mode = s => (s.cmd('VALVE?', 300).match(/VALVE mode ([A-Z ]+)/) || [, '?'])[1].trim();
const drift = s => s.get(30) - s.get(5) / 2;
const pos = s => s.get(31) / 1000;
function run(name, fn) {
  const s = load();
  try { fn(s); } catch (e) { find(name + ': exception', e.stack); }
  if (s.halted()) find(name + ': firmware halted', s.halted());
}
function auto5(s) { homed(s); s.cmd('CTRL SP 5', 300); s.cmd('CTRL AUTO', 300); s.run(400000); }

run('long line does not run its tail', s => {
  homed(s);
  const before = s.get(30);
  const o = s.cmd('X'.repeat(250) + 'VALVE GOTO 50', 100000);
  note('reply', o.slice(-200));
  if (Math.abs(s.get(30) - before) > 10) find('the tail of an over-long line ran as a command', o.slice(-300));
  const o2 = s.cmd('VALVE?', 300);
  if (!/VALVE mode/.test(o2)) find('next command after an over-long line not answered', o2);
  ok('long line');
});

run('commands refused in AUTO', s => {
  auto5(s);
  const c = ['MOT MOVE 1000', 'MOT GOTO 0', 'MOT ZERO', 'MOT OVERRIDE', 'MOT RATE 300', 'MOT DIR 1', 'MOT ORDER 0123',
    'VALVE GOTO 10', 'VALVE CLOSED', 'VALVE CAL', 'VALVE POSMAX 50000', 'CFG LOAD DEFAULTS', 'CFG LOAD PREVIOUS', 'CAL VAC', 'CAL ERASE', 'MOT HOLD 1'];
  const accepted = [];
  for (const l of c) { const o = s.cmd(l, 400); if (!/ERR/.test(o)) accepted.push(l + ' -> ' + o.replace(/\r?\n/g, ' ').slice(0, 160)); }
  note('mode after', mode(s));
  if (accepted.length) find('accepted in AUTO', accepted.join('\n'));
  ok('AUTO refusals');
});

run('KDIR wrong way in AUTO', s => {
  homed(s);
  s.cmd('PID KDIR -1', 300); s.cmd('CTRL SP 5', 300); s.cmd('CTRL AUTO', 300); s.run(900000);
  note('after 15 min', mode(s) + ', valve ' + pos(s).toFixed(2) + ', P ' + s.get(0).toFixed(2));
  note('events', s.cmd('EVT? 8', 400));
  ok('KDIR wrong');
});

run('switch reads pressed (broken wire) in AUTO', s => {
  auto5(s);
  s.set(7, 1); s.run(5000);
  note('after break', mode(s) + ' ' + s.cmd('CTRL?', 300));
  s.cmd('CTRL SP 1', 300); s.run(600000);
  note('target 1 with closing blocked', mode(s) + ', valve ' + pos(s).toFixed(2) + ', P ' + s.get(0).toFixed(2));
  note('events', s.cmd('EVT? 10', 400));
  s.set(7, 0); s.run(3000);
  s.cmd('CTRL SP 1', 300); s.run(400000);
  note('switch mended', mode(s) + ', valve ' + pos(s).toFixed(2) + ', P ' + s.get(0).toFixed(2));
  ok('broken switch in AUTO');
});

run('pressure over the sensor range in AUTO', s => {
  auto5(s);
  s.set(4, 0); s.set(40, 1100); s.run(20000);
  note('P 1100', mode(s) + ' ' + s.cmd('CTRL?', 300) + s.cmd('EVT? 5', 300));
  ok('over range');
});

run('excitation drifts in AUTO, then back', s => {
  auto5(s);
  s.set(10, 10.7); s.run(5000);
  note('exc 10.7', mode(s) + ' ' + s.cmd('EVT? 4', 300));
  s.set(10, 10.0); s.run(5000);
  note('reset', s.cmd('CTRL RESET', 400));
  ok('excitation');
});

run('hot controller in AUTO', s => {
  auto5(s);
  s.set(13, 60); s.run(10000);
  note('60 C', mode(s) + ' ' + s.cmd('EVT? 3', 300));
  if (mode(s) !== 'AUTO') find('W-3 hot stopped AUTO (it should only warn)');
  ok('hot');
});

run('supply low but on (9 V) during moves', s => {
  homed(s);
  s.set(12, 9.0); s.run(3000);
  const o = s.cmd('VALVE GOTO 30', 120000);
  note('9 V', o.split('\n').filter(l => /SUP|ERR|W-/.test(l)).join(' / ') + ' drift ' + drift(s));
  if (Math.abs(drift(s)) > 1) find('steps lost at 9 V supply with no warning', 'drift ' + drift(s));
  ok('supply 9 V');
});

run('rapid AUTO / MANUAL toggling', s => {
  homed(s); s.cmd('CTRL SP 5', 300);
  for (let i = 0; i < 200; i++) { s.cmd('CTRL AUTO', 200 + (Math.random() * 3000 | 0)); s.cmd('CTRL MAN', 200 + (Math.random() * 3000 | 0)); }
  s.run(60000);
  note('after 200 toggles', mode(s) + ' drift ' + drift(s) + ' ' + s.cmd('CTRL?', 300));
  if (Math.abs(drift(s)) > 1) find('toggling AUTO and MANUAL loses steps', 'drift ' + drift(s));
  ok('toggle');
});

run('CTRL MAN with a % while moving', s => {
  auto5(s);
  const o = s.cmd('CTRL MAN 80', 200000);
  note('CTRL MAN 80', mode(s) + ' valve ' + pos(s).toFixed(2));
  if (Math.abs(pos(s) - 80) > 0.05) find('CTRL MAN 80 did not end at 80 %', o.slice(-400));
  ok('CTRL MAN %');
});

run('settings record corrupted then reboot', s => {
  s.boot(); s.run(2000);
  s.cmd('PID KP 2.5', 300); s.cmd('CFG SAVE', 600); s.cmd('PID KP 3.5', 300); s.cmd('CFG SAVE', 600);
  s.cmd('CFG TEST CORRUPT', 600);
  const t = load(); t.boot(s.flash()); t.run(3000);
  note('after corrupt', t.cmd('CFG?', 400) + t.cmd('PID?', 300).split('\n')[1]);
  if (!/KP 2\.50/.test(t.cmd('PID?', 300))) find('corrupted newest: previous not loaded');
  s.cmd('CFG TEST ERASE', 600);
  const u = load(); u.boot(s.flash()); u.run(3000);
  note('after erase', u.cmd('CFG?', 400));
  ok('corrupt');
});

run('cal capture with the Druck in the wrong state', s => {
  s.boot(); s.run(2000);
  // vacuum point with the line at room air, air point with the line at vacuum
  s.set(11, 1); s.run(3000);
  const v = s.cmd('CAL VAC', 15000);
  s.set(11, 0); s.run(3000);
  const a = s.cmd('CAL AIR 1013', 15000);
  note('CAL VAC at air', v.slice(-250)); note('CAL AIR at vacuum', a.slice(-250));
  if (!/ERR/.test(v + a)) find('cal points taken the wrong way round with no check', v.slice(-200) + '\n' + a.slice(-200));
  ok('cal sanity');
});

run('MOT DIR flipped then homing', s => {
  s.boot(); s.set(5, 40000); s.run(2000);
  note('MOT DIR 1', s.cmd('MOT DIR 1', 400));
  const h = s.cmd('VALVE CAL', 250000);
  note('homing with direction flipped', h.split('\n').filter(l => /EVT|ERR|F-/.test(l)).slice(-4).join(' / ') + ', stalled ' + s.get(17));
  if (s.get(17) > 100) find('homing with the wrong direction drives the needle into the top stop', 'stalled ' + s.get(17));
  ok('dir flipped');
});

run('limit trips early (ice) during homing', s => {
  s.boot(); s.set(5, 40000); s.set(7, 3); s.run(2000);
  const h = s.cmd('VALVE CAL', 150000);
  note('homing with early trip', (h.match(/Homed in[^\r\n]*/) || ['no'])[0] + ', needle ' + s.get(5) / 2);
  s.cmd('VALVE GOTO 0', 100000);
  note('GOTO 0 with early trip', 'needle full ' + s.get(5) / 2 + ' (seat 0), pos ' + pos(s));
  ok('early trip');
});

run('TIME set back and forward during AUTO', s => {
  auto5(s);
  s.cmd('TIME SET 2026-10-24 23:59:50', 300); s.run(20000);
  s.cmd('TIME SET 2020-01-01 00:00:00', 300); s.run(20000);
  const t = s.cmd('TREND? ', 300) + s.cmd('EVT? 5', 300);
  note('after clock jumps', mode(s) + ' ' + t);
  if (mode(s) !== 'AUTO') find('clock change upset AUTO', t);
  ok('clock');
});

run('position offered after power cut at rest then moved', s => {
  homed(s); s.cmd('VALVE GOTO 25', 100000);
  const f = s.flash(), needle = s.get(5);
  const t = load(); t.boot(f); t.set(5, needle); t.run(3000);
  t.cmd('MOT MOVE 500', 5000);
  const r = t.cmd('VALVE RESTORE', 600);
  note('restore after a move', r.replace(/\r?\n/g, ' '));
  if (/OK/.test(r) && !/ERR/.test(r)) find('saved position still used after the motor moved');
  ok('restore after move');
});

console.log('\n' + finds.length + ' suspected bugs');
