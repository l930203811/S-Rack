// dump_graph.js — 输出 .hance 的 def/tag/refs/tensors 图结构
'use strict';
const fs = require('fs');
const file = process.argv[2] || 'C:/ProgramData/Acon Digital/AIModels/VocalsSeparation.hance';
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
function nameAt (p) {
  if (p + 4 > buf.length) return null;
  const n = dv.getUint32(p, true);
  if (n < 3 || n > 24 || p + 4 + n > buf.length) return null;
  const s = buf.slice(p + 4, p + 4 + n).toString('latin1');
  return NAMES.has(s) ? s : null;
}
const TAGS = new Set([0x02, 0x03, 0x05, 0x06, 0x08, 0x10, 0x11, 0x13]);
const entries = [];
for (let i = 0; i + 4 < buf.length; i++) {
  const name = nameAt(i);
  if (!name) continue;
  const nameEnd = i + 4 + name.length;
  if (nameAt(nameEnd)) { entries.push({ off: i, name, nameEnd, kind: 'ref' }); continue; }
  if (nameEnd + 2 > buf.length) { entries.push({ off: i, name, nameEnd, kind: 'ref' }); continue; }
  const t = dv.getUint16(nameEnd, true);
  if (TAGS.has(t) && nameAt(nameEnd + 2)) entries.push({ off: i, name, nameEnd, kind: 'def', tag: t });
  else entries.push({ off: i, name, nameEnd, kind: 'ref' });
}
const defs = entries.filter(e => e.kind === 'def');

function tryTensor (from, to) {
  if (from >= to) return null;
  const t = buf[from];
  let rank;
  if (t === 0x01) {
    if (from + 5 > to) return null;
    const cnt = dv.getUint32(from + 1, true);
    if (cnt < 1 || cnt > 100000 || from + 5 + cnt * 4 > to) return null;
    return { type: 'int', dims: [cnt], next: from + 5 + cnt * 4 };
  }
  if (t === 0x02) rank = 2; else if (t === 0x03) rank = 3; else if (t === 0x04) rank = 4; else return null;
  if (from + 1 + rank * 4 > to) return null;
  const dims = []; let prod = 1;
  for (let k = 0; k < rank; k++) {
    const d = dv.getUint32(from + 1 + k * 4, true);
    if (d < 1 || d > 4096) return null;
    dims.push(d); prod *= d;
  }
  if (prod > 2000000 || from + 1 + rank * 4 + prod * 4 > to) return null;
  const f0 = dv.getFloat32(from + 1 + rank * 4, true);
  const fl = dv.getFloat32(from + 1 + rank * 4 + (prod - 1) * 4, true);
  if (!isFinite(f0) || !isFinite(fl) || Math.abs(f0) > 1000 || Math.abs(fl) > 1000) return null;
  return { type: 'f32', dims, next: from + 1 + rank * 4 + prod * 4 };
}

for (let i = 0; i < defs.length; i++) {
  const nd = defs[i];
  let p = nd.nameEnd + 2;
  const refs = [];
  let r;
  while ((r = nameAt(p)) !== null) { refs.push(r); p += 4 + r.length; }
  const nextOff = i + 1 < defs.length ? defs[i + 1].off : buf.length;
  let p2 = p;
  const tensors = [];
  while (p2 < nextOff) {
    const t = tryTensor(p2, nextOff);
    if (t) { tensors.push(t.type + '[' + t.dims.join(',') + '] @0x' + p2.toString(16)); p2 = t.next; } else p2++;
  }
  console.log(nd.name.padEnd(10), 'tag=0x' + nd.tag.toString(16).padStart(2, '0'),
    'refs=[' + refs.join(',') + ']');
  for (const s of tensors) console.log('           ', s);
}
