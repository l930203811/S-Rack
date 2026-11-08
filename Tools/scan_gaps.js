// scan_gaps.js — 扫描 def 之间的未解析区域，找出小的 f32 数组（可能是 Norm/激活参数）
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
const defs = [];
for (let i = 0; i + 4 < buf.length; i++) {
  const name = nameAt(i);
  if (!name) continue;
  const nameEnd = i + 4 + name.length;
  if (nameAt(nameEnd)) continue;
  if (nameEnd + 2 > buf.length) continue;
  const t = dv.getUint16(nameEnd, true);
  if (TAGS.has(t) && nameAt(nameEnd + 2)) defs.push({ off: i, name, nameEnd, tag: t });
}
// 已知大张量区段（来自 parse_hance7 结果）——手工按 fileOff/dims 排除
const known = [
  [72, 8 + 5], [132, 192 * 4 + 17], [964, 256 * 4 + 9], [1997, 7168 * 4 + 9],
  [30765, 4608 * 4 + 13], [49261, 1024 * 4 + 9], [53366, 7168 * 4 + 9],
  [82134, 30720 * 4 + 17], [205078, 4096 * 4 + 9], [221471, 7168 * 4 + 9],
  [250239, 49152 * 4 + 13], [446911, 16384 * 4 + 9], [512456, 7168 * 4 + 9],
  [541224, 98304 * 4 + 13], [934504, 16384 * 4 + 9], [1000049, 3584 * 4 + 9],
  [1014481, 131072 * 4 + 17], [1538835, 100352 * 4 + 13], [1940256, 3584 * 4 + 9],
  [1954644, 16384 * 4 + 17], [2020285, 65536 * 4 + 9], [2282438, 7168 * 4 + 9],
  [2311209, 196608 * 4 + 13], [3097746, 65536 * 4 + 9], [3359899, 14336 * 4 + 9],
  [3417342, 98304 * 4 + 13], [3810663, 16384 * 4 + 9], [3876208, 14336 * 4 + 9],
  [3933651, 36864 * 4 + 13], [4081212, 4096 * 4 + 9], [4097605, 14336 * 4 + 9],
  [4155048, 9216 * 4 + 13], [4192017, 1024 * 4 + 9], [4196122, 14336 * 4 + 9],
  [4253565, 384 * 4 + 13]
];
for (let i = 0; i < defs.length; i++) {
  const nd = defs[i];
  const nextOff = i + 1 < defs.length ? defs[i + 1].off : buf.length;
  let p = nd.nameEnd + 2;
  const refs = [];
  let r;
  while ((r = nameAt(p)) !== null) { refs.push(r); p += 4 + r.length; }
  // 找出此 def 区段内不属于已知张量的字节
  const segEnd = Math.min(nextOff, nd.nameEnd + 2 + 400);
  const chunks = [];
  let q = p;
  while (q < segEnd) {
    const hit = known.find(k => q >= k[0] && q < k[0] + k[1]);
    if (hit) { q = hit[0] + hit[1]; continue; }
    chunks.push(q); q++;
  }
  if (chunks.length < 8) continue;
  // 尝试把每个 chunk 起点解释为小 f32 数组 [02][1..64][f32]
  const out = [];
  for (const c of chunks) {
    if (buf[c] !== 0x02) continue;
    const d0 = dv.getUint32(c + 1, true), d1 = dv.getUint32(c + 5, true);
    if (d0 >= 1 && d0 <= 64 && d1 >= 1 && d1 <= 4096 && d0 * d1 <= 8192 && c + 9 + d0 * d1 * 4 <= nextOff) {
      const vals = [];
      for (let k = 0; k < Math.min(d0 * d1, 6); k++) vals.push(dv.getFloat32(c + 9 + k * 4, true).toFixed(4));
      out.push(`@0x${c.toString(16)} f32[${d0},${d1}] [${vals.join(' ')}${d0 * d1 > 6 ? ' ...' : ''}]`);
    }
  }
  if (out.length) {
    console.log(`${nd.name} (0x${nd.tag.toString(16)}) refs=[${refs.join(',')}] region 0x${p.toString(16)}..0x${segEnd.toString(16)}`);
    for (const s of out) console.log('   ', s);
  }
}
// 全文件统计所有 [02][d0,d1] 小数组（< 8192）及其前面的名字上下文
console.log('\n=== all small rank2 arrays not near known tensors ===');
const all = [];
for (let c = 0; c + 9 < buf.length; c++) {
  if (buf[c] !== 0x02) continue;
  const d0 = dv.getUint32(c + 1, true), d1 = dv.getUint32(c + 5, true);
  if (d0 < 1 || d0 > 64 || d1 < 1 || d1 > 4096 || d0 * d1 > 512) continue;
  if (c + 9 + d0 * d1 * 4 > buf.length) continue;
  const inKnown = known.some(k => c >= k[0] - 16 && c < k[0] + k[1] + 16);
  if (inKnown) continue;
  const vals = [];
  for (let k = 0; k < Math.min(d0 * d1, 4); k++) vals.push(dv.getFloat32(c + 9 + k * 4, true).toFixed(4));
  all.push(`@0x${c.toString(16)} f32[${d0},${d1}] [${vals.join(' ')}${d0 * d1 > 4 ? '...' : ''}]`);
}
for (const s of all.slice(0, 60)) console.log(s);
console.log('total small arrays:', all.length);
