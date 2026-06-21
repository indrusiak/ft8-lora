// Copyright 2026 Leandro Soares Indrusiak (G5LSI)
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// ft8.js - the FT8-over-LoRa protocol brain.
//
// This is pure logic, no DOM and no radio. It is used verbatim inside the web
// page AND by the Node test in ../test, exactly like decoders.js was in the
// sniffer. Three jobs:
//
//   1. parseMsg(text)      - classify one FT8 message string into a typed object
//   2. rssiToReport(dbm)   - map a LoRa RSSI (dBm) to an FT8 report in [-23,+23]
//   3. a small QSO engine  - given my call/grid and the running context, decide
//                            what reply belongs in the TX field, and notice when
//                            a QSO has completed (so it can be logged).
//
// What "FT8 over LoRa" means here: we keep the FT8 *message grammar* and the
// *QSO sequence* (CQ -> call -> report -> roger -> RR73/73, Maidenhead grids,
// signed dB reports) but the bytes ride inside an ordinary LoRa packet as ASCII,
// and the report we hand out is synthesised from LoRa RSSI rather than recovered
// from an FT8 soft-decode. None of the 8-FSK / 15 s slot / LDPC machinery of
// real FT8 is involved; the SX1262 cannot make that waveform.

(function (root) {
  'use strict';

  // ---- field tests ---------------------------------------------------------
  // Maidenhead: 2 letters A-R, 2 digits, optional 2 letters (sub-square).
  const GRID_RE = /^[A-R]{2}[0-9]{2}([A-X]{2})?$/i;
  // A callsign is loosely "has a digit, alnum/slash, 3-10 chars" - good enough to
  // separate calls from grids/reports/keywords without a full ITU validator.
  const CALL_RE = /^[A-Z0-9/]{3,10}$/i;

  const isGrid   = (t) => GRID_RE.test(t);
  const isReport = (t) => /^[-+]\d{1,2}$/.test(t);          // -12, +05, -1
  const isRReport= (t) => /^R[-+]\d{1,2}$/i.test(t);        // R-12, R+05
  // A call has a letter and a digit, fits the charset, and is NOT grid-shaped
  // (a bare "IO82" is a grid, never a callsign).
  const isCall    = (t) => CALL_RE.test(t) && /\d/.test(t) && /[A-Z]/i.test(t) && !isGrid(t);

  // ---- RSSI -> FT8 report --------------------------------------------------
  // LoRa RSSI runs from roughly the noise floor (~-120 dBm on a quiet 70 cm
  // channel with this modem) up to ~-28 dBm for a strong nearby transmitter.
  // Map that span linearly onto the FT8 report range,
  // [-23, +23] (~2 dB of RSSI per report unit), and clamp the ends.
  const RSSI_LO = -120;   // -> -23
  const RSSI_HI = -28;    // -> +23
  function rssiToReport(dbm) {
    if (dbm === null || dbm === undefined || isNaN(dbm)) return 0;
    let r = Math.round(((dbm - RSSI_LO) / (RSSI_HI - RSSI_LO)) * 46) - 23;
    if (r < -23) r = -23;
    if (r > 23) r = 23;
    return r;
  }
  // Format a report the FT8 way: sign always shown, magnitude zero-padded to 2.
  function fmtReport(r) {
    const s = r < 0 ? '-' : '+';
    return s + String(Math.abs(r)).padStart(2, '0');
  }

  // ---- message parser ------------------------------------------------------
  // Returns one of:
  //   {kind:'CQ',      call, grid?}                      e.g. "CQ G5LSI IO82"
  //   {kind:'grid',    to, from, grid}                   e.g. "G5LSI M7NSE IO93"
  //   {kind:'report',  to, from, report}                 e.g. "M7NSE G5LSI -12"
  //   {kind:'Rreport', to, from, report}                 e.g. "G5LSI M7NSE R-08"
  //   {kind:'RR73'|'RRR'|'73', to, from}                 e.g. "M7NSE G5LSI RR73"
  //   {kind:'other',   to?, from?, text}                 anything else
  function parseMsg(text) {
    const toks = String(text || '').trim().toUpperCase().split(/\s+/).filter(Boolean);
    if (!toks.length) return { kind: 'other', text: '' };

    if (toks[0] === 'CQ') {
      // Forms: "CQ CALL GRID", "CQ DX CALL GRID", "CQ POTA CALL GRID", "CQ CALL".
      // Find the call (token containing a digit) and an optional trailing grid.
      let grid = null, call = null;
      if (toks.length >= 2 && isGrid(toks[toks.length - 1])) grid = toks[toks.length - 1];
      for (let i = 1; i < toks.length; i++) {
        if (toks[i] === grid) continue;
        if (isCall(toks[i])) { call = toks[i]; break; }
      }
      return { kind: 'CQ', call, grid };
    }

    // Directed: "<to> <from> <rest...>"
    const to = toks[0], from = toks[1];
    const rest = toks.slice(2);
    const base = { to, from };
    if (!rest.length) return Object.assign({ kind: 'call' }, base);

    const r0 = rest[0];
    if (r0 === 'RR73') return Object.assign({ kind: 'RR73' }, base);
    if (r0 === 'RRR')  return Object.assign({ kind: 'RRR' }, base);
    if (r0 === '73')   return Object.assign({ kind: '73' }, base);
    if (isGrid(r0))    return Object.assign({ kind: 'grid', grid: r0 }, base);
    if (isRReport(r0)) return Object.assign({ kind: 'Rreport', report: parseInt(r0.slice(1), 10) }, base);
    if (isReport(r0))  return Object.assign({ kind: 'report', report: parseInt(r0, 10) }, base);
    return Object.assign({ kind: 'other', text: rest.join(' ') }, base);
  }

  // Convenience: who is this message addressed to? (null for CQ / unparseable)
  function addressedTo(text) {
    const m = parseMsg(text);
    return (m.kind === 'CQ' || m.kind === 'other' && !m.to) ? null : (m.to || null);
  }

  // ---- QSO engine ----------------------------------------------------------
  // A QSO context is a plain object; make a fresh one with newQso().
  function newQso() {
    return {
      partner: null,        // their callsign once known
      theirGrid: null,
      myReportToThem: null,  // dB int I sent (or will send) them
      theirReportToMe: null,  // dB int they sent me
      iCalledCQ: false,     // true if this QSO started from my CQ
      state: 'idle',        // idle|cq|called|reported|rogered|complete
      logged: false,
    };
  }

  // Build the reply to a CQ I clicked on: call them with my grid.
  // Returns { tx, qso } (a fresh QSO context that I am initiating).
  function answerCQ(cqMsg, myCall, myGrid) {
    const m = (typeof cqMsg === 'string') ? parseMsg(cqMsg) : cqMsg;
    if (!m || m.kind !== 'CQ' || !m.call) return null;
    const qso = newQso();
    qso.partner = m.call;
    qso.theirGrid = m.grid || null;
    qso.iCalledCQ = false;
    qso.state = 'called';
    return { tx: `${m.call} ${myCall} ${myGrid}`.toUpperCase(), qso };
  }

  // My own CQ. Resets the context to "waiting for an answer".
  function callCQ(myCall, myGrid) {
    const qso = newQso();
    qso.iCalledCQ = true;
    qso.state = 'cq';
    return { tx: `CQ ${myCall} ${myGrid}`.toUpperCase(), qso };
  }

  // Update the context from a message *I* just transmitted, so that an incoming
  // reply can be recognised as belonging to this QSO. Mutates and returns qso.
  function onTx(qso, text, myCall) {
    const m = parseMsg(text);
    myCall = (myCall || '').toUpperCase();
    if (m.kind === 'CQ') {
      qso.iCalledCQ = true; qso.partner = null; qso.state = 'cq'; qso.logged = false;
      qso.myReportToThem = qso.theirReportToMe = null; qso.theirGrid = null;
      return qso;
    }
    if (m.from === myCall && m.to) {
      // A directed message from me to "m.to".
      if (qso.partner && qso.partner !== m.to) {
        // New partner -> start a fresh QSO context around it.
        const fresh = newQso(); Object.assign(qso, fresh);
      }
      qso.partner = m.to;
      if (m.kind === 'grid')        { qso.state = 'called'; }
      else if (m.kind === 'report') { qso.myReportToThem = m.report; qso.state = 'reported'; }
      else if (m.kind === 'Rreport'){ qso.myReportToThem = m.report; qso.state = 'rogered'; }
      else if (m.kind === 'RR73' || m.kind === 'RRR') { qso.state = 'sentRR73'; }
      else if (m.kind === '73')     { qso.state = 'complete'; }
    }
    return qso;
  }

  // A message arrived. Decide whether it belongs to my QSO and, if so, what to
  // put in the TX field next. Mutates qso. Returns:
  //   { fill, complete, record }
  //     fill     - suggested TX string, or null (nothing to auto-fill)
  //     complete - true when this RX completed the QSO
  //     record   - a QSO log record when complete (else null)
  // myReportFromRssi is the report I computed from THIS rx's RSSI (their signal).
  function onRx(qso, text, myCall, myGrid, myReportFromRssi, opts) {
    opts = opts || {};
    const m = parseMsg(text);
    myCall = (myCall || '').toUpperCase();

    // Only react to traffic addressed to me.
    if (m.kind === 'CQ' || !m.to || m.to !== myCall) return { fill: null, complete: false, record: null };

    const from = m.from;
    // Adopt a partner if I'm CQing (anyone may answer) or I have none yet.
    if (!qso.partner) {
      if (qso.iCalledCQ || qso.state === 'idle' || qso.state === 'cq') qso.partner = from;
    }
    // If mid-QSO with someone else, ignore a third party (teaching-simple).
    if (qso.partner && qso.partner !== from) return { fill: null, complete: false, record: null };
    qso.partner = qso.partner || from;

    const R = (myReportFromRssi === undefined || myReportFromRssi === null) ? 0 : myReportFromRssi;
    let fill = null, complete = false, record = null;

    switch (m.kind) {
      case 'grid':                       // they answered my CQ (or called me) w/ grid
        qso.theirGrid = m.grid;
        qso.myReportToThem = R;
        qso.state = 'reported';
        fill = `${from} ${myCall} ${fmtReport(R)}`;
        break;
      case 'report':                     // they reported me, no roger yet
        qso.theirReportToMe = m.report;
        qso.myReportToThem = R;
        qso.state = 'rogered';
        fill = `${from} ${myCall} R${fmtReport(R)}`;
        break;
      case 'Rreport':                    // they rogered + reported -> I close with RR73
        qso.theirReportToMe = m.report;
        qso.state = 'sentRR73';
        fill = `${from} ${myCall} RR73`;
        complete = true;                 // both reports known; this closes it
        break;
      case 'RRR':
      case 'RR73':                       // they are closing -> I answer 73
        qso.state = 'complete';
        fill = `${from} ${myCall} 73`;
        complete = true;
        break;
      case '73':                         // bare 73 -> QSO done, nothing required
        qso.state = 'complete';
        complete = true;
        break;
      default:
        return { fill: null, complete: false, record: null };
    }

    if (complete && !qso.logged) {
      qso.logged = true;
      record = makeRecord(qso, myCall, myGrid, opts);
    } else if (complete) {
      complete = false;                  // already logged; don't double-count
    }
    return { fill, complete, record };
  }

  // Stateless: "what should I put in the TX field in response to THIS message?"
  // Used by click-to-respond. Does not touch any QSO context or the log; the
  // stateful onRx() remains the authority for the running QSO + completion.
  function suggestReply(text, myCall, myGrid, report) {
    const m = parseMsg(text);
    myCall = (myCall || '').toUpperCase();
    myGrid = (myGrid || '').toUpperCase();
    const R = (report === undefined || report === null) ? 0 : report;
    if (m.kind === 'CQ') return m.call ? `${m.call} ${myCall} ${myGrid}` : null;
    if (m.to !== myCall) return null;
    const from = m.from;
    switch (m.kind) {
      case 'grid':    return `${from} ${myCall} ${fmtReport(R)}`;
      case 'report':  return `${from} ${myCall} R${fmtReport(R)}`;
      case 'Rreport': return `${from} ${myCall} RR73`;
      case 'RRR':
      case 'RR73':    return `${from} ${myCall} 73`;
      default:        return null;     // bare 73 / call / other -> nothing to send
    }
  }

  // Assemble a log record from the finished context. dateTime defaults to now.
  function makeRecord(qso, myCall, myGrid, opts) {
    opts = opts || {};
    const d = opts.date ? new Date(opts.date) : new Date();
    const pad = (n, w) => String(n).padStart(w, '0');
    const qso_date = pad(d.getUTCFullYear(), 4) + pad(d.getUTCMonth() + 1, 2) + pad(d.getUTCDate(), 2);
    const time_on  = pad(d.getUTCHours(), 2) + pad(d.getUTCMinutes(), 2) + pad(d.getUTCSeconds(), 2);
    return {
      call: qso.partner || '',
      gridsquare: qso.theirGrid || '',
      rst_sent: qso.myReportToThem !== null ? fmtReport(qso.myReportToThem) : '',
      rst_rcvd: qso.theirReportToMe !== null ? fmtReport(qso.theirReportToMe) : '',
      qso_date, time_on,
      freq: opts.freq || null,
      mycall: (myCall || '').toUpperCase(),
      mygrid: (myGrid || '').toUpperCase(),
    };
  }

  const api = {
    GRID_RE, CALL_RE, isGrid, isReport, isRReport, isCall,
    RSSI_LO, RSSI_HI, rssiToReport, fmtReport,
    parseMsg, addressedTo,
    newQso, answerCQ, callCQ, onTx, onRx, suggestReply, makeRecord,
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.FT8 = api;
})(typeof self !== 'undefined' ? self : this);
