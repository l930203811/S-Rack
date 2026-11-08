// 列出 0x2f4400~0x345000 和 0x1ed300~0x235000 区间所有 def，找假 def
'use strict';
const fs = require('fs');
const file = 'C:/ProgramData/Acon Digital/AIModels/VocalsSeparation.hance';
const buf = fs.readFileSync(file);
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);

const NAMES = new Set();
for (let i = 0; i + 4 < buf.length; i++) {
  const n = dv.getUint32(i, true);
  if (n >= 3 && n <= 24 && i + 4 + n <= buf.length) {
    let ok = true;
    for (let j = 0; j < n; j++) { const c = buf[i + 4 + j]; if (c < 0x20 || c > 0x7e) { ok = false; break; } }
    if (ok) { const s = buf.slice(i + 4, i + 4 + n).toString('latin1'); if (/^[A-Za-z_][A-Za-z0-9_.]*$/.test(s)) NAMES.add(s); }
  }
}
const TAGS = new Set([0x02, 0x03, 0x05, 0x06, 0x08, 0x10, 0x11, 0x13]);

const ranges = [[0x1ed300, 0x235000], [0x2f4400, 0x345000]];
for (let i = 0; i + 4 < buf.length; i++) {
  if (i < ranges[0][0] || (i > ranges[0][1] && i < ranges[1][0]) || i > ranges[1][1]) continue;
  const n = dv.getUint32(i, true);
  if (n >= 3 && n <= 24 && i + 4 + n <= buf.length) {
    let ok = true;
    for (let j = 0; j < n; j++) { const c = buf[i + 4 + j]; if (c < 0x20 || c > 0x7e) { ok = false; break; } }
    if (!ok) continue;
    const s = buf.slice(i + 4, i + 4 + n).toString('latin1');
    if (!NAMES.has(s)) continue;
    const t = i + 4 + n + 2 <= buf.length ? dv.getUint16(i + 4 + n, true) : -1;
    const t32 = i + 4 + n + 4 <= buf.length ? dv.getUint32(i + 4 + n, true) : -1;
    const isDef = TAGS.has(t) && !(t32 >= 3 && t32 <= 24);
    console.log(`@${i.toString(16).padStart(6, '0')} ${s.padEnd(10)} nextU16=0x${(t>>>0).toString(16)} nextU32=0x${(t32>>>0).toString(16)} ${isDef ? 'DEF' : 'ref'}`);
  }
}
