// .hance 解析器 v4 —— 提取全部节点定义 + 所有权重张量清单
// 已确认格式：
//   节点 = [u32 len][name][u16 tag][refs: u32 len+name ×N][可选: 子结构(u16 op/标记 + 更多 refs + 张量)]
//   f32 张量 = [04][u32 rank][u32 dims×rank][f32×prod] (LE)
//   int 张量 = [01][u32 count][u32×count] (值大端，prenorm 为 62×8)
// 用法: node parse_hance4.js <model.hance> [--weights out.bin]
'use strict';
const fs = require('fs');
const file = process.argv[2];
const buf = fs.readFileSync(file);
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);

// 已知算符 tag（观察值）
const TAGS = new Set([0x02, 0x03, 0x05, 0x06, 0x08, 0x10, 0x11, 0x13]);

// 1) 扫描全部「u32 len + ASCII 名」条目（len 3~24）
const rawNames = new Set();
for (let i = 0; i + 4 < buf.length; i++) {
  const n = dv.getUint32(i, true);
  if (n >= 3 && n <= 24 && i + 4 + n <= buf.length) {
    let ok = true;
    for (let j = 0; j < n; j++) { const c = buf[i + 4 + j]; if (c < 0x20 || c > 0x7e) { ok = false; break; } }
    if (ok) { const s = buf.slice(i + 4, i + 4 + n).toString('latin1'); if (/^[A-Za-z_][A-Za-z0-9_.]*$/.test(s)) rawNames.add(s); }
  }
}
const NAMES = rawNames;

// 2) 按顺序收集所有名字条目
const entries = [];
for (let i = 0; i + 4 < buf.length; i++) {
  const n = dv.getUint32(i, true);
  if (n >= 3 && n <= 24 && i + 4 + n <= buf.length) {
    let ok = true;
    for (let j = 0; j < n; j++) { const c = buf[i + 4 + j]; if (c < 0x20 || c > 0x7e) { ok = false; break; } }
    if (ok) {
      const s = buf.slice(i + 4, i + 4 + n).toString('latin1');
      if (NAMES.has(s)) entries.push({ off: i, name: s, end: i + 4 + n });
    }
  }
}

// 3) 在一个区间内提取所有 f32 张量（tag 04 + rank + dims + 数据，带合理性校验）
function extractTensors(from, to) {
  const out = [];
  let i = from;
  while (i + 1 < to) {
    if (buf[i] !== 0x04) { i++; continue; }
    const save = i;
    i++;
    if (i + 4 > to) break;
    const rank = dv.getUint32(i, true); i += 4;
    if (rank < 1 || rank > 4) { i = save + 1; continue; }
    const dims = []; let prod = 1;
    let bad = false;
    for (let k = 0; k < rank; k++) {
      if (i + 4 > to) { bad = true; break; }
      const d = dv.getUint32(i, true); i += 4;
      if (d < 1 || d > 4096) { bad = true; break; }
      dims.push(d); prod *= d;
    }
    if (bad || prod < 1 || prod > 2000000 || i + prod * 4 > to) { i = save + 1; continue; }
    const f0 = dv.getFloat32(i, true);
    const fLast = dv.getFloat32(i + (prod - 1) * 4, true);
    // 合理性：权重数值应有界
    if (!isFinite(f0) || !isFinite(fLast) || Math.abs(f0) > 100 || Math.abs(fLast) > 100) { i = save + 1; continue; }
    out.push({ off: save, dims, prod, f0: +f0.toFixed(5), fLast: +fLast.toFixed(5) });
    i += prod * 4;
  }
  return out;
}

// 4) 分类条目：节点定义（名字后紧跟已知 tag u16 且其后是 refs/张量/子结构）vs 引用
//    启发式：定义处后续可走通 [tag][refs*][结构]，引用处紧跟另一个 len+名 或数值。
//    这里只做轻量判定：紧跟 u16 已知 tag → def；否则 ref。
const nodes = [];
const refs = [];
for (const e of entries) {
  const t = e.end + 2 <= buf.length ? dv.getUint16(e.end, true) : -1;
  if (TAGS.has(t)) nodes.push({ ...e, tag: t });
  else refs.push(e);
}

// 5) 相邻定义之间的间隙 → 提取张量
console.log(`file=0x${buf.length.toString(16)} entries=${entries.length} nodeDefs=${nodes.length} refs=${refs.length}`);
console.log('\n=== 节点定义（含 tag 与直接 refs） ===');
for (let k = 0; k < nodes.length; k++) {
  const nd = nodes[k];
  // refs：从 tag 之后连续的 len+名
  let p = nd.end + 2; const rf = [];
  while (p + 4 <= buf.length) {
    const n = dv.getUint32(p, true);
    if (n >= 3 && n <= 24 && p + 4 + n <= buf.length) {
      const s = buf.slice(p + 4, p + 4 + n).toString('latin1');
      if (NAMES.has(s)) { rf.push(s); p += 4 + n; continue; }
    }
    break;
  }
  const next = nodes[k + 1];
  const gapEnd = next ? next.off : buf.length;
  const tensors = extractTensors(p, gapEnd);
  console.log(`@${nd.off.toString(16).padStart(6, '0')} ${nd.name.padEnd(10)} tag=0x${nd.tag.toString(16).padStart(4, '0')} refs=[${rf.join(',')}] tensors=${tensors.map(t => `${t.dims.join('x')}@0x${t.off.toString(16)}`).join(' ') || '-'}`);
}

// 6) 全部张量汇总（含间隙中无法归属的）
console.log('\n=== 全部张量 ===');
let tw = 0;
const all = [];
{
  // 用间隙法重扫全文件（定义到下一个定义之间）
  for (let k = 0; k < nodes.length; k++) {
    const nd = nodes[k];
    let p = nd.end + 2;
    while (p + 4 <= buf.length) {
      const n = dv.getUint32(p, true);
      if (n >= 3 && n <= 24 && p + 4 + n <= buf.length) {
        const s = buf.slice(p + 4, p + 4 + n).toString('latin1');
        if (NAMES.has(s)) { p += 4 + n; continue; }
      }
      break;
    }
    const gapEnd = k + 1 < nodes.length ? nodes[k + 1].off : buf.length;
    for (const t of extractTensors(p, gapEnd)) { t.owner = nd.name; all.push(t); tw += t.prod; }
  }
}
console.log(`tensor count=${all.length} total params=${tw}`);
for (const t of all)
  console.log(`@${t.off.toString(16).padStart(6, '0')} ${t.owner.padEnd(10)} [${t.dims.join('x')}] prod=${t.prod} f0=${t.f0} fLast=${t.fLast}`);

// 7) 可选导出权重
if (process.argv.includes('--weights')) {
  const outIdx = process.argv.indexOf('--weights');
  const outPath = process.argv[outIdx + 1];
  const meta = all.map(t => ({ owner: t.owner, off: t.off, dims: t.dims }));
  fs.writeFileSync(outPath, JSON.stringify({ file, tensors: meta }));
  console.log(`\nweights meta -> ${outPath}`);
}
