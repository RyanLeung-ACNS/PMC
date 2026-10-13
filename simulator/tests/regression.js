// Earlier review findings that must stay fixed (v49, v50), the v51 all-or-nothing Apply and the v52 fixed header.
module.exports = async (p, h) => {
  const { ok } = h, ev = f => p.evaluate(f);
  await ev(() => { localStorage.clear(); SAVED = defaultCfg(); loadActive(SAVED); document.getElementById('reboot').click(); setHe(true); setPump(true); });
  let r = await ev(() => { home(); let n = 0; while (!(C.mode === 'HOMING' && !M.moving && C.homeStage === 'back-off') && n++ < 400 / DT) simTick();
    operatorStop(); for (let i = 0; i < 5 / DT; i++) simTick(); return { mode: C.mode, moving: M.moving, f12: C.faults.has('F-12') }; });
  ok(r.mode === 'NOT HOMED' && !r.moving && r.f12, 'STOP in the pause between referencing stages: no further move');
  r = await ev(() => { command('LIM SIM OPEN'); for (let i = 0; i < 1 / DT; i++) simTick(); const started = requestMove(M.pos + 2000, true); M.moving = true; M.target = M.pos + 5000; M.dir = 1; M.v = 50; operatorStop(); return { lim: LIM.active, started, moving: M.moving }; });
  ok(r.lim && r.started === false && !r.moving, 'closed limit with F-12: opening refused; STOP again stops');
  await ev(() => { command('LIM SIM NORMAL'); ack(); bootHomed(); });
  r = await ev(() => { const res = importCfg({ MOT: { run: -100 } }, 'bad'); return { res, d: DRAFT.size }; });
  ok(r.res === false && r.d === 0, 'import with run speed -100 refused, nothing drafted');
  r = await ev(() => { DRAFT.clear(); DRAFT.set('MOT.start', 900); DRAFT.set('MOT.run', 500); DRAFT.set('P.Kp', 9); const res = applyAll(); return { res, kp: P.Kp, d: DRAFT.size, why: C.refusal }; });
  ok(r.res === false && r.kp === 4 && r.d === 3, 'invalid change set rejected whole, nothing applied, drafts kept: ' + r.why);
  r = await ev(() => { DRAFT.clear(); command('PID SPMAX 0.1'); return [...document.querySelectorAll('#con div')].slice(-1)[0].textContent; });
  ok(/^ERR/.test(r), 'serial PID SPMAX below Target min refused: ' + r);
  r = await ev(() => { DRAFT.clear(); bootHomed(); BENCH.plumb = 'air'; BENCH.air = 0.01; const q = JSON.parse(JSON.stringify(SENS.prof[0])); q.table = [[q.table[0][0] + 0.005, 0], q.table[1]]; SENS.prof[0] = q;
    for (let i = 0; i < 200; i++) simTick(); show('ctrl', false, true); updateUI(); const shown = document.querySelector('#dev').innerText; const f = [...C.faults.keys()]; const sp = setSP(0); BENCH.plumb = 'line'; loadActive(SAVED); return { pv: PV, shown, f, sp }; });
  ok(r.pv < 0 && /-0\.\d/.test(r.shown) && !r.f.length && r.sp !== false, 'negative reading after drift: shown as ' + (r.pv != null ? r.pv.toFixed(2) : '?') + ', no fault, target 0 allowed');
  r = await ev(() => { DRAFT.clear(); loadActive(SAVED); const q = JSON.parse(JSON.stringify(SENS.prof[0])); q.table = [[0.01, 0], [9.4, 1013.2]]; delete q.bench; q.calId = 'Reissue'; importCfg({ sens: { prof: [q] } }, 't'); return { d: DRAFT.has('S.prof'), tab: SENS.prof[0].table[0][0] }; });
  ok(r.d && r.tab === 0.004756, 'revised calibration for a known serial is a draft, not applied');
  r = await ev(() => { DRAFT.clear(); P.Kp = 5; SIM_FLASH_FAIL = true; const rev = SAVED.rev; const res = saveCfg(); SIM_FLASH_FAIL = false; return { res, same: rev === SAVED.rev, dirty: cfgDirty() }; });
  ok(r.res === false && r.same && r.dirty, 'failed flash write: not saved, reported');
  r = await ev(() => { DRAFT.clear(); show('ctrl', false, true); updateUI(); const home = G.date.textContent; show('menu', false, true); updateUI(); const tile = [...document.querySelectorAll('#dev .b')].some(b => b.offsetParent && b.textContent.trim() === 'Settings'); show('comm'); return { home, tile, cur: current }; });
  ok(/^\d\d [A-Z][a-z]{2} 2026 \d\d:\d\d$/.test(r.home) && r.tile && r.cur === 'comm', 'date on the home header (' + r.home + '); Settings opens with no PIN');
  r = await ev(() => { const o = []; const keep = conLog; conLog = x => o.push(x); command('PID?'); command('HELP'); conLog = keep; return o; });
  ok(r.every(l => l.length <= 70), 'console replies broken into lines of 70 characters or less: longest ' + Math.max(...r.map(l => l.length)));
};
