// .hance 解析器 v5 —— 完整张量解码（01=int, 02=f32 rank2, 03=f32 rank3, 04=f32 显式rank）
// 输出：全部权重 → weights.bin + weights.json（形状+归属）
'use strict';
const fs = require('fs');
const file = process.argv[2] || 'C:/ProgramData/Acon Digital/AIModels/VocalsSeparation.hance';
const buf = fs.readFileSync(file);
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);

// ---- 扫描已知名集合 ----
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

// ---- 收集定义条目（名字+已知tag）与全部名字条目 ----
const entries = [];
for (let i = 0; i + 4 < buf.length; i++) {
  const n = dv.getUint32(i, true);
  if (n >= 3 && n <= 24 && i + 4 + n <= buf.length) {
    let ok = true;
    for (let j = 0; j < n; j++) { const c = buf[i + 4 + j]; if (c < 0x20 || c > 0x7e) { ok = false; break; } }
    if (!ok) continue;
    const s = buf.slice(i + 4, i + 4 + n).toString('latin1');
    if (!NAMES.has(s)) continue;
    const t = i + 4 + n + 2 <= buf.length ? dv.getUint16(i + 4 + n, true) : -1;
    entries.push({ off: i, name: s, end: i + 4 + n, tag: t, isDef: TAGS.has(t) });
  }
}
const defs = entries.filter(e => e.isDef);

// ---- 张量解码器：在 [from,to) 尝试解析一个张量，成功返回 {type,dims,dataoff,count,next} ----
function tryTensor(from, to) {
  if (from >= to) return null;
  const t = buf[from];
  if (t === 0x01) { // int 数组（值大端）
    if (from + 5 > to) return null;
    const cnt = dv.getUint32(from + 1, true);
    if (cnt < 1 || cnt > 100000 || from + 5 + cnt * 4 > to) return null;
    return { type: 'int', dims: [cnt], dataoff: from + 5, count: cnt, next: from + 5 + cnt * 4 };
  }
  if (t === 0x02 || t === 0x03) { // f32 固定 rank 2/3
    const rank = t === 0x02 ? 2 : 3;
    if (from + 1 + rank * 4 > to) return null;
    const dims = []; let prod = 1;
    for (let k = 0; k < rank; k++) {
      const d = dv.getUint32(from + 1 + k * 4, true);
      if (d < 1 || d > 4096) return null;
      dims.push(d); prod *= d;
    }
    if (prod > 2000000 || from + 1 + rank * 4 + prod * 4 > to) return null;
    return { type: 'f32', dims, dataoff: from + 1 + rank * 4, count: prod, next: from + 1 + rank * 4 + prod * 4 };
  }
  if (t === 0x04) { // f32 显式 rank
    if (from + 5 > to) return null;
    const rank = dv.getUint32(from + 1, true);
    if (rank < 1 || rank > 4) return null;
    if (from + 5 + rank * 4 > to) return null;
    const dims = []; let prod = 1;
    for (let k = 0; k < rank; k++) {
      const d = dv.getUint32(from + 5 + k * 4, true);
      if (d < 1 || d > 4096) return null;
      dims.push(d); prod *= d;
    }
    if (prod > 2000000 || from + 5 + rank * 4 + prod * 4 > to) return null;
    return { type: 'f32', dims, dataoff: from + 5 + rank * 4, count: prod, next: from + 5 + rank * 4 + prod * 4 };
  }
  return null;
}

function readF32(t, idx) { return dv.getFloat32(t.dataoff + idx * 4, true); }
function validTensor(t) {
  // 合理性：首尾值有限且有界
  if (t.type === 'int') return true;
  const f0 = readF32(t, 0), fl = readF32(t, t.count - 1);
  return isFinite(f0) && isFinite(fl) && Math.abs(f0) < 1000 && Math.abs(fl) < 1000;
}

// ---- 主解析：定义 i 的 tail 区间 [refsEnd, defs[i+1].off)，贪心跳字节找张量 ----
const all = [];
let totalParams = 0;
const skipped = [];
for (let i = 0; i < defs.length; i++) {
  const nd = defs[i];
  // refs
  let p = nd.end + 2;
  const refs = [];
  while (p + 4 <= buf.length) {
    const n = dv.getUint32(p, true);
    if (n >= 3 && n <= 24 && p + 4 + n <= buf.length) {
      const s = buf.slice(p + 4, p + 4 + n).toString('latin1');
      if (NAMES.has(s)) { refs.push(s); p += 4 + n; continue; }
    }
    break;
  }
  const gapEnd = i + 1 < defs.length ? defs[i + 1].off : buf.length;
  // 贪心：逐字节滑动找张量
  while (p < gapEnd) {
    const t = tryTensor(p, gapEnd);
    if (t && validTensor(t)) {
      all.push({ owner: nd.name, tag: nd.tag, type: t.type, dims: t.dims, off: p, count: t.count });
      totalParams += t.count;
      p = t.next;
    } else p++;
  }
}

console.log(`file=0x${buf.length.toString(16)} defs=${defs.length} tensors=${all.length} totalParams=${totalParams}`);
for (const t of all)
  console.log(`@${t.off.toString(16).padStart(6, '0')} ${t.owner.padEnd(10)} tag=0x${t.tag.toString(16).padStart(4, '0')} ${t.type}[${t.dims.join(',')}] count=${t.count}`);

// ---- 导出 ----
if (process.argv.includes('--out')) {
  const oi = process.argv.indexOf('--out');
  const base = process.argv[oi + 1];
  const ws = new Float32Array(totalParams);
  let w = 0;
  const meta = [];
  for (const t of all) {
    if (t.type === 'f32') {
      for (let k = 0; k < t.count; k++) ws[w + k] = readF32(t, k);
    } else {
      for (let k = 0; k < t.count; k++) ws[w + k] = dv.getUint32(t.dataoff + k * 4, false);
    }
    meta.push({ owner: t.owner, tag: t.tag, type: t.type, dims: t.dims, offset: w, count: t.count, fileOff: t.off });
    w += t.count;
  }
  fs.writeFileSync(base + '.bin', Buffer.from(ws.buffer));
  fs.writeFileSync(base + '.json', JSON.stringify(meta, null, 1));
  console.log(`\nwrote ${base}.bin (${totalParams} f32) + ${base}.json`);
}
