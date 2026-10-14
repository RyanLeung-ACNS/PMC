// The seven review workflows, driven by taps on the 480 x 272 screen. Screenshots: tests/shots/wfN-*.png
module.exports = async (p, h) => {
  const { tap, keys, run, st, shot, ok } = h;
  const ev = f => p.evaluate(f);
  await ev(() => { localStorage.clear(); SAVED = defaultCfg(); loadActive(SAVED); setHe(true); setPump(true); });

  // 1. Unreferenced startup -> reference -> set target -> AUTO
  await ev(() => document.getElementById('reboot').click());
  let o = await st(); ok(o.badge === 'NOT ZEROED' && o.mode === 'NOT HOMED', 'WF1 real start boots NOT ZEROED'); await shot('wf1-1-boot');
  await tap('Zero valve'); await shot('wf1-2-ask'); await tap('Start zeroing'); await run(3); await shot('wf1-3-referencing');
  await run(400); o = await st(); ok(o.mode === 'MANUAL' && o.pos === 0, 'WF1 referenced, MANUAL at 0 %');
  await tap('Valve'); await tap('Go to'); await keys('34'); await tap('Set'); await run(120); await tap('Back');
  ok(await ev(() => !C.spSet), 'WF1 no default target');
  await tap('Start AUTO'); await keys('5'); await shot('wf1-4-target'); await tap('Set');
  await run(5); o = await st(); ok(o.mode === 'AUTO', 'WF1 first Start AUTO asks for the target and starts AUTO');
  await shot('wf1-5-regulating'); const s1 = await ev(() => { C.msg = null; return statusLine().text; });
  await run(600); const s2 = await ev(() => { C.msg = null; return statusLine().text; }); await shot('wf1-6-stable');
  ok(/Regulating|Within|Stable/.test(s1) && /Stable|Within/.test(s2), 'WF1 status "' + s1 + '" then "' + s2 + '"');

  // 2. AUTO -> Valve page -> MANUAL -> enter position -> navigate away while moving
  await tap('Valve'); await shot('wf2-1-valve-auto'); await tap('Switch to MANUAL'); await shot('wf2-2-confirm'); await tap('Switch to MANUAL');
  o = await st(); ok(o.mode === 'MANUAL' && o.cur === 'valve', 'WF2 switched to MANUAL from the Valve page');
  await tap('Go to'); await keys('60'); await tap('Set'); await run(2); await shot('wf2-3-moving');
  const a = await st(); await tap('Back'); await run(1); const b = await st();
  ok(a.moving && b.moving && a.target === b.target && b.cur === 'ctrl', 'WF2 leaving the Valve page keeps the move');
  await shot('wf2-4-home-moving'); await run(400); o = await st(); ok(Math.abs(o.pos - 60) < 0.2, 'WF2 move finished at ' + o.pos + ' %');

  // 3. Paused AUTO -> resume, or switch to MANUAL
  await tap('Start AUTO'); await tap('Start AUTO'); await run(30);
  await ev(() => { C.mode = 'PAUSED'; ev('W-02', 'No convergence (test)'); }); await shot('wf3-1-paused');
  ok((await ev(() => [...document.querySelectorAll('#dev .b')].filter(b => b.offsetParent).map(b => b.textContent).join('|'))).includes('Switch to MANUAL'), 'WF3 paused home shows Resume AUTO and Switch to MANUAL');
  await tap('Resume AUTO'); await shot('wf3-2-resume-ask'); await tap('Resume AUTO'); o = await st(); ok(o.mode === 'AUTO', 'WF3 resumed AUTO');
  await ev(() => { C.mode = 'PAUSED'; updateUI(); }); await tap('Switch to MANUAL'); await tap('Switch to MANUAL'); o = await st(); ok(o.mode === 'MANUAL', 'WF3 paused -> MANUAL from the status line');

  // 4. Edit several commissioning pages -> review -> apply -> save -> restart
  await run(5); await tap('Menu'); await tap('Settings'); await shot('wf4-1-comm');
  await tap('Control tuning', true); await tap('Gain, %/mB', true); await keys('6'); await tap('Set'); await tap('Back');
  o = await st(); ok(o.cur === 'comm' && !o.ov, 'WF4 Back with a draft: no Apply/Discard question');
  await tap('Control tuning', true); await tap('Target band, mB', true); await keys('0.2'); await tap('Set'); await tap('Back');
  await tap('Valve setup', true); await tap('Drive'); await tap('Run speed, steps/s', true); await keys('500'); await tap('Set'); await shot('wf4-2-drive-draft'); await tap('Back');
  await tap('Review and save', true); await shot('wf4-3-config');
  await tap('Unsaved changes', true); const list = (await st()).ov; await shot('wf4-4-list');
  ok(/Gain/.test(list) && /Target band/.test(list) && /Run speed/.test(list), 'WF4 review lists all three by label and page'); await tap('Close');
  await tap('Save settings'); await shot('wf4-6-save-ask'); await tap('Save settings');
  o = await st(); ok(o.rev === 2 && !o.dirty && !o.drafts.length, 'WF4 one step: Save settings puts all edits in use and saves rev 2');
  await ev(() => document.getElementById('reboot').click());
  o = await ev(() => [P.Kp, P.band, MOT.run, SAVED.rev]); ok(o.join() === '6,0.2,500,2', 'WF4 kept after restart: ' + o.join(', '));

  // 5. Import revised calibration -> inspect identity/status -> apply and save
  await ev(() => { bootHomed(); show('sensor', false, true); });
  await ev(() => { const q = JSON.parse(JSON.stringify(SENS.prof[0])); q.atm = 1.06; q.calId = 'Druck reissue CA-5880156'; q.calDate = '09 Oct 2026'; importCfg({ sens: { prof: [q] } }, 'PC tool'); });
  await shot('wf5-1-sensor-imported'); await tap('Calibration', true); o = await st(); await shot('wf5-2-detail');
  ok(/Druck reissue/.test(o.ov) && /Not saved/.test(o.ov), 'WF5 detail shows new identity, state Not saved'); await tap('Close');
  await tap('Save settings'); await tap('Save settings');
  await ev(() => show('info', false, true)); await shot('wf5-4-info-saved'); o = await ev(() => calState(SENS.prof[SENS.active])); ok(o === 'Saved', 'WF5 calibration saved');

  // 6. STOP and fault recovery, including during referencing
  await ev(() => show('ctrl', false, true)); await tap('Menu'); await tap('Zero valve'); await tap('Start zeroing'); await tap('Start zeroing'); await run(3);
  await shot('wf6-1-referencing'); await tap('STOP'); await run(5); o = await st();
  ok(o.mode === 'NOT HOMED' && !o.moving && o.badge === 'STOPPED', 'WF6 STOP while referencing: stopped, not referenced'); await shot('wf6-2-stopped');
  await ev(() => { home && 0; }); await ev(() => show('ctrl', false, true)); await tap('Reset fault'); o = await st(); ok(!o.faults.length && o.mode === 'NOT HOMED', 'WF6 reset; still NOT REFERENCED');
  await ev(() => { bootHomed(); document.getElementById('b-druck').click(); }); await run(2); o = await st(); ok(o.faults.includes('F-01'), 'WF6 F-01 on Druck unplugged'); await shot('wf6-3-f01');
  await tap('Reset fault'); o = await st(); ok(o.faults.includes('F-01'), 'WF6 reset refused while unplugged');
  await ev(() => document.getElementById('b-druck').click()); await run(2); await tap('Reset fault'); o = await st(); ok(!o.faults.length && o.mode === 'MANUAL', 'WF6 reset after replug, MANUAL');

  // 7. Failed save, and commissioning timeout with pending edits
  await ev(() => { show('tune', false, true); P.Kp = 7; document.getElementById('b-flash').click(); updateUI(); });
  const rev0 = (await st()).rev;
  await tap('Save settings'); await tap('Save settings'); o = await st(); await shot('wf7-1-save-failed');
  const why = await ev(() => C.refusal);
  ok(o.rev === rev0 && o.dirty && /Save failed/.test(o.foot + why), 'WF7 failed save reported, rev unchanged, still not saved: ' + JSON.stringify([o.rev, o.dirty, o.foot, why]));
  await ev(() => document.getElementById('b-flash').click());
  await tap('Integral time, min', true); await keys('0.4'); await tap('Set');
  await tap('Save settings'); await tap('Save settings'); const kp = await ev(() => P.Kp);
  await ev(() => { P.Kp = 3; updateUI(); }); await tap('Save settings'); await tap('Save settings'); o = await st();
  await ev(() => show('cfg', false, true)); await tap('Load settings', true); await shot('wf7-2-load-list'); await tap('Previous save', true); await shot('wf7-2-revert'); await tap('Save settings'); await tap('Save settings'); o = await st();
  const kpNow = await ev(() => P.Kp); ok(kpNow === kp && !o.dirty, 'WF7 Load settings > Previous save, then Save settings, brings back gain ' + kp);
  // 8. Live calibration: capture vacuum and air through the board, check New cal, apply and save
  await ev(() => { DRAFT.clear(); toManual(); BENCH.plumb = 'air'; BENCH.air = 0.01; show('comm'); updateUI(); });
  await tap('Pressure sensor', true); await tap('Live calibration', true); await shot('wf8-1-cal');
  await tap('Capture vacuum'); await shot('wf8-2-vac-ask'); await tap('Capture'); await run(1); await shot('wf8-2-capturing');
  await run(30); o = await st(); ok(o.drafts.includes('S.prof'), 'WF8 vacuum point held as an unapplied change');
  await ev(() => { BENCH.air = 1013.4; }); await run(2);
  await tap('Capture air'); await shot('wf8-3-air-keypad'); await keys('1013.4'); await tap('Set'); await run(30); await shot('wf8-3-both');
  let c = await ev(() => { const q = fv(PROF_FIELD)[sIdx()]; return { kind: calKind(q), nw: calP(C.mv, q), old: PV }; });
  ok(c.kind === 'two-point bench' && Math.abs(c.nw - 1013.4) < 0.3, 'WF8 two-point table reads ' + c.nw.toFixed(2) + ' mB at 1013.4 (in use ' + c.old.toFixed(1) + ')');
  await tap('Save settings'); await tap('Save settings'); o = await st(); ok(!o.dirty && !o.drafts.length, 'WF8 Save settings puts the new calibration in use and saves it');
  await ev(() => { BENCH.plumb = 'line'; });

  // 9. Add a sensor on the board, then remove it
  await ev(() => show('sensor', false, true)); await tap('Add sensor'); await keys('12345678'); await tap('Set'); await shot('wf9-1-range'); await tap('30 psi');
  o = await ev(() => SENS.prof.map(q => sensName(q))); ok(o.includes('30 psi, s/n 12345678'), 'WF9 added: ' + o.join(' | '));
  await tap('Remove sensor'); await tap('30 psi, s/n 12345678'); await shot('wf9-2-remove-ask'); await tap('Remove');
  o = await ev(() => SENS.prof.map(q => q.serial)); ok(!o.includes(12345678), 'WF9 removed');
};
