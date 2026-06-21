// ft8.test.js - exercises the FT8-over-LoRa protocol brain.
// Run: node ft8.test.js
const F = require('./ft8.js');

let pass = 0, fail = 0;
function check(name, cond, extra) {
  if (cond) { pass++; console.log('  PASS', name); }
  else { fail++; console.log('  FAIL', name, extra !== undefined ? JSON.stringify(extra) : ''); }
}

console.log('=== ft8 protocol tests ===');

// ---- field predicates ------------------------------------------------------
check('isGrid IO82', F.isGrid('IO82'));
check('isGrid IO82wm (6-char)', F.isGrid('IO82wm'));
check('isGrid rejects ZZ99', !F.isGrid('ZZ99'));     // Z out of A-R
check('isGrid rejects call', !F.isGrid('G5LSI'));
check('isReport -12', F.isReport('-12'));
check('isReport +05', F.isReport('+05'));
check('isRReport R-08', F.isRReport('R-08'));
check('isCall G5LSI', F.isCall('G5LSI'));
check('isCall rejects grid', !F.isCall('IO82'));

// ---- RSSI -> report --------------------------------------------------------
check('rssi floor clamps to -23', F.rssiToReport(-130) === -23, F.rssiToReport(-130));
check('rssi strong clamps to +23', F.rssiToReport(-10) === 23, F.rssiToReport(-10));
check('rssi mid ~ 0', Math.abs(F.rssiToReport(-74)) <= 1, F.rssiToReport(-74));
check('rssi monotonic', F.rssiToReport(-100) < F.rssiToReport(-60));
check('fmtReport -3 -> -03', F.fmtReport(-3) === '-03', F.fmtReport(-3));
check('fmtReport 5 -> +05', F.fmtReport(5) === '+05', F.fmtReport(5));
check('fmtReport 0 -> +00', F.fmtReport(0) === '+00', F.fmtReport(0));

// ---- parser ---------------------------------------------------------------
check('parse CQ', (() => { const m = F.parseMsg('CQ G5LSI IO82'); return m.kind === 'CQ' && m.call === 'G5LSI' && m.grid === 'IO82'; })());
check('parse CQ DX', (() => { const m = F.parseMsg('CQ DX G5LSI IO82'); return m.kind === 'CQ' && m.call === 'G5LSI' && m.grid === 'IO82'; })());
check('parse CQ no grid', (() => { const m = F.parseMsg('CQ G5LSI'); return m.kind === 'CQ' && m.call === 'G5LSI' && !m.grid; })());
check('parse grid', (() => { const m = F.parseMsg('G5LSI M7LSI IO93'); return m.kind === 'grid' && m.to === 'G5LSI' && m.from === 'M7LSI' && m.grid === 'IO93'; })());
check('parse report', (() => { const m = F.parseMsg('M7LSI G5LSI -12'); return m.kind === 'report' && m.report === -12; })());
check('parse Rreport', (() => { const m = F.parseMsg('G5LSI M7LSI R-08'); return m.kind === 'Rreport' && m.report === -8; })());
check('parse RR73', (() => { const m = F.parseMsg('M7LSI G5LSI RR73'); return m.kind === 'RR73'; })());
check('parse 73', (() => { const m = F.parseMsg('G5LSI M7LSI 73'); return m.kind === '73'; })());
check('addressedTo directed', F.addressedTo('G5LSI M7LSI IO93') === 'G5LSI');
check('addressedTo CQ is null', F.addressedTo('CQ G5LSI IO82') === null);

// ---- Scenario A: I (M7LSI) call CQ, G5LSI answers -------------------------
// Full sequence and a log record at the end.
(function scenarioA() {
  const me = 'M7LSI', myGrid = 'IO82';
  // 1. I call CQ.
  const cq = F.callCQ(me, myGrid);
  check('A1 cq text', cq.tx === 'CQ M7LSI IO82', cq.tx);
  let qso = cq.qso;

  // 2. G5LSI answers with grid (I heard them at a strong-ish -64 dBm -> ~+1).
  let r = F.onRx(qso, 'M7LSI G5LSI IO93', me, myGrid, F.rssiToReport(-64));
  check('A2 reply is a report to G5LSI', r.fill === 'G5LSI M7LSI ' + F.fmtReport(F.rssiToReport(-64)), r.fill);
  check('A2 partner adopted', qso.partner === 'G5LSI');
  // I transmit it.
  F.onTx(qso, r.fill, me);

  // 3. They roger my report and sends their report of me.
  r = F.onRx(qso, 'M7LSI G5LSI R-09', me, myGrid, F.rssiToReport(-70));
  check('A3 reply is RR73', r.fill === 'G5LSI M7LSI RR73', r.fill);
  check('A3 complete', r.complete === true);
  check('A3 record call', r.record && r.record.call === 'G5LSI', r.record);
  check('A3 record rcvd report', r.record && r.record.rst_rcvd === '-09', r.record);
  check('A3 record sent report set', r.record && /^[-+]\d\d$/.test(r.record.rst_sent), r.record);
  F.onTx(qso, r.fill, me);

  // 4. They send a final 73 - already logged, must NOT double-log.
  r = F.onRx(qso, 'M7LSI G5LSI 73', me, myGrid, F.rssiToReport(-70));
  check('A4 no double log', r.complete === false && r.record === null, r);
})();

