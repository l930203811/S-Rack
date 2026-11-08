// dump 大数组头部字节，对比 attention 节点的张量头格式
'use strict';
const fs = require('fs');
const file = 'C:/ProgramData/Acon Digital/AIModels/VocalsSeparation.hance';
const buf = fs.readFileSync(file);
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);

function hex(off, len) {
  let s = [];
  for (let i = off; i < Math.min(off + len, buf.length); i++) s.push(buf[i].toString(16).padStart(2, '0'));
  return s.join(' ');
}

// 已知节点地址（来自 parse_hance4 输出，需重新定位）
// 定位方式：扫描所有 len+name 条目
function findNodes() {
  const out = [];
  for (let i = 0; i + 4 < buf.length; i++) {
    const n = dv.getUint32(i, true);
    if (n >= 3 && n <= 24 && i + 4 + n <= buf.length) {
      let ok = true;
      for (let j = 0; j < n; j++) { const c = buf[i + 4 + j]; if (c < 0x20 || c > 0x7e) { ok = false; break; } }
      if (!ok) continue;
      const s = buf.slice(i + 4, i + 4 + n).toString('latin1');
      if (!/^[A-Za-z_][A-Za-z0-9_.]*$/.test(s)) continue;
      const t = i + 4 + n + 2 <= buf.length ? dv.getUint16(i + 4 + n, true) : -1;
      out.push({ off: i, name: s, end: i + 4 + n, tag: t });
    }
  }
  return out;
}

const nodes = findNodes();
// 只看 tag 是已知算符的（定义处），且名字含 att/act
const defs = nodes.filter(n => [0x02, 0x03, 0x05, 0x06, 0x08, 0x10, 0x11, 0x13].includes(n.tag));
const attDefs = defs.filter(n => /att|act|mult/.test(n.name));
console.log(`total entries=${nodes.length} defs=${defs.length}`);

for (const nd of attDefs) {
  // 找 refs 结束点
  let p = nd.end + 2;
  while (p + 4 <= buf.length) {
    const n = dv.getUint32(p, true);
    if (n >= 3 && n <= 24 && p + 4 + n <= buf.length) {
      const s = buf.slice(p + 4, p + 4 + n).toString('latin1');
      if (/^[A-Za-z_][A-Za-z0-9_.]*$/.test(s)) { p += 4 + n; continue; }
    }
    break;
  }
  // 找下一个定义
  const idx = defs.indexOf(nd);
  const nextDef = defs[idx + 1];
  const gapEnd = nextDef ? nextDef.off : buf.length;
  const gap = gapEnd - p;
  console.log(`\n@${nd.off.toString(16).padStart(6, '0')} ${nd.name} tag=0x${nd.tag.toString(16).padStart(4, '0')} tailStart=0x${p.toString(16)} gap=${gap} bytes`);
  console.log('  head:', hex(p, Math.min(gap, 48)));
}
