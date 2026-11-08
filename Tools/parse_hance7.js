// .hance 解析器 v7 —— 修正 def/ref 判别：
//   名字后 u32 = 合法len+已知名  -> ref（属于前一 def 的 ref 链）
//   否则 u16 ∈ TAGS 且 tag 后紧跟合法 ref -> def
//   张量: 01=int, 02=f32 rank2, 03=f32 rank3, 04=f32 rank4
'use strict';
const fs = require('fs');
const file = process.argv[2] || 'C:/ProgramData/Acon Digital/AIModels/VocalsSeparation.hance';
const buf = fs.readFileSync(file);
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);

// 名字集合
const NAMES = new Set();
for (let i = 0; i + 4 < buf.length; i++) {
  const n = dv.getUint32(i, true);
  if (n >= 3 && n <= 24 && i + 4 + n <= buf.length) {
    let ok = true;
    for (let j = 0; j < n; j++) { const c = buf[i + 4 + j]; if (c < 0x20 || c > 0x7e) { ok = false; break; } }
    if (ok) { const s = buf.slice(i + 4, i + 4 + n).toString('latin1'); if (/^[A-Za-z_][A-Za-z0-9_.]*$/.test(s)) NAMES.add(s); }
  }
}
function nameAt(p) { // p 处是否为合法 len+已知名，返回名字或 null
  if (p + 4 > buf.length) return null;
  const n = dv.getUint32(p, true);
  if (n < 3 || n > 24 || p + 4 + n > buf.length) return null;
  const s = buf.slice(p + 4, p + 4 + n).toString('latin1');
  return NAMES.has(s) ? s : null;
}
const TAGS = new Set([0x02, 0x03, 0x05, 0x06, 0x08, 0x10, 0x11, 0x13]);

// 收集条目并分类
const entries = [];
for (let i = 0; i + 4 < buf.length; i++) {
  const name = nameAt(i);
  if (!name) continue;
  const nameEnd = i + 4 + name.length;
  // 规则1: 名字后紧跟 len+名 -> 本条是 ref
  if (nameAt(nameEnd)) { entries.push({ off: i, name, end: nameEnd, kind: 'ref' }); continue; }
  // 规则2: u16 ∈ TAGS 且 tag 后紧跟 len+名 -> def
  if (nameEnd + 2 > buf.length) { entries.push({ off: i, name, end: nameEnd, kind: 'ref' }); continue; }
  const t = dv.getUint16(nameEnd, true);
  if (TAGS.has(t) && nameAt(nameEnd + 2)) entries.push({ off: i, name, end: nameEnd, kind: 'def', tag: t });
  else entries.push({ off: i, name, end: nameEnd, kind: 'ref' });
}
const defs = entries.filter(e => e.kind === 'def');
console.error(`entries=${entries.length} defs=${defs.length}`);

function tryTensor(from, to) {
  if (from >= to) return null;
  const t = buf[from];
  let rank;
  if (t === 0x01) {
    if (from + 5 > to) return null;
    const cnt = dv.getUint32(from + 1, true);
    if (cnt < 1 || cnt > 100000 || from + 5 + cnt * 4 > to) return null;
    return { type: 'int', dims: [cnt], dataoff: from + 5, count: cnt, next: from + 5 + cnt * 4 };
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
  return { type: 'f32', dims, dataoff: from + 1 + rank * 4, count: prod, next: from + 1 + rank * 4 + prod * 4 };
}

const all = [];
let totalParams = 0;
for (let i = 0; i < defs.length; i++) {
  const nd = defs[i];
  let p = nd.end + 2;
  const refs = [];
  let r;
  while ((r = nameAt(p)) !== null) { refs.push(r); p += 4 + r.length; }
  const gapEnd = i + 1 < defs.length ? defs[i + 1].off : buf.length;
  while (p < gapEnd) {
    const t = tryTensor(p, gapEnd);
    if (t) {
      all.push({ owner: nd.name, tag: nd.tag, refs: refs.join(','), type: t.type, dims: t.dims, off: p, count: t.count });
      totalParams += t.count;
      p = t.next;
    } else p++;
  }
}

console.log(`file=0x${buf.length.toString(16)} defs=${defs.length} tensors=${all.length} totalParams=${totalParams}`);
for (const t of all)
  console.log(`@${t.off.toString(16).padStart(6, '0')} ${t.owner.padEnd(10)} tag=0x${t.tag.toString(16).padStart(4, '0')} refs=[${t.refs}] ${t.type}[${t.dims.join(',')}] count=${t.count}`);

if (process.argv.includes('--out')) {
  const oi = process.argv.indexOf('--out');
  const base = process.argv[oi + 1];
  const ws = new Float32Array(totalParams);
  let w = 0;
  const meta = [];
  for (const t of all) {
    const hdr = t.type === 'int' ? 5 : 1 + t.dims.length * 4;
    for (let k = 0; k < t.count; k++) {
      ws[w + k] = t.type === 'f32' ? dv.getFloat32(t.off + hdr + k * 4, true)
                                    : dv.getUint32(t.off + hdr + k * 4, false);
    }
    meta.push({ owner: t.owner, tag: t.tag, refs: t.refs, type: t.type, dims: t.dims, offset: w, count: t.count, fileOff: t.off });
    w += t.count;
  }
  fs.writeFileSync(base + '.bin', Buffer.from(ws.buffer));
  fs.writeFileSync(base + '.json', JSON.stringify(meta, null, 1));
  console.log(`\nwrote ${base}.bin (${totalParams} f32) + ${base}.json (${meta.length} tensors)`);
}