// ---- Scenario B: G5LSI calls CQ, I (M7LSI) answer -------------------------
(function scenarioB() {
  const me = 'M7LSI', myGrid = 'IO82';
  // 1. I click their CQ -> compose my call.
  const a = F.answerCQ('CQ G5LSI IO93', me, myGrid);
  check('B1 answer text', a.tx === 'G5LSI M7LSI IO82', a.tx);
  let qso = a.qso;
  check('B1 partner + grid', qso.partner === 'G5LSI' && qso.theirGrid === 'IO93');
  F.onTx(qso, a.tx, me);

  // 2. They send me a report -> I roger + report them.
  let r = F.onRx(qso, 'M7LSI G5LSI -05', me, myGrid, F.rssiToReport(-58));
  check('B2 reply is R<report>', r.fill === 'G5LSI M7LSI R' + F.fmtReport(F.rssiToReport(-58)), r.fill);
  check('B2 their report stored', qso.theirReportToMe === -5);
  F.onTx(qso, r.fill, me);

  // 3. They send RR73 -> I answer 73 and the QSO logs.
  r = F.onRx(qso, 'M7LSI G5LSI RR73', me, myGrid, F.rssiToReport(-58));
  check('B3 reply is 73', r.fill === 'G5LSI M7LSI 73', r.fill);
  check('B3 complete + record', r.complete === true && !!r.record, r);
  check('B3 record sent is R-stripped report', r.record && /^[-+]\d\d$/.test(r.record.rst_sent), r.record);
  check('B3 record rcvd -05', r.record && r.record.rst_rcvd === '-05', r.record);
  check('B3 record grid IO93', r.record && r.record.gridsquare === 'IO93', r.record);
})();

// ---- Negative: traffic not addressed to me is ignored ---------------------
(function scenarioC() {
  const me = 'M7LSI';
  let qso = F.callCQ(me, 'IO82').qso;
  const r = F.onRx(qso, 'G5LSI 2E0ABC IO91', me, 'IO82', 5);   // others working each other
  check('C ignores third-party traffic', r.fill === null && !r.complete, r);
})();

// ---- record date/time formatting ------------------------------------------
(function scenarioD() {
  const qso = F.newQso(); qso.partner = 'G5LSI'; qso.theirGrid = 'IO93';
  qso.myReportToThem = -3; qso.theirReportToMe = -7;
  const rec = F.makeRecord(qso, 'M7LSI', 'IO82', { date: '2026-06-20T14:05:09Z', freq: 433.074 });
  check('D date YYYYMMDD', rec.qso_date === '20260620', rec.qso_date);
  check('D time HHMMSS', rec.time_on === '140509', rec.time_on);
  check('D freq carried', rec.freq === 433.074);
})();

// ---- suggestReply (stateless) matches onRx fills ---------------------------
(function scenarioE() {
  const me = 'M7LSI', g = 'IO82';
  check('E click CQ -> call them', F.suggestReply('CQ G5LSI IO93', me, g, 0) === 'G5LSI M7LSI IO82');
  check('E grid -> report', F.suggestReply('M7LSI G5LSI IO93', me, g, F.rssiToReport(-64)) === 'G5LSI M7LSI ' + F.fmtReport(F.rssiToReport(-64)));
  check('E report -> R report', F.suggestReply('M7LSI G5LSI -05', me, g, -3) === 'G5LSI M7LSI R-03');
  check('E Rreport -> RR73', F.suggestReply('M7LSI G5LSI R-09', me, g, 0) === 'G5LSI M7LSI RR73');
  check('E RR73 -> 73', F.suggestReply('M7LSI G5LSI RR73', me, g, 0) === 'G5LSI M7LSI 73');
  check('E not-to-me -> null', F.suggestReply('2E0ABC G5LSI IO91', me, g, 0) === null);
})();

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
